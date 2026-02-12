#include <WiFi.h>
#include <PubSubClient.h>
#include <heltec-eink-modules.h>
#include "esp_sleep.h"
#include <cstring>
#include <cstdlib>
#include <ctime>
#include "secrets.h"

// ============================================================
// Freedom Clock - Config
// ============================================================

// Base monthly expense in USD (today)
static constexpr float MONTHLY_EXP_USD = 10000.0f;

// Annual inflation rate (0.02 = 2%)
static constexpr float INFLATION_ANNUAL = 0.02f;

// Deep sleep interval
static constexpr uint64_t SLEEP_MINUTES = 60;

// MQTT topics
static const char* TOPIC_PRICE_USD   = "home/bitcoin/price/usd";
static const char* TOPIC_BALANCE_BTC = "home/bitcoin/wallets/total_btc";

// NTP / TZ (Switzerland)
static const char* NTP_1 = "pool.ntp.org";
static const char* NTP_2 = "time.nist.gov";
static const char* TZ_CH = "CET-1CEST,M3.5.0,M10.5.0/3";

// ============================================================
// Hardware pins (Vision Master E213)
// ============================================================
static constexpr int PIN_EINK_POWER = 45; // E-ink VCC enable
static constexpr int PIN_BAT_ADC    = 7;  // VBAT_Read
static constexpr int PIN_ADC_CTRL   = 46; // ADC_Ctrl gate

// Battery ADC constants
static constexpr float ADC_MAX    = 4095.0f;
static constexpr float ADC_REF_V  = 3.3f;
static constexpr float VBAT_SCALE = 4.9f;

// ============================================================
// RTC persisted data (survives deep sleep)
// ============================================================
RTC_DATA_ATTR char lastPriceUsd[16]   = "--";
RTC_DATA_ATTR char lastBalanceBtc[16] = "--";

RTC_DATA_ATTR float lastBatteryVoltage = 0.0f;
RTC_DATA_ATTR int   lastBatteryPercent = -1;

// ============================================================
// Globals
// ============================================================
WiFiClient espClient;
PubSubClient mqttClient(espClient);
EInkDisplay_VisionMasterE213 display;

// Incoming MQTT values stored in fixed buffers (avoid String heap churn)
static volatile bool gotPrice   = false;
static volatile bool gotBalance = false;

static char priceUsdBuf[16]   = "";
static char balanceBtcBuf[16] = "";

// ============================================================
// Utilities
// ============================================================

static float clampNonNegative(float v) {
  return (v < 0.0f) ? 0.0f : v;
}

static float parseFloatSafe(const char* s) {
  if (!s || !*s) return 0.0f;
  char* end = nullptr;
  float v = strtof(s, &end);
  if (end == s) return 0.0f; // no parse
  return clampNonNegative(v);
}

static void safeCopy(char* dst, size_t dstSize, const char* src, size_t srcLen) {
  if (!dst || dstSize == 0) return;
  size_t n = (srcLen < (dstSize - 1)) ? srcLen : (dstSize - 1);
  if (src && srcLen > 0) memcpy(dst, src, n);
  dst[n] = '\0';
}

static String makeTimestampOrFallback() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 1500 /*ms*/)) return "--";
  char buffer[20]; // "YYYY-MM-DD HH:MM"
  strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M", &timeinfo);
  return String(buffer);
}

// ============================================================
// Battery helpers
// ============================================================

static int batteryPercentFromVoltage(float v) {
  static constexpr int   NUM_POINTS = 8;
  static constexpr float voltTable[NUM_POINTS] = { 3.20f, 3.30f, 3.60f, 3.75f, 3.85f, 3.95f, 4.05f, 4.15f };
  static constexpr int   socTable[NUM_POINTS]  = { 0,     2,     25,    50,    70,    90,    97,    100 };

  if (v <= voltTable[0]) return socTable[0];
  if (v >= voltTable[NUM_POINTS - 1]) return socTable[NUM_POINTS - 1];

  for (int i = 0; i < NUM_POINTS - 1; i++) {
    float v1 = voltTable[i];
    float v2 = voltTable[i + 1];
    if (v >= v1 && v <= v2) {
      int soc1 = socTable[i];
      int soc2 = socTable[i + 1];
      float t = (v - v1) / (v2 - v1);
      float soc = soc1 + t * (soc2 - soc1);
      if (soc < 0) soc = 0;
      if (soc > 100) soc = 100;
      return (int)(soc + 0.5f);
    }
  }
  return 0;
}

static float readBatteryVoltage() {
  pinMode(PIN_ADC_CTRL, OUTPUT);
  digitalWrite(PIN_ADC_CTRL, HIGH);
  delay(5);

  pinMode(PIN_BAT_ADC, INPUT);
  int raw = analogRead(PIN_BAT_ADC);

  digitalWrite(PIN_ADC_CTRL, LOW);

  float v_adc  = (raw / ADC_MAX) * ADC_REF_V;
  float v_batt = v_adc * VBAT_SCALE;
  return v_batt;
}

// ============================================================
// Inflation-aware longevity calculation
// ============================================================

static void computeLongevityWithInflation(
  float usdWealth,
  float monthlyExpenseToday,
  float inflationAnnual,
  int &outYears,
  int &outMonths,
  int &outWeeks
) {
  outYears = 0;
  outMonths = 0;
  outWeeks = 0;

  if (usdWealth <= 0.0f || monthlyExpenseToday <= 0.0f) return;
  if (inflationAnnual < 0.0f) inflationAnnual = 0.0f;

  // Convert annual inflation to daily multiplier (compounded daily)
  const float dailyMul = powf(1.0f + inflationAnnual, 1.0f / 365.0f);

  // Approximate daily expense from monthly
  const float dailyExpenseBase = monthlyExpenseToday / 30.0f;

  float remaining = usdWealth;
  float dailyExpense = dailyExpenseBase;

  int days = 0;
  static constexpr int MAX_DAYS = 365 * 200; // 200 years cap

  while (days < MAX_DAYS) {
    if (remaining < dailyExpense) break;
    remaining -= dailyExpense;
    dailyExpense *= dailyMul;
    days++;
  }

  // Convert days -> years/months/weeks (simple approximation)
  outYears = days / 365;
  int remDays = days % 365;

  outMonths = remDays / 30;
  remDays = remDays % 30;
  outWeeks = remDays / 7;
}

// ============================================================
// Display (NO BTC balance / NO USD wealth shown)
// ============================================================

static void drawFreedomClock(
  int years,
  int months,
  int weeks,
  float btcPriceUsd,
  float vbat,
  int pct,
  const String& timestamp
) {
  display.clear();
  display.setRotation(1);
  display.setTextColor(BLACK);

  // Title
  display.setTextSize(1);
  display.setCursor(10, 8);
  display.println("FREEDOM CLOCK");

  // Big numbers
  display.setTextSize(4);
  display.setCursor(10, 42);
  display.print(years);
  display.print("Y");

  display.setCursor(110, 42);
  display.print(months);
  display.print("M");

  display.setCursor(185, 42);
  display.print(weeks);
  display.print("W");

  // Footer: BTC price
  display.setTextSize(1);
  display.setCursor(10, 90);
  display.print((long)btcPriceUsd);
  display.print(" (BTC price USD)");

  // Footer: Battery
  display.setCursor(10, 100);
  if (pct >= 0) {
    display.print(pct);
    display.print("% (Battery)");
  } else {
    display.print("-- (Battery)");
  }

  // Footer: Last update
  display.setCursor(10, 110);
  display.print(timestamp);
  display.print(" (Last update)");

  display.update();
}

// ============================================================
// MQTT callback
// ============================================================

static void mqttCallback(char* topic, byte* payload, unsigned int length) {
  if (!topic || !payload || length == 0) return;

  if (strcmp(topic, TOPIC_PRICE_USD) == 0) {
    safeCopy(priceUsdBuf, sizeof(priceUsdBuf), (const char*)payload, length);
    gotPrice = true;
    return;
  }

  if (strcmp(topic, TOPIC_BALANCE_BTC) == 0) {
    safeCopy(balanceBtcBuf, sizeof(balanceBtcBuf), (const char*)payload, length);
    gotBalance = true;
    return;
  }
}

// ============================================================
// WiFi + MQTT
// ============================================================

static bool connectWiFi(uint16_t timeout_ms = 15000) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < timeout_ms) {
    delay(250);
  }
  return WiFi.status() == WL_CONNECTED;
}

static bool connectMQTT(uint16_t timeout_ms = 5000) {
  mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);

  uint32_t start = millis();
  while (!mqttClient.connected() && (millis() - start) < timeout_ms) {
    if (mqttClient.connect("VM_E213_FreedomClock", MQTT_USER, MQTT_PASS)) break;
    delay(500);
  }
  if (!mqttClient.connected()) return false;

  mqttClient.subscribe(TOPIC_PRICE_USD);
  mqttClient.subscribe(TOPIC_BALANCE_BTC);
  return true;
}

// ============================================================
// Deep sleep
// ============================================================

static void goToSleep() {
  digitalWrite(PIN_EINK_POWER, LOW);
  uint64_t sleep_us = SLEEP_MINUTES * 60ULL * 1000000ULL;
  esp_sleep_enable_timer_wakeup(sleep_us);
  esp_deep_sleep_start();
}

// ============================================================
// Setup
// ============================================================

void setup() {
  // Power e-ink
  pinMode(PIN_EINK_POWER, OUTPUT);
  digitalWrite(PIN_EINK_POWER, HIGH);
  delay(100);

  display.begin();

  // Battery
  float vbat = readBatteryVoltage();
  int pct = batteryPercentFromVoltage(vbat);
  lastBatteryVoltage = vbat;
  lastBatteryPercent = pct;

  // Network
  bool wifiOK = connectWiFi();
  String timestamp = "--";
  if (wifiOK) {
    configTime(0, 0, NTP_1, NTP_2);
    setenv("TZ", TZ_CH, 1);
    tzset();
    timestamp = makeTimestampOrFallback();
  }

  // MQTT
  bool mqttOK = false;
  gotPrice = false;
  gotBalance = false;
  priceUsdBuf[0] = '\0';
  balanceBtcBuf[0] = '\0';

  if (wifiOK) mqttOK = connectMQTT();

  if (mqttOK) {
    uint32_t start = millis();
    while (millis() - start < 4000) {
      mqttClient.loop();
      delay(10);
      if (gotPrice && gotBalance) break;
    }
  }

  // Fallbacks from RTC if needed
  const char* priceStr   = gotPrice   ? priceUsdBuf   : lastPriceUsd;
  const char* balanceStr = gotBalance ? balanceBtcBuf : lastBalanceBtc;

  float priceUsd   = parseFloatSafe(priceStr);
  float balanceBtc = parseFloatSafe(balanceStr);

  // Sell BTC today -> USD wealth
  float usdWealth = balanceBtc * priceUsd;

  // Longevity with inflation
  int years = 0, months = 0, weeks = 0;
  computeLongevityWithInflation(usdWealth, MONTHLY_EXP_USD, INFLATION_ANNUAL, years, months, weeks);

  drawFreedomClock(years, months, weeks, priceUsd, vbat, pct, timestamp);

  // Persist last known values
  if (gotPrice) {
    strncpy(lastPriceUsd, priceUsdBuf, sizeof(lastPriceUsd) - 1);
    lastPriceUsd[sizeof(lastPriceUsd) - 1] = '\0';
  }
  if (gotBalance) {
    strncpy(lastBalanceBtc, balanceBtcBuf, sizeof(lastBalanceBtc) - 1);
    lastBalanceBtc[sizeof(lastBalanceBtc) - 1] = '\0';
  }

  // Clean shutdown
  if (mqttClient.connected()) mqttClient.disconnect();
  WiFi.disconnect(true);

  goToSleep();
}

void loop() {
  // not used
}
