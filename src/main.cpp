#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

#include "config.h"
#include "display.h"

namespace {
  constexpr uint32_t kFetchIntervalMs = 15000;
  constexpr uint32_t kWifiTimeoutMs   = 15000;
  constexpr uint32_t kRequestTimeoutMs = 5000;
  constexpr uint32_t kLoadingTickMs   = 20;

  struct WifiCredential { const char* ssid; const char* pass; };
  struct Station        { const char* rbl;  const char* line; const char* towards; };

  const Station kStations[] = {
#define X(rbl, line, towards) {rbl, line, towards},
    STATION_LIST
#undef X
  };
  const WifiCredential kWifiCredentials[] = {
    {WIFI_SSID, WIFI_PASS},
    {WIFI_SSID_2, WIFI_PASS_2},
    {WIFI_SSID_3, WIFI_PASS_3},
  };
  const size_t kStationCount        = sizeof(kStations) / sizeof(kStations[0]);
  const size_t kWifiCredentialCount = sizeof(kWifiCredentials) / sizeof(kWifiCredentials[0]);

  // ── Shared state: written by fetch task, read by main loop ─────────────────
  SemaphoreHandle_t gMutex = nullptr;

  struct FetchResult {
    Departure first;
    Departure second;
    bool      haveSecond;
    bool      succeeded;
    size_t    stationIdx;
    bool      fresh;      // main loop clears after consuming
  };
  FetchResult gPending = {{"",(String)"",0},{"",(String)"",0}, false, false, 0, false};

  volatile bool   gFetchRequested = false;
  volatile size_t gFetchStartIdx  = 0;

  // ── Main loop display state (never touched by fetch task) ──────────────────
  Departure lastFirst      = {"", "", -1};
  Departure lastSecond     = {"", "", -1};
  bool      lastHaveSecond = false;
  bool      everHadData    = false;

  size_t    stationIndex   = 0;
  uint32_t  lastFetchMs    = 0;
  uint32_t  lastCycleMs    = 0;

  // ── Button state (used when STATION_CYCLE_MODE == 0) ──────────────────────
  bool     lastRawButton   = true;   // HIGH = not pressed (INPUT_PULLUP)
  bool     stableButton    = true;
  uint32_t lastDebounceMs  = 0;
  constexpr uint32_t kDebounceMs = 50;

  // ── Helpers ────────────────────────────────────────────────────────────────
  const Station& stationAt(size_t idx) { return kStations[idx % kStationCount]; }

  void waitWithLoading(uint32_t durationMs) {
    uint32_t start = millis();
    while (millis() - start < durationMs) {
      display_update_animation();
      delay(kLoadingTickMs);
    }
  }

  void applyOverrides(Departure& dep, const Station& s) {
    if (dep.countdown < 0) return;
    if (s.line    && s.line[0])    dep.line    = s.line;
    if (s.towards && s.towards[0]) dep.towards = s.towards;
  }

  // ── Try one station (called from fetch task only) ──────────────────────────
  bool fetchOne(size_t idx, Departure& best, Departure& second) {
    best   = {"", "", -1};
    second = {"", "", -1};

    if (WiFi.status() != WL_CONNECTED) return false;

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;

    const Station& s = stationAt(idx);
    String url = "https://www.wienerlinien.at/ogd_realtime/monitor?activateTrafficInfo=stoerunglang&rbl=";
    url += s.rbl;
    http.setTimeout(kRequestTimeoutMs);
    if (!http.begin(client, url)) return false;

    int code = http.GET();
    if (code != HTTP_CODE_OK) { http.end(); return false; }

    String body = http.getString();
    http.end();

    DynamicJsonDocument doc(16384);
    if (deserializeJson(doc, body, DeserializationOption::NestingLimit(64))) return false;

    JsonObject data = doc["data"];
    if (data.isNull()) return false;

    JsonArray monitors = data["monitors"];
    if (monitors.isNull() || monitors.size() == 0) return false;

    for (JsonObject monitor : monitors) {
      JsonArray lines = monitor["lines"];
      if (lines.isNull()) continue;
      for (JsonObject line : lines) {
        String lineName = line["name"]    | "";
        String towards  = line["towards"] | "";
        JsonObject deps = line["departures"];
        if (deps.isNull()) continue;
        JsonVariant dv = deps["departure"];
        if (!dv.is<JsonArray>()) continue;
        for (JsonObject dep : dv.as<JsonArray>()) {
          int cd = -1;
          JsonVariant dt = dep["departureTime"];
          if (dt.is<JsonObject>()) cd = dt["countdown"] | -1;
          if (cd < 0) continue;
          if (best.countdown < 0 || cd < best.countdown) {
            second = best;
            best   = {lineName, towards, cd};
          } else if (second.countdown < 0 || cd < second.countdown) {
            second = {lineName, towards, cd};
          }
        }
      }
    }

    if (best.countdown >= 0) {
      applyOverrides(best, s);
      applyOverrides(second, s);
      return true;
    }
    return false;
  }

  // ── Fetch task: runs in background, never touches the display ──────────────
  void fetchTask(void*) {
    for (;;) {
      if (gFetchRequested) {
        gFetchRequested = false;
        size_t startIdx = gFetchStartIdx;

        FetchResult result;
        result.first      = {"", "", -1};
        result.second     = {"", "", -1};
        result.haveSecond = false;
        result.succeeded  = false;
        result.stationIdx = startIdx;
        result.fresh      = false;

        for (size_t attempt = 0; attempt < kStationCount; attempt++) {
          size_t idx = (startIdx + attempt) % kStationCount;
          Serial.printf("Fetch: trying RBL=%s\n", stationAt(idx).rbl);
          if (fetchOne(idx, result.first, result.second)) {
            result.haveSecond = result.second.countdown >= 0;
            result.succeeded  = true;
            result.stationIdx = idx;
            break;
          }
        }

        xSemaphoreTake(gMutex, portMAX_DELAY);
        gPending       = result;
        gPending.fresh = true;
        xSemaphoreGive(gMutex);
      }
      vTaskDelay(pdMS_TO_TICKS(20));
    }
  }

  // ── WiFi (blocking, only called during setup) ──────────────────────────────
  void connectWifi() {
    if (WiFi.status() == WL_CONNECTED) return;
    display_show_loading();
    WiFi.disconnect(true); waitWithLoading(200);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.setSleep(false);
    waitWithLoading(500);

    size_t startIdx =
      (WIFI_PRIMARY_INDEX >= 1 && WIFI_PRIMARY_INDEX <= kWifiCredentialCount)
        ? (WIFI_PRIMARY_INDEX - 1)
        : 0;
    for (size_t offset = 0; offset < kWifiCredentialCount; offset++) {
      size_t ci = (startIdx + offset) % kWifiCredentialCount;
      const WifiCredential& c = kWifiCredentials[ci];
      if (!c.ssid || !c.ssid[0]) continue;
      for (uint32_t attempt = 0; attempt < 3; attempt++) {
        WiFi.begin(c.ssid, c.pass);
        uint32_t t0 = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - t0 < kWifiTimeoutMs) {
          display_update_animation();
          delay(300);
        }
        if (WiFi.status() == WL_CONNECTED) {
          display_status("WiFi connected!"); delay(500); return;
        }
        WiFi.disconnect(true); waitWithLoading(500);
      }
    }
    display_status("WiFi failed!"); delay(2000);
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n\n=== Wiener Linien Monitor Starting ===");

  display_init();
  display_status("Booting...");

  connectWifi();

  gMutex = xSemaphoreCreateMutex();
  // 12KB stack: fetchOne heap-allocates JSON doc, stack usage is modest
  xTaskCreate(fetchTask, "fetch", 12288, nullptr, 1, nullptr);

  lastFetchMs = millis() - kFetchIntervalMs;  // trigger fetch immediately
  lastCycleMs = millis();

  if (STATION_CYCLE_MODE == 0 && kStationCount > 1) {
    pinMode(STATION_BUTTON_PIN, INPUT_PULLUP);
  }
}

void loop() {
  uint32_t now = millis();

  // ── Button-cycle station ────────────────────────────────────────────────────
  if (STATION_CYCLE_MODE == 0 && kStationCount > 1) {
    bool raw = (bool)digitalRead(STATION_BUTTON_PIN);
    bool active = STATION_BUTTON_ACTIVE_LOW ? !raw : raw;
    if (raw != lastRawButton) {
      lastRawButton  = raw;
      lastDebounceMs = now;
    }
    if ((now - lastDebounceMs >= kDebounceMs) && active && !stableButton) {
      // Rising edge of debounced press
      stationIndex = (stationIndex + 1) % kStationCount;
      lastFetchMs  = 0;  // force immediate fetch for the new station
    }
    if (now - lastDebounceMs >= kDebounceMs) {
      stableButton = active;
    }
  }

  // ── Auto-cycle station (just advances index + triggers fetch, no blocking) ──
  if (STATION_CYCLE_MODE == 1 && kStationCount > 1) {
    if (now - lastCycleMs >= STATION_CYCLE_INTERVAL_MS) {
      lastCycleMs  = now;
      stationIndex = (stationIndex + 1) % kStationCount;
      lastFetchMs  = 0;  // force immediate fetch for the new station
    }
  }

  // ── Request fetch when due (just sets a flag, never blocks) ───────────────
  if (!gFetchRequested && (now - lastFetchMs >= kFetchIntervalMs)) {
    lastFetchMs     = now;
    gFetchStartIdx  = stationIndex;
    gFetchRequested = true;
  }

  // ── Consume fetch result (non-blocking mutex) ─────────────────────────────
  if (xSemaphoreTake(gMutex, 0)) {
    if (gPending.fresh) {
      gPending.fresh = false;
      if (gPending.succeeded) {
        lastFirst      = gPending.first;
        lastSecond     = gPending.second;
        lastHaveSecond = gPending.haveSecond;
        stationIndex   = gPending.stationIdx;
        everHadData    = true;
        lastCycleMs    = millis();  // stay on this station for the full interval
        display_departures(lastFirst, lastSecond, lastHaveSecond);
      } else if (!everHadData) {
        // Only show "no data" if we have never successfully fetched anything
        display_no_data(stationAt(stationIndex).line, stationAt(stationIndex).towards);
      }
      // If failed but had data before: keep showing last screen unchanged
    }
    xSemaphoreGive(gMutex);
  }

  // ── Animation (runs every loop, only redraws on state change) ─────────────
  if ((TEST_ANIMATION_MODE || lastFirst.countdown == 0) && everHadData) {
    display_update_animation();
  }

  delay(5);
}
