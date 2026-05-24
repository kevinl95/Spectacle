/*
 * GlassesDetector - Smart Glasses BLE Scanner for M5Stack StickS3
 * 
 * Passively scans for BLE advertisements from known smart glasses
 * (Meta Ray-Ban, Snap Spectacles, etc.) and alerts the user.
 * 
 * Configuration is loaded from /config.json on SPIFFS.
 * 
 * Hardware: M5Stack StickS3 (ESP32-S3)
 * License: MIT
 */

#include <M5Unified.h>
#include <NimBLEDevice.h>
#include <ArduinoJson.h>
#include <SPIFFS.h>
#include <esp_sleep.h>

// ── Structs ──────────────────────────────────────────────────────────────────

struct DeviceSignature {
  String id;
  String label;
  String category;       // "glasses", "meta_ambiguous"
  uint16_t manufacturer_ids[4];
  uint8_t  mfr_id_count;
  String   service_uuids[4];
  uint8_t  svc_uuid_count;
  String   name_prefixes[4];
  uint8_t  name_prefix_count;
  uint16_t adv_len_min;
  uint16_t adv_len_max;
};

struct Detection {
  String   label;
  String   category;
  int      rssi;
  int      confidence;
  uint32_t timestamp;
};

struct ScanConfig {
  uint32_t window_ms;
  uint32_t interval_ms;
  int      rssi_threshold;
  uint32_t display_timeout_ms;
  uint32_t lcd_on_ms;           // how long LCD stays on after wake
  uint8_t  lcd_brightness;      // 0-255
  uint8_t  lcd_brightness_dim;  // dimmed level before off
};

struct ConfidenceWeights {
  int manufacturer_id_match;
  int service_uuid_match;
  int name_prefix_match;
  int adv_length_match;
  int threshold_likely;
  int threshold_possible;
};

// ── Globals ──────────────────────────────────────────────────────────────────

static DeviceSignature   g_signatures[16];
static uint8_t           g_signature_count = 0;
static ScanConfig        g_scan_config;
static ConfidenceWeights g_weights;

static Detection         g_detections[8];
static uint8_t           g_detection_count = 0;
static SemaphoreHandle_t g_detection_mutex;

static bool              g_scanning = false;
static uint32_t          g_last_detection_time = 0;
static uint32_t          g_scan_count = 0;

// ── LCD & Power State ────────────────────────────────────────────────────────

static bool              g_lcd_on = true;
static uint32_t          g_lcd_wake_time = 0;     // when LCD was last turned on
static bool              g_new_detection_flag = false;  // set in ISR/callback context

// ── Display Constants ────────────────────────────────────────────────────────

static const uint16_t COLOR_BG        = TFT_BLACK;
static const uint16_t COLOR_CLEAR     = 0x0400;  // dark green
static const uint16_t COLOR_POSSIBLE  = 0xFDA0;  // amber/orange
static const uint16_t COLOR_LIKELY    = 0xF800;  // red
static const uint16_t COLOR_AMBIGUOUS = 0xFFE0;  // yellow
static const uint16_t COLOR_TEXT      = TFT_WHITE;
static const uint16_t COLOR_DIM       = 0x7BEF;  // gray

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
  g_scan_config.window_ms        = scan["window_ms"] | 3000;
  g_scan_config.interval_ms      = scan["interval_ms"] | 10000;
  g_scan_config.rssi_threshold   = scan["rssi_threshold"] | -75;
  g_scan_config.display_timeout_ms = scan["display_timeout_ms"] | 30000;
  g_scan_config.lcd_on_ms        = scan["lcd_on_ms"] | 5000;
  g_scan_config.lcd_brightness   = scan["lcd_brightness"] | 128;
  g_scan_config.lcd_brightness_dim = scan["lcd_brightness_dim"] | 32;

  // Confidence weights
  JsonObject conf = doc["confidence"];
  g_weights.manufacturer_id_match = conf["manufacturer_id_match"] | 40;
  g_weights.service_uuid_match    = conf["service_uuid_match"] | 30;
  g_weights.name_prefix_match     = conf["name_prefix_match"] | 20;
  g_weights.adv_length_match      = conf["adv_length_match"] | 10;
  g_weights.threshold_likely      = conf["threshold_likely"] | 70;
  g_weights.threshold_possible    = conf["threshold_possible"] | 40;

  // Device signatures
  JsonArray devices = doc["devices"];
  g_signature_count = 0;

  for (JsonObject dev : devices) {
    if (g_signature_count >= 16) break;

    DeviceSignature& sig = g_signatures[g_signature_count];
    sig.id       = dev["id"] | "unknown";
    sig.label    = dev["label"] | "Unknown";
    sig.category = dev["category"] | "glasses";

    // Manufacturer IDs
    sig.mfr_id_count = 0;
    JsonArray mfr_ids = dev["manufacturer_ids"];
    for (const char* hex : mfr_ids) {
      if (sig.mfr_id_count >= 4) break;
      sig.manufacturer_ids[sig.mfr_id_count++] = parseHex16(hex);
    }

    // Service UUIDs
    sig.svc_uuid_count = 0;
    JsonArray svc_uuids = dev["service_uuids"];
    for (const char* uuid : svc_uuids) {
      if (sig.svc_uuid_count >= 4) break;
      sig.service_uuids[sig.svc_uuid_count++] = uuid;
    }

    // Name prefixes
    sig.name_prefix_count = 0;
    JsonArray name_pfx = dev["name_prefixes"];
    for (const char* pfx : name_pfx) {
      if (sig.name_prefix_count >= 4) break;
      sig.name_prefixes[sig.name_prefix_count++] = pfx;
    }

    // Advertisement data length range
    JsonArray adv_range = dev["adv_data_length_range"];
    sig.adv_len_min = adv_range[0] | 0;
    sig.adv_len_max = adv_range[1] | 0;

    g_signature_count++;
  }

  Serial.printf("Loaded %d device signatures\n", g_signature_count);
  return true;
}

// ── BLE Scanning ─────────────────────────────────────────────────────────────

int scoreDetection(const NimBLEAdvertisedDevice* device, const DeviceSignature& sig) {
  int score = 0;

  // Check manufacturer IDs
  if (sig.mfr_id_count > 0 && device->haveManufacturerData()) {
    std::string mfr_data = device->getManufacturerData();
    if (mfr_data.length() >= 2) {
      uint16_t detected_id = (uint16_t)mfr_data[0] | ((uint16_t)mfr_data[1] << 8);
      for (uint8_t i = 0; i < sig.mfr_id_count; i++) {
        if (detected_id == sig.manufacturer_ids[i]) {
          score += g_weights.manufacturer_id_match;
          break;
        }
      }
    }
  }

  // Check service UUIDs
  if (sig.svc_uuid_count > 0) {
    for (uint8_t i = 0; i < sig.svc_uuid_count; i++) {
      if (device->isAdvertisingService(NimBLEUUID(sig.service_uuids[i].c_str()))) {
        score += g_weights.service_uuid_match;
        break;
      }
    }
  }

  // Check device name prefixes
  if (sig.name_prefix_count > 0 && device->haveName()) {
    String name = device->getName().c_str();
    for (uint8_t i = 0; i < sig.name_prefix_count; i++) {
      if (name.startsWith(sig.name_prefixes[i])) {
        score += g_weights.name_prefix_match;
        break;
      }
    }
  }

  // Check advertisement data length range
  if (sig.adv_len_min > 0 && sig.adv_len_max > 0) {
    size_t payload_len = device->getPayload().size();
    if (payload_len >= sig.adv_len_min && payload_len <= sig.adv_len_max) {
      score += g_weights.adv_length_match;
    }
  }

  return score;
}

class ScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* device) override {
    int rssi = device->getRSSI();
    if (rssi < g_scan_config.rssi_threshold) return;

    for (uint8_t s = 0; s < g_signature_count; s++) {
      int score = scoreDetection(device, g_signatures[s]);
      if (score < g_weights.threshold_possible) continue;

      xSemaphoreTake(g_detection_mutex, portMAX_DELAY);

      // Check if already detected (update if higher confidence)
      bool found = false;
      for (uint8_t d = 0; d < g_detection_count; d++) {
        if (g_detections[d].label == g_signatures[s].label) {
          if (score > g_detections[d].confidence || rssi > g_detections[d].rssi) {
            g_detections[d].rssi       = rssi;
            g_detections[d].confidence = score;
            g_detections[d].timestamp  = millis();
          }
          found = true;
          break;
        }
      }

      // Add new detection
      if (!found && g_detection_count < 8) {
        g_detections[g_detection_count].label      = g_signatures[s].label;
        g_detections[g_detection_count].category   = g_signatures[s].category;
        g_detections[g_detection_count].rssi       = rssi;
        g_detections[g_detection_count].confidence = score;
        g_detections[g_detection_count].timestamp  = millis();
        g_detection_count++;
      }

      g_last_detection_time = millis();
      g_new_detection_flag = true;

      xSemaphoreGive(g_detection_mutex);
      break; // matched a signature, move to next device
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
      // Shift remaining detections down
      for (int j = i; j < g_detection_count - 1; j++) {
        g_detections[j] = g_detections[j + 1];
      }
      g_detection_count--;
    }
  }
  xSemaphoreGive(g_detection_mutex);

  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setActiveScan(false);  // passive scan - don't send scan requests
  scan->setDuplicateFilter(true);
  scan->setScanCallbacks(&g_scan_callbacks, false);
  scan->start(g_scan_config.window_ms / 1000, false);
  g_scanning = true;
  g_scan_count++;
}

// ── LCD & Power Management ───────────────────────────────────────────────────

void lcdWake() {
  if (!g_lcd_on) {
    M5.Lcd.wakeup();
    M5.Lcd.setBrightness(g_scan_config.lcd_brightness);
    g_lcd_on = true;
    Serial.println("LCD wake");
  }
  g_lcd_wake_time = millis();
}

void lcdSleep() {
  if (g_lcd_on) {
    M5.Lcd.setBrightness(0);
    M5.Lcd.sleep();
    g_lcd_on = false;
    Serial.println("LCD sleep");
  }
}

void enterLightSleep(uint32_t sleep_ms) {
  // LCD off before sleeping
  lcdSleep();

  // Configure timer wakeup
  esp_sleep_enable_timer_wakeup((uint64_t)sleep_ms * 1000ULL);

  // Configure GPIO wakeup for Button A (active low)
  // StickS3 BtnA is GPIO37 on M5Unified — verify pin for your board revision
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_37, 0);  // wake on LOW

  Serial.printf("Light sleep %lums\n", sleep_ms);
  Serial.flush();

  esp_light_sleep_start();

  // ── We wake up here ──
  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  Serial.printf("Woke up, cause: %d\n", cause);

  // If woken by button, turn on LCD
  if (cause == ESP_SLEEP_WAKEUP_EXT0) {
    lcdWake();
  }
}

// ── Display ──────────────────────────────────────────────────────────────────

void drawStatusBar() {
  M5.Lcd.setTextColor(COLOR_DIM, COLOR_BG);
  M5.Lcd.setTextSize(1);
  M5.Lcd.setCursor(4, 2);
  M5.Lcd.printf("Scan #%lu  RSSI>%d", g_scan_count, g_scan_config.rssi_threshold);

  // Battery level on right side
  int batt = M5.Power.getBatteryLevel();
  M5.Lcd.setCursor(M5.Lcd.width() - 48, 2);
  M5.Lcd.printf("BAT:%d%%", batt);
}

void drawClearScreen() {
  M5.Lcd.fillScreen(COLOR_BG);
  drawStatusBar();

  // Draw "CLEAR" indicator
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

  // Scanning indicator
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

    // Choose color based on confidence and category
    uint16_t color;
    const char* level;

    if (det.category == "meta_ambiguous") {
      color = COLOR_AMBIGUOUS;
      level = "MAYBE";
    } else if (det.confidence >= g_weights.threshold_likely) {
      color = COLOR_LIKELY;
      level = "LIKELY";
    } else {
      color = COLOR_POSSIBLE;
      level = "POSS";
    }

    // Alert indicator bar
    M5.Lcd.fillRect(0, y, 4, 26, color);

    // Device label
    M5.Lcd.setTextColor(color, COLOR_BG);
    M5.Lcd.setTextSize(1);
    M5.Lcd.setCursor(8, y + 2);
    M5.Lcd.print(det.label);

    // Confidence and RSSI
    M5.Lcd.setTextColor(COLOR_DIM, COLOR_BG);
    M5.Lcd.setCursor(8, y + 15);
    M5.Lcd.printf("%s %d%%  %ddBm", level, det.confidence, det.rssi);

    y += 30;
  }

  xSemaphoreGive(g_detection_mutex);

  // Scanning indicator at bottom
  if (g_scanning) {
    M5.Lcd.setCursor(M5.Lcd.width() / 2 - 30, M5.Lcd.height() - 14);
    M5.Lcd.setTextColor(COLOR_DIM, COLOR_BG);
    M5.Lcd.print("Scanning...");
  }
}

void alertBuzz() {
  M5.Speaker.tone(2000, 100);
  delay(150);
  M5.Speaker.tone(2000, 100);
}

// ── Setup & Loop ─────────────────────────────────────────────────────────────

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);

  Serial.begin(115200);
  Serial.println("GlassesDetector starting...");

  M5.Lcd.setRotation(1);  // landscape on StickS3
  M5.Lcd.fillScreen(COLOR_BG);
  M5.Lcd.setTextColor(COLOR_TEXT, COLOR_BG);
  M5.Lcd.setTextSize(1);
  M5.Lcd.setCursor(4, 4);
  M5.Lcd.println("GlassesDetector v0.1");
  M5.Lcd.println("Loading config...");

  // Init SPIFFS
  if (!SPIFFS.begin(true)) {
    M5.Lcd.println("SPIFFS FAILED");
    Serial.println("SPIFFS mount failed");
    while (1) delay(1000);
  }

  // Load config
  if (!loadConfig("/config.json")) {
    M5.Lcd.println("CONFIG FAILED");
    M5.Lcd.println("Check /config.json");
    Serial.println("Config load failed");
    while (1) delay(1000);
  }

  M5.Lcd.printf("Loaded %d sigs\n", g_signature_count);
  M5.Lcd.printf("RSSI thresh: %d\n", g_scan_config.rssi_threshold);

  // Init mutex
  g_detection_mutex = xSemaphoreCreateMutex();

  // Init BLE
  NimBLEDevice::init(""); // no device name - we're just scanning
  M5.Lcd.println("BLE initialized");

  // Speaker setup
  M5.Speaker.setVolume(64);

  // LCD initial brightness
  M5.Lcd.setBrightness(g_scan_config.lcd_brightness);
  g_lcd_wake_time = millis();

  delay(1500);

  Serial.println("Setup complete, starting scan loop");

  // Do an initial scan immediately
  startScan();
}

void loop() {
  M5.update();

  static uint8_t  prev_detection_count = 0;
  static bool     needs_redraw = true;
  uint32_t now = millis();

  // ── Handle new detections (flag set from BLE callback) ───────────────────
  if (g_new_detection_flag) {
    g_new_detection_flag = false;
    lcdWake();
    alertBuzz();
    needs_redraw = true;
  }

  // ── LCD auto-off timer ───────────────────────────────────────────────────
  if (g_lcd_on && (now - g_lcd_wake_time >= g_scan_config.lcd_on_ms)) {
    // Dim briefly before turning off
    M5.Lcd.setBrightness(g_scan_config.lcd_brightness_dim);
    delay(500);
    lcdSleep();
  }

  // ── Check if detection count changed (expiry) ───────────────────────────
  xSemaphoreTake(g_detection_mutex, portMAX_DELAY);
  uint8_t current_count = g_detection_count;
  xSemaphoreGive(g_detection_mutex);

  if (current_count != prev_detection_count) {
    if (current_count > prev_detection_count && !g_new_detection_flag) {
      // Edge case: count increased but flag was already cleared
      lcdWake();
      alertBuzz();
    }
    prev_detection_count = current_count;
    needs_redraw = true;
  }

  // ── Redraw display (only if LCD is on) ──────────────────────────────────
  if (needs_redraw && g_lcd_on) {
    if (current_count == 0) {
      drawClearScreen();
    } else {
      drawDetectionScreen();
    }
    needs_redraw = false;
  }

  // ── Button A: force rescan + wake LCD ───────────────────────────────────
  if (M5.BtnA.wasPressed()) {
    lcdWake();
    if (!g_scanning) {
      startScan();
      needs_redraw = true;
    }
  }

  // ── Button B: cycle RSSI threshold ──────────────────────────────────────
  if (M5.BtnB.wasPressed()) {
    lcdWake();
    g_scan_config.rssi_threshold -= 10;
    if (g_scan_config.rssi_threshold < -100) {
      g_scan_config.rssi_threshold = -50;
    }
    Serial.printf("RSSI threshold: %d\n", g_scan_config.rssi_threshold);
    needs_redraw = true;
  }

  // ── Sleep between scans ─────────────────────────────────────────────────
  // If not currently scanning and LCD is off, enter light sleep until
  // the next scan window. If LCD is on, just idle with short delays
  // so the display stays responsive to buttons.
  if (!g_scanning) {
    if (!g_lcd_on) {
      // Calculate remaining time until next scan
      uint32_t sleep_time = g_scan_config.interval_ms - g_scan_config.window_ms;
      if (sleep_time > 100) {
        enterLightSleep(sleep_time);
        // After waking, immediately start the next scan
        startScan();
        needs_redraw = true;
      }
    } else {
      // LCD is on — stay awake for button responsiveness but start scan on schedule
      static uint32_t last_scan_end = 0;
      if (now - last_scan_end >= g_scan_config.interval_ms) {
        startScan();
        last_scan_end = now;
        needs_redraw = true;
      }
      delay(50);
    }
  } else {
    delay(50);  // polling during active scan
  }
}
