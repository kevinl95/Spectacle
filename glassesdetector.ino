/*
 * Spectacle - Smart Glasses BLE Scanner for M5Stack StickS3
 * 
 * Passively scans for BLE advertisements from known smart glasses
 * (Meta Ray-Ban, Snap Spectacles, etc.) and alerts the user.
 * 
 * Detection logic:
 *   1. Match manufacturer company ID from BLE advertisement
 *   2. Optionally match a payload string in manufacturer data
 *      (e.g., Ray-Bans broadcast "META_RB_GLASS" in their payload)
 *   3. Optionally match BLE device name prefix
 * 
 * Configuration is loaded from /config.json on SPIFFS.
 * 
 * Hardware: M5Stack StickS3 (ESP32-S3)
 * License: MIT
 */

#include <M5Unified.h>
#include <M5PM1.h>
#include <NimBLEDevice.h>
#include <ArduinoJson.h>
#include <SPIFFS.h>
#include <driver/gpio.h>
#include <esp_sleep.h>

// ── Structs ──────────────────────────────────────────────────────────────────

struct DeviceSignature {
  String   id;
  String   label;
  bool     has_camera;
  uint16_t manufacturer_ids[4];
  uint8_t  mfr_id_count;
  String   payload_strings[4];   // strings to find in manufacturer data payload
  uint8_t  payload_str_count;
  String   name_prefixes[4];
  uint8_t  name_prefix_count;
};

struct Detection {
  String   label;
  bool     has_camera;
  int      rssi;
  uint32_t timestamp;
};

struct ScanConfig {
  uint32_t window_ms;
  uint32_t interval_ms;
  int      rssi_threshold;
  uint32_t display_timeout_ms;
  uint32_t lcd_on_ms;
  uint8_t  lcd_brightness;
  uint8_t  lcd_brightness_dim;
};

// ── Globals ──────────────────────────────────────────────────────────────────

static DeviceSignature   g_signatures[16];
static uint8_t           g_signature_count = 0;
static ScanConfig        g_scan_config;

static Detection         g_detections[8];
static uint8_t           g_detection_count = 0;
static SemaphoreHandle_t g_detection_mutex;

static bool              g_scanning = false;
static uint32_t          g_last_detection_time = 0;
static uint32_t          g_scan_count = 0;
static const bool        USE_DEEP_SLEEP_BETWEEN_SCANS = true;

RTC_DATA_ATTR static uint32_t g_rtc_scan_count = 0;
RTC_DATA_ATTR static int      g_rtc_rssi_threshold = -75;
RTC_DATA_ATTR static bool     g_rtc_rssi_threshold_valid = false;

// ── LCD & Power State ────────────────────────────────────────────────────────

static bool              g_lcd_on = true;
static uint32_t          g_lcd_wake_time = 0;
static bool              g_new_detection_flag = false;
static int               g_battery_level = -1;
static int               g_battery_voltage_mv = 0;
static int               g_battery_filtered_mv = 0;
static uint32_t          g_last_battery_sample_time = 0;
static uint32_t          g_next_battery_sample_at = 0;
static bool              g_skip_next_btn_b_press = false;

static esp_sleep_wakeup_cause_t g_boot_wakeup_cause = ESP_SLEEP_WAKEUP_UNDEFINED;
static uint64_t                 g_boot_gpio_wakeup_status = 0;

// ── Display Constants ────────────────────────────────────────────────────────

static const uint16_t COLOR_BG       = TFT_BLACK;
static const uint16_t COLOR_CLEAR    = 0x0400;  // dark green
static const uint16_t COLOR_CAMERA   = 0xF800;  // red - has camera
static const uint16_t COLOR_NOCAM    = 0xFDA0;  // amber - no camera
static const uint16_t COLOR_TEXT     = TFT_WHITE;
static const uint16_t COLOR_DIM      = 0x7BEF;  // gray
static const uint8_t  BATTERY_SAMPLE_COUNT = 4;
static const uint32_t BATTERY_SAMPLE_DELAY_MS = 20;
static const uint32_t BATTERY_SAMPLE_INTERVAL_LCD_ON_MS = 5000;
static const uint32_t BATTERY_SAMPLE_INTERVAL_LCD_OFF_MS = 15000;
static const uint32_t BATTERY_WAKE_SETTLE_MS = 250;
static const uint8_t  BATTERY_FILTER_SHIFT = 3;
static const uint8_t  BATTERY_LEVEL_HYSTERESIS = 2;
static const uint8_t  BATTERY_LEVEL_REBOUND_HYSTERESIS = 4;
static const uint8_t  BATTERY_CHARGE_SNAP_THRESHOLD = 10;

static const gpio_num_t STICKS3_BTN_A_GPIO = GPIO_NUM_11;
static const gpio_num_t STICKS3_BTN_B_GPIO = GPIO_NUM_12;
static const gpio_num_t STICKS3_PM1_IRQ_GPIO = GPIO_NUM_13;

static M5PM1            g_pm1;
static bool             g_pm1_ready = false;

bool pm1CallOk(m5pm1_err_t err, const char* op) {
  if (err == M5PM1_OK) {
    return true;
  }

  Serial.printf("PM1 %s failed: %d\n", op, err);
  return false;
}

bool initPm1ButtonWake() {
  pinMode(STICKS3_PM1_IRQ_GPIO, INPUT_PULLUP);

  if (!pm1CallOk(g_pm1.begin(&M5.In_I2C, M5PM1_DEFAULT_ADDR, M5PM1_I2C_FREQ_100K), "begin")) {
    return false;
  }

  g_pm1.setAutoWakeEnable(true);

  uint8_t wake_src = 0;
  if (g_pm1.getWakeSource(&wake_src, M5PM1_CLEAN_ALL) == M5PM1_OK && wake_src != 0) {
    Serial.printf("PM1 wake source: 0x%02X\n", wake_src);
  }

  bool ok = true;
  ok &= pm1CallOk(g_pm1.irqClearGpioAll(), "irqClearGpioAll");
  ok &= pm1CallOk(g_pm1.irqClearSysAll(), "irqClearSysAll");
  ok &= pm1CallOk(g_pm1.irqClearBtnAll(), "irqClearBtnAll");
  ok &= pm1CallOk(g_pm1.irqSetGpioMaskAll(M5PM1_IRQ_MASK_ENABLE), "irqSetGpioMaskAll");
  ok &= pm1CallOk(g_pm1.irqSetSysMaskAll(M5PM1_IRQ_MASK_ENABLE), "irqSetSysMaskAll");
  ok &= pm1CallOk(g_pm1.irqSetBtnMaskAll(M5PM1_IRQ_MASK_ENABLE), "irqSetBtnMaskAll");
  ok &= pm1CallOk(g_pm1.irqSetBtnMask(M5PM1_IRQ_BTN_CLICK, M5PM1_IRQ_MASK_DISABLE), "irqSetBtnMask(click)");
  ok &= pm1CallOk(g_pm1.irqSetBtnMask(M5PM1_IRQ_BTN_WAKE, M5PM1_IRQ_MASK_DISABLE), "irqSetBtnMask(wake)");
  ok &= pm1CallOk(g_pm1.setSingleResetDisable(true), "setSingleResetDisable");
  ok &= pm1CallOk(g_pm1.gpioSetMode(M5PM1_GPIO_NUM_1, M5PM1_GPIO_MODE_OUTPUT), "gpioSetMode(GPIO1)");
  ok &= pm1CallOk(g_pm1.gpioSetDrive(M5PM1_GPIO_NUM_1, M5PM1_GPIO_DRIVE_PUSHPULL), "gpioSetDrive(GPIO1)");
  ok &= pm1CallOk(g_pm1.gpioSetFunc(M5PM1_GPIO_NUM_1, M5PM1_GPIO_FUNC_IRQ), "gpioSetFunc(GPIO1)");

  if (ok) {
    Serial.println("PM1 front-button IRQ routed to ESP32 GPIO13");
  }

  g_pm1_ready = ok;
  return ok;
}

bool consumePm1ButtonEvent() {
  if (!g_pm1_ready || digitalRead(STICKS3_PM1_IRQ_GPIO) != LOW) {
    return false;
  }

  bool handled = false;
  uint8_t btn_irq = 0;
  if (g_pm1.irqGetBtnStatus(&btn_irq, M5PM1_CLEAN_ALL) == M5PM1_OK && btn_irq != M5PM1_IRQ_BTN_NONE) {
    Serial.printf("PM1 button IRQ: 0x%02X\n", btn_irq);
    handled = true;
  }

  uint8_t wake_src = 0;
  if (g_pm1.getWakeSource(&wake_src, M5PM1_CLEAN_ALL) == M5PM1_OK && (wake_src & M5PM1_WAKE_SRC_PWRBTN)) {
    Serial.println("PM1 power button wake");
    handled = true;
  }

  g_pm1.irqClearBtnAll();
  g_pm1.irqClearGpioAll();
  g_pm1.irqClearSysAll();
  return handled;
}

int batteryLevelFromMillivolts(int battery_mv) {
  struct BatteryCurvePoint {
    int millivolts;
    int percent;
  };

  static const BatteryCurvePoint curve[] = {
    {4200, 100},
    {4160, 95},
    {4120, 90},
    {4080, 80},
    {4010, 70},
    {3970, 60},
    {3920, 50},
    {3870, 40},
    {3830, 30},
    {3790, 20},
    {3750, 12},
    {3700, 6},
    {3600, 2},
    {3300, 0},
  };

  if (battery_mv >= curve[0].millivolts) {
    return curve[0].percent;
  }

  for (size_t i = 1; i < sizeof(curve) / sizeof(curve[0]); i++) {
    if (battery_mv >= curve[i].millivolts) {
      const BatteryCurvePoint& upper = curve[i - 1];
      const BatteryCurvePoint& lower = curve[i];
      int span_mv = upper.millivolts - lower.millivolts;
      if (span_mv <= 0) {
        return lower.percent;
      }

      int offset_mv = battery_mv - lower.millivolts;
      return lower.percent + (offset_mv * (upper.percent - lower.percent)) / span_mv;
    }
  }

  return 0;
}

void updateDisplayedBatteryLevel(int estimated_level, int charge_state) {
  if (g_battery_level < 0) {
    g_battery_level = estimated_level;
    return;
  }

  int diff = estimated_level - g_battery_level;
  if (diff == 0) {
    return;
  }

  if (charge_state == 1 && diff >= BATTERY_CHARGE_SNAP_THRESHOLD) {
    g_battery_level = estimated_level;
    return;
  }

  int threshold = BATTERY_LEVEL_HYSTERESIS;
  if ((charge_state == 0 && diff > 0) || (charge_state == 1 && diff < 0)) {
    threshold = BATTERY_LEVEL_REBOUND_HYSTERESIS;
  }

  if (abs(diff) < threshold) {
    return;
  }

  g_battery_level += (diff > 0) ? 1 : -1;
}

void refreshBatteryStatus(bool force = false) {
  uint32_t now = millis();

  if (!force) {
    if ((int32_t)(now - g_next_battery_sample_at) < 0) {
      return;
    }
    uint32_t sample_interval_ms = g_lcd_on
      ? BATTERY_SAMPLE_INTERVAL_LCD_ON_MS
      : BATTERY_SAMPLE_INTERVAL_LCD_OFF_MS;
    if (now - g_last_battery_sample_time < sample_interval_ms) {
      return;
    }
  }

  int32_t total_mv = 0;
  uint8_t valid_samples = 0;

  for (uint8_t i = 0; i < BATTERY_SAMPLE_COUNT; i++) {
    int sample_mv = M5.Power.getBatteryVoltage();
    if (sample_mv > 0) {
      total_mv += sample_mv;
      valid_samples++;
    }

    if (i + 1 < BATTERY_SAMPLE_COUNT) {
      delay(BATTERY_SAMPLE_DELAY_MS);
    }
  }

  if (valid_samples > 0) {
    int average_mv = total_mv / valid_samples;
    int charge_state = (int)M5.Power.isCharging();

    if (g_battery_filtered_mv <= 0 || force) {
      g_battery_filtered_mv = average_mv;
    } else {
      g_battery_filtered_mv =
        (g_battery_filtered_mv * ((1 << BATTERY_FILTER_SHIFT) - 1) + average_mv) >> BATTERY_FILTER_SHIFT;
    }

    g_battery_voltage_mv = g_battery_filtered_mv;
    updateDisplayedBatteryLevel(batteryLevelFromMillivolts(g_battery_filtered_mv), charge_state);
  } else {
    g_battery_level = M5.Power.getBatteryLevel();
  }

  g_last_battery_sample_time = millis();
}

// ── Config Loading ───────────────────────────────────────────────────────────

uint16_t parseHex16(const char* str) {
  if (str == nullptr) return 0;
  return (uint16_t)strtoul(str, nullptr, 16);
}

bool loadConfig(const char* path) {
  File file = SPIFFS.open(path, "r");
  if (!file) {
    Serial.println("Failed to open config file");
    return false;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, file);
  file.close();

  if (err) {
    Serial.printf("JSON parse error: %s\n", err.c_str());
    return false;
  }

  // Scan settings
  JsonObject scan = doc["scan"];
  g_scan_config.window_ms          = scan["window_ms"] | 3000;
  g_scan_config.interval_ms        = scan["interval_ms"] | 10000;
  g_scan_config.rssi_threshold     = scan["rssi_threshold"] | -75;
  g_scan_config.display_timeout_ms = scan["display_timeout_ms"] | 30000;
  g_scan_config.lcd_on_ms          = scan["lcd_on_ms"] | 5000;
  g_scan_config.lcd_brightness     = scan["lcd_brightness"] | 128;
  g_scan_config.lcd_brightness_dim = scan["lcd_brightness_dim"] | 32;

  // Device signatures
  JsonArray devices = doc["devices"];
  g_signature_count = 0;

  for (JsonObject dev : devices) {
    if (g_signature_count >= 16) break;

    DeviceSignature& sig = g_signatures[g_signature_count];
    sig.id         = dev["id"] | "unknown";
    sig.label      = dev["label"] | "Unknown";
    sig.has_camera = dev["has_camera"] | false;

    // Manufacturer IDs
    sig.mfr_id_count = 0;
    JsonArray mfr_ids = dev["manufacturer_ids"];
    for (const char* hex : mfr_ids) {
      if (sig.mfr_id_count >= 4) break;
      sig.manufacturer_ids[sig.mfr_id_count++] = parseHex16(hex);
    }

    // Payload strings (e.g., "META_RB_GLASS")
    sig.payload_str_count = 0;
    JsonArray payloads = dev["payload_strings"];
    for (const char* ps : payloads) {
      if (sig.payload_str_count >= 4) break;
      sig.payload_strings[sig.payload_str_count++] = ps;
    }

    // Name prefixes
    sig.name_prefix_count = 0;
    JsonArray name_pfx = dev["name_prefixes"];
    for (const char* pfx : name_pfx) {
      if (sig.name_prefix_count >= 4) break;
      sig.name_prefixes[sig.name_prefix_count++] = pfx;
    }

    g_signature_count++;
  }

  Serial.printf("Loaded %d device signatures\n", g_signature_count);
  return true;
}

// ── BLE Scanning ─────────────────────────────────────────────────────────────

// Check if the manufacturer data payload contains a specific string.
// The payload bytes start AFTER the 2-byte company ID.
bool payloadContains(const std::string& mfr_data, const String& search) {
  if (mfr_data.length() < 2 + search.length()) return false;

  // Search in the data portion (after the 2-byte company ID)
  const char* data_start = mfr_data.c_str() + 2;
  size_t data_len = mfr_data.length() - 2;

  // Simple substring search
  const char* needle = search.c_str();
  size_t needle_len = search.length();

  for (size_t i = 0; i <= data_len - needle_len; i++) {
    if (memcmp(data_start + i, needle, needle_len) == 0) {
      return true;
    }
  }
  return false;
}

bool matchDevice(const NimBLEAdvertisedDevice* device, const DeviceSignature& sig) {
  const bool has_matcher =
    sig.mfr_id_count > 0 ||
    sig.payload_str_count > 0 ||
    sig.name_prefix_count > 0;
  if (!has_matcher) return false;

  std::string mfr_data;
  const bool needs_mfr_data = sig.mfr_id_count > 0 || sig.payload_str_count > 0;
  if (needs_mfr_data) {
    if (!device->haveManufacturerData()) return false;
    mfr_data = device->getManufacturerData();
    if (mfr_data.length() < 2) return false;
  }

  // If manufacturer IDs are defined, one of them must match.
  if (sig.mfr_id_count > 0) {
    bool mfr_id_matched = false;
    const uint16_t detected_id = (uint16_t)(uint8_t)mfr_data[0] | ((uint16_t)(uint8_t)mfr_data[1] << 8);

    for (uint8_t i = 0; i < sig.mfr_id_count; i++) {
      if (detected_id == sig.manufacturer_ids[i]) {
        mfr_id_matched = true;
        break;
      }
    }

    if (!mfr_id_matched) return false;
  }

  // If payload strings are defined, at least one must match in manufacturer data.
  if (sig.payload_str_count > 0) {
    bool payload_matched = false;
    for (uint8_t i = 0; i < sig.payload_str_count; i++) {
      if (payloadContains(mfr_data, sig.payload_strings[i])) {
        payload_matched = true;
        break;
      }
    }
    if (!payload_matched) return false;
  }

  // If name prefixes are defined, at least one must match.
  if (sig.name_prefix_count > 0) {
    if (!device->haveName()) return false;
    const String name = device->getName().c_str();
    bool name_matched = false;
    for (uint8_t i = 0; i < sig.name_prefix_count; i++) {
      if (name.startsWith(sig.name_prefixes[i])) {
        name_matched = true;
        break;
      }
    }
    if (!name_matched) return false;
  }

  return true;
}

class ScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* device) override {
    int rssi = device->getRSSI();
    if (rssi < g_scan_config.rssi_threshold) return;

    for (uint8_t s = 0; s < g_signature_count; s++) {
      if (!matchDevice(device, g_signatures[s])) continue;

      xSemaphoreTake(g_detection_mutex, portMAX_DELAY);

      // Check if already detected (update RSSI if stronger)
      bool found = false;
      for (uint8_t d = 0; d < g_detection_count; d++) {
        if (g_detections[d].label == g_signatures[s].label) {
          if (rssi > g_detections[d].rssi) {
            g_detections[d].rssi = rssi;
          }
          g_detections[d].timestamp = millis();
          found = true;
          break;
        }
      }

      // Add new detection
      if (!found && g_detection_count < 8) {
        g_detections[g_detection_count].label      = g_signatures[s].label;
        g_detections[g_detection_count].has_camera  = g_signatures[s].has_camera;
        g_detections[g_detection_count].rssi        = rssi;
        g_detections[g_detection_count].timestamp   = millis();
        g_detection_count++;
        g_new_detection_flag = true;
      }

      g_last_detection_time = millis();

      xSemaphoreGive(g_detection_mutex);
      break;
    }
  }

  void onScanEnd(const NimBLEScanResults& results, int reason) override {
    g_scanning = false;
  }
};

static ScanCallbacks g_scan_callbacks;

void startScan() {
  // Expire old detections
  xSemaphoreTake(g_detection_mutex, portMAX_DELAY);
  uint32_t now = millis();
  for (int i = g_detection_count - 1; i >= 0; i--) {
    if (now - g_detections[i].timestamp > g_scan_config.display_timeout_ms) {
      for (int j = i; j < g_detection_count - 1; j++) {
        g_detections[j] = g_detections[j + 1];
      }
      g_detection_count--;
    }
  }
  xSemaphoreGive(g_detection_mutex);

  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setActiveScan(false);  // passive - don't send scan requests
  scan->setDuplicateFilter(true);
  scan->setScanCallbacks(&g_scan_callbacks, false);
  scan->start(g_scan_config.window_ms / 1000, false);
  g_scanning = true;
  g_scan_count++;
  g_rtc_scan_count = g_scan_count;
}

// ── LCD & Power Management ───────────────────────────────────────────────────

void lcdWake() {
  if (!g_lcd_on) {
    M5.Lcd.wakeup();
    M5.Lcd.setBrightness(g_scan_config.lcd_brightness);
    g_lcd_on = true;
  }
  g_lcd_wake_time = millis();
  g_next_battery_sample_at = g_lcd_wake_time + BATTERY_WAKE_SETTLE_MS;
}

void lcdSleep() {
  if (g_lcd_on) {
    M5.Lcd.setBrightness(0);
    M5.Lcd.sleep();
    g_lcd_on = false;
  }
}

const char* wakeCauseName(esp_sleep_wakeup_cause_t cause) {
  switch (cause) {
    case ESP_SLEEP_WAKEUP_EXT0: return "ext0";
    case ESP_SLEEP_WAKEUP_EXT1: return "ext1";
    case ESP_SLEEP_WAKEUP_TIMER: return "timer";
    case ESP_SLEEP_WAKEUP_TOUCHPAD: return "touchpad";
    case ESP_SLEEP_WAKEUP_ULP: return "ulp";
    case ESP_SLEEP_WAKEUP_GPIO: return "gpio";
    case ESP_SLEEP_WAKEUP_UART: return "uart";
    default: return "other";
  }
}

void cycleRssiThreshold() {
  g_scan_config.rssi_threshold -= 10;
  if (g_scan_config.rssi_threshold < -100) {
    g_scan_config.rssi_threshold = -50;
  }

  g_rtc_rssi_threshold = g_scan_config.rssi_threshold;
  g_rtc_rssi_threshold_valid = true;
  Serial.printf("RSSI threshold: %d\n", g_scan_config.rssi_threshold);
}

void enterLightSleep(uint32_t sleep_ms) {
  lcdSleep();

  if (consumePm1ButtonEvent()) {
    Serial.println("Skipping light sleep due to pending PM1 button event");
    lcdWake();
    return;
  }

  esp_sleep_enable_timer_wakeup((uint64_t)sleep_ms * 1000ULL);
  gpio_wakeup_enable(STICKS3_BTN_A_GPIO, GPIO_INTR_LOW_LEVEL);
  gpio_wakeup_enable(STICKS3_BTN_B_GPIO, GPIO_INTR_LOW_LEVEL);
  if (g_pm1_ready) {
    gpio_wakeup_enable(STICKS3_PM1_IRQ_GPIO, GPIO_INTR_LOW_LEVEL);
  }
  esp_sleep_enable_gpio_wakeup();

  Serial.printf("Light sleep %lums\n", sleep_ms);
  Serial.flush();

  esp_light_sleep_start();

  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  Serial.printf("Wake cause: %s (%d)\n", wakeCauseName(cause), (int)cause);
  if (cause == ESP_SLEEP_WAKEUP_GPIO) {
    consumePm1ButtonEvent();
    lcdWake();
  }
}

void enterDeepSleep(uint32_t sleep_ms) {
  lcdSleep();

  if (consumePm1ButtonEvent()) {
    Serial.println("Skipping deep sleep due to pending PM1 button event");
    lcdWake();
    return;
  }

  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  esp_sleep_enable_timer_wakeup((uint64_t)sleep_ms * 1000ULL);

  uint64_t wake_mask = (1ULL << STICKS3_BTN_A_GPIO) | (1ULL << STICKS3_BTN_B_GPIO);
  if (g_pm1_ready) {
    wake_mask |= (1ULL << STICKS3_PM1_IRQ_GPIO);
  }

  esp_err_t wake_err = esp_sleep_enable_ext1_wakeup(wake_mask, ESP_EXT1_WAKEUP_ANY_LOW);
  if (wake_err != ESP_OK) {
    Serial.printf("Deep sleep ext1 wake setup failed: %d\n", (int)wake_err);
    lcdWake();
    return;
  }

  Serial.printf("Deep sleep %lums\n", sleep_ms);
  Serial.flush();
  esp_deep_sleep_start();
}

// ── Display ──────────────────────────────────────────────────────────────────

void drawStatusBar() {
  M5.Lcd.setTextColor(COLOR_DIM, COLOR_BG);
  M5.Lcd.setTextSize(1);
  M5.Lcd.setCursor(4, 2);
  M5.Lcd.printf("Scan #%lu  RSSI>%d", g_scan_count, g_scan_config.rssi_threshold);
}

void drawClearScreen() {
  M5.Lcd.fillScreen(COLOR_BG);
  drawStatusBar();

  int cx = M5.Lcd.width() / 2;
  int cy = M5.Lcd.height() / 2 - 10;

  M5.Lcd.fillCircle(cx, cy, 24, COLOR_CLEAR);
  M5.Lcd.setTextColor(COLOR_TEXT, COLOR_CLEAR);
  M5.Lcd.setTextSize(1);
  M5.Lcd.setTextDatum(MC_DATUM);
  M5.Lcd.drawString("OK", cx, cy);
  M5.Lcd.setTextDatum(TL_DATUM);

  M5.Lcd.setTextColor(COLOR_DIM, COLOR_BG);
  M5.Lcd.setTextSize(1);
  M5.Lcd.setCursor(cx - 48, cy + 34);
  M5.Lcd.print("No glasses found");

  if (g_scanning) {
    M5.Lcd.setCursor(cx - 30, M5.Lcd.height() - 14);
    M5.Lcd.setTextColor(COLOR_DIM, COLOR_BG);
    M5.Lcd.print("Scanning...");
  }
}

void drawDetectionScreen() {
  M5.Lcd.fillScreen(COLOR_BG);
  drawStatusBar();

  xSemaphoreTake(g_detection_mutex, portMAX_DELAY);

  int y = 16;

  for (uint8_t i = 0; i < g_detection_count && i < 4; i++) {
    Detection& det = g_detections[i];

    // Red for camera-equipped glasses, amber for non-camera
    uint16_t color = det.has_camera ? COLOR_CAMERA : COLOR_NOCAM;

    // Alert indicator bar
    M5.Lcd.fillRect(0, y, 4, 26, color);

    // Device label
    M5.Lcd.setTextColor(color, COLOR_BG);
    M5.Lcd.setTextSize(1);
    M5.Lcd.setCursor(8, y + 2);
    M5.Lcd.print(det.label);

    // RSSI and camera indicator
    M5.Lcd.setTextColor(COLOR_DIM, COLOR_BG);
    M5.Lcd.setCursor(8, y + 15);
    M5.Lcd.printf("%ddBm  %s", det.rssi, det.has_camera ? "CAM" : "NO CAM");

    y += 30;
  }

  xSemaphoreGive(g_detection_mutex);

  if (g_scanning) {
    M5.Lcd.setCursor(M5.Lcd.width() / 2 - 30, M5.Lcd.height() - 14);
    M5.Lcd.setTextColor(COLOR_DIM, COLOR_BG);
    M5.Lcd.print("Scanning...");
  }
}

void alertBuzz() {
  M5.Speaker.begin();
  M5.Speaker.setVolume(64);
  M5.Speaker.tone(2000, 100);
  delay(150);
  M5.Speaker.tone(2000, 100);
  delay(120);
  M5.Speaker.end();
}

// ── Setup & Loop ─────────────────────────────────────────────────────────────

void setup() {
  g_boot_wakeup_cause = esp_sleep_get_wakeup_cause();
  g_boot_gpio_wakeup_status = esp_sleep_get_ext1_wakeup_status();

  const bool woke_from_deep_sleep = g_boot_wakeup_cause != ESP_SLEEP_WAKEUP_UNDEFINED;
  const bool woke_from_timer = g_boot_wakeup_cause == ESP_SLEEP_WAKEUP_TIMER;
  const bool woke_from_ext = g_boot_wakeup_cause == ESP_SLEEP_WAKEUP_EXT1;
  const bool quiet_boot = USE_DEEP_SLEEP_BETWEEN_SCANS && woke_from_timer;

  auto cfg = M5.config();
  M5.begin(cfg);

  if (quiet_boot) {
    M5.Lcd.setBrightness(0);
    M5.Lcd.sleep();
    g_lcd_on = false;
  }

  Serial.begin(115200);
  Serial.println("Spectacle starting...");
  Serial.printf("Boot wake cause: %s (%d)\n", wakeCauseName(g_boot_wakeup_cause), (int)g_boot_wakeup_cause);
  if (g_boot_gpio_wakeup_status != 0) {
    Serial.printf("Boot ext wake status: 0x%llX\n", (unsigned long long)g_boot_gpio_wakeup_status);
  }

  if (!initPm1ButtonWake()) {
    Serial.println("PM1 front-button wake unavailable; side-button wake remains enabled.");
  }

  M5.Lcd.setRotation(1);
  if (!quiet_boot) {
    M5.Lcd.fillScreen(COLOR_BG);
    M5.Lcd.setTextColor(COLOR_TEXT, COLOR_BG);
    M5.Lcd.setTextSize(1);
    M5.Lcd.setCursor(4, 4);
    M5.Lcd.println("Spectacle v0.1");
    M5.Lcd.println("Loading config...");
  }

  if (!SPIFFS.begin(true)) {
    M5.Lcd.println("SPIFFS FAILED");
    M5.Lcd.println("Use factory");
    M5.Lcd.println("reflash");
    Serial.println("SPIFFS mount failed. Reflash bootloader, partitions, firmware, and SPIFFS.");
    while (1) delay(1000);
  }

  if (!loadConfig("/config.json")) {
    M5.Lcd.println("CONFIG FAILED");
    M5.Lcd.println("Flash SPIFFS or");
    M5.Lcd.println("factory reflash");
    Serial.println("Config load failed. Flash the SPIFFS image or run a full factory reflash.");
    while (1) delay(1000);
  }

  g_scan_count = g_rtc_scan_count;
  if (g_rtc_rssi_threshold_valid) {
    g_scan_config.rssi_threshold = g_rtc_rssi_threshold;
  } else {
    g_rtc_rssi_threshold = g_scan_config.rssi_threshold;
    g_rtc_rssi_threshold_valid = true;
  }

  if (USE_DEEP_SLEEP_BETWEEN_SCANS && woke_from_ext && (g_boot_gpio_wakeup_status & (1ULL << STICKS3_BTN_B_GPIO))) {
    cycleRssiThreshold();
    g_skip_next_btn_b_press = true;
  }

  if (woke_from_ext && (g_boot_gpio_wakeup_status & (1ULL << STICKS3_PM1_IRQ_GPIO))) {
    consumePm1ButtonEvent();
  }

  if (!quiet_boot) {
    M5.Lcd.printf("Loaded %d sigs\n", g_signature_count);
    M5.Lcd.printf("RSSI thresh: %d\n", g_scan_config.rssi_threshold);
  }

  g_detection_mutex = xSemaphoreCreateMutex();

  NimBLEDevice::init("");
  if (!quiet_boot) {
    M5.Lcd.println("BLE initialized");
  }

  M5.Speaker.setVolume(64);
  if (quiet_boot) {
    lcdSleep();
  } else {
    M5.Lcd.setBrightness(g_scan_config.lcd_brightness);
  }
  g_lcd_wake_time = millis();
  g_next_battery_sample_at = g_lcd_wake_time + BATTERY_WAKE_SETTLE_MS;
  refreshBatteryStatus(true);

  if (!woke_from_deep_sleep) {
    delay(1500);
  }
  Serial.println("Setup complete, starting scan loop");
  startScan();
}

void loop() {
  M5.update();

  static uint8_t  prev_detection_count = 0;
  static bool     needs_redraw = true;
  uint32_t now = millis();

  refreshBatteryStatus();

  if (consumePm1ButtonEvent()) {
    lcdWake();
    needs_redraw = true;
  }

  // ── Handle new detections ─────────────────────────────────────────────────
  if (g_new_detection_flag) {
    g_new_detection_flag = false;
    lcdWake();
    needs_redraw = true;
  }

  // ── LCD auto-off ──────────────────────────────────────────────────────────
  if (g_lcd_on && (now - g_lcd_wake_time >= g_scan_config.lcd_on_ms)) {
    M5.Lcd.setBrightness(g_scan_config.lcd_brightness_dim);
    delay(500);
    lcdSleep();
  }

  // ── Detection count changes (expiry) ──────────────────────────────────────
  xSemaphoreTake(g_detection_mutex, portMAX_DELAY);
  uint8_t current_count = g_detection_count;
  xSemaphoreGive(g_detection_mutex);

  if (current_count != prev_detection_count) {
    if (current_count > prev_detection_count) {
      lcdWake();
      alertBuzz();
    }
    prev_detection_count = current_count;
    needs_redraw = true;
  }

  // ── Redraw ────────────────────────────────────────────────────────────────
  if (needs_redraw && g_lcd_on) {
    if (current_count == 0) {
      drawClearScreen();
    } else {
      drawDetectionScreen();
    }
    needs_redraw = false;
  }

  // ── Button A: force rescan + wake LCD ─────────────────────────────────────
  if (M5.BtnA.wasPressed()) {
    lcdWake();
    if (!g_scanning) {
      startScan();
      needs_redraw = true;
    }
  }

  // ── Button B: cycle RSSI threshold ────────────────────────────────────────
  if (M5.BtnB.wasPressed()) {
    if (g_skip_next_btn_b_press) {
      g_skip_next_btn_b_press = false;
    } else {
      lcdWake();
      cycleRssiThreshold();
      needs_redraw = true;
    }
  } else if (g_skip_next_btn_b_press && !M5.BtnB.isPressed()) {
    g_skip_next_btn_b_press = false;
  }

  // ── Sleep between scans ───────────────────────────────────────────────────
  if (!g_scanning) {
    if (!g_lcd_on) {
      uint32_t sleep_time = g_scan_config.interval_ms - g_scan_config.window_ms;
      if (sleep_time > 100) {
        if (USE_DEEP_SLEEP_BETWEEN_SCANS) {
          enterDeepSleep(sleep_time);
        } else {
          enterLightSleep(sleep_time);
        }
        startScan();
        needs_redraw = true;
      }
    } else {
      static uint32_t last_scan_end = 0;
      if (now - last_scan_end >= g_scan_config.interval_ms) {
        startScan();
        last_scan_end = now;
        needs_redraw = true;
      }
      delay(50);
    }
  } else {
    delay(50);
  }
}