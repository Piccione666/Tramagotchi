#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ESPmDNS.h>
#include <DNSServer.h>
#include <LittleFS.h>
#include <Update.h>
#include <ArduinoOTA.h>
#include "config.h"
#include "pinout.h"
#include "stations/stations.h"
#if STATION_CONFIG_MODE == 1
#include <Preferences.h>
#if CONFIG_WEB_SERVER_ENABLED
#include <WebServer.h>
#include <HTTPUpdate.h>
#endif
#endif
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

#include "display.h"
#include "graphics_config.h"

#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif
#ifndef WIFI_PASS
#define WIFI_PASS ""
#endif
#ifndef WIFI_SSID_2
#define WIFI_SSID_2 ""
#endif
#ifndef WIFI_PASS_2
#define WIFI_PASS_2 ""
#endif
#ifndef WIFI_SSID_3
#define WIFI_SSID_3 ""
#endif
#ifndef WIFI_PASS_3
#define WIFI_PASS_3 ""
#endif

#if SERIAL_DEBUG_ENABLED
  #define LOGF(...) Serial.printf(__VA_ARGS__)
#else
  #define LOGF(...) do {} while (0)
#endif

namespace {
  constexpr uint32_t kFetchIntervalMs = 15000;
  constexpr uint32_t kWifiTimeoutMs   = WIFI_CONNECT_TIMEOUT_MS;
  constexpr uint32_t kRequestTimeoutMs = 5000;
  const char* kUserAgent = "Tramagotchi/1.0 (ESP32)";
  constexpr uint32_t kLoadingTickMs   = 20;
  constexpr uint32_t kStorageVersion = 1;
  constexpr size_t kMaxStoredStationBytes = 768;
  constexpr size_t kMaxStoredWifiSsidBytes = 32;
  constexpr size_t kMaxStoredWifiPassBytes = 63;
  constexpr size_t kMaxStoredStationNameBytes = 48;
  constexpr size_t kMaxStoredLineBytes = 24;
  constexpr size_t kMaxStoredTowardsBytes = 64;
  constexpr uint32_t kDebounceMs = 50;
#if !PURGE_EMOJI
  constexpr uint32_t kKawaiiToggleLongMs = KAWAII_TOGGLE_LONG_MS;
#endif
  constexpr uint32_t kRetryIntervalMs   = 500;
  constexpr uint32_t kNoDataThresholdMs = NO_DATA_THRESHOLD_MS;
  constexpr uint8_t  kNoDataSkipCycles  = 3;  // after this many no-data cycles, skip a station's no-data screen

  struct WifiCredential { const char* ssid; const char* pass; };
  struct DefaultStation { const char* rbl; const char* stationName; const char* line; const char* towards; };
  struct Station        { String rbl; String stationName; String line; String towards; };

  const DefaultStation kDefaultStations[] = {
#define X(rbl, stationName, line, towards) {rbl, stationName, line, towards},
    STATION_LIST
#undef X
  };
  const char* kDepartureBlacklist[] = {
#define B(pattern) pattern,
    DEPARTURE_BLACKLIST
#undef B
    nullptr,
  };
  const WifiCredential kWifiCredentials[] = {
    {WIFI_SSID, WIFI_PASS},
    {WIFI_SSID_2, WIFI_PASS_2},
    {WIFI_SSID_3, WIFI_PASS_3},
  };
  constexpr size_t kDefaultStationCount = sizeof(kDefaultStations) / sizeof(kDefaultStations[0]);
  constexpr size_t kMaxStationCount = STATION_CONFIG_MAX_COUNT;
  const size_t kWifiCredentialCount = sizeof(kWifiCredentials) / sizeof(kWifiCredentials[0]);

  Station gStations[kMaxStationCount];
  size_t gStationCount = 0;

  // ── Shared state: written by fetch task, read by main loop ─────────────────
  SemaphoreHandle_t gMutex = nullptr;
  SemaphoreHandle_t gStationMutex = nullptr;
#if STATION_CONFIG_MODE == 1
  Preferences gPrefs;
#if CONFIG_WEB_SERVER_ENABLED
  WebServer gServer(80);
  DNSServer gDnsServer;
  bool gSetupPortalActive = false;
  constexpr size_t kMaxSetupSsids = 20;
  String gSetupSsids[kMaxSetupSsids];
  size_t gSetupSsidCount = 0;
  bool gDataFsReady = false;
  File gUploadFile;
  bool gUploadSucceeded = false;
  String gUploadMessage;
  bool gFsImageUploadSucceeded = false;
  String gFsImageUploadMessage;
  bool gFirmwareUploadSucceeded = false;
  String gFirmwareUploadMessage;
  String   gDataVersion;            // version.txt of the installed stop data, "" = unknown
  uint32_t gLastDataCheckMs = 0;
  bool     gDataCheckedOnce = false;
  constexpr uint32_t kDataFirstCheckDelayMs = 5UL * 60UL * 1000UL;
  constexpr uint32_t kDataCheckIntervalMs   = DATA_UPDATE_CHECK_HOURS * 60UL * 60UL * 1000UL;
#endif
#endif

  struct FetchResult {
    Departure first;
    Departure second;
    bool      haveSecond;
    bool      succeeded;
    size_t    stationIdx;
    String    noDataLine;
    String    noDataTowards;
    NoDataReason reason;  // why the start station had nothing to show
    bool      fresh;      // main loop clears after consuming
  };
  FetchResult gPending = {{"",(String)"",0},{"",(String)"",0}, false, false, 0, "", "", NoDataUnknown, false};

  volatile bool   gFetchRequested = false;
  volatile size_t gFetchStartIdx  = 0;

  // Last fetch outcome per station, for the config page's status endpoint.
  enum StationState : uint8_t { StationUntried = 0, StationOk, StationNoMatch, StationNoService, StationNetwork };
  volatile uint8_t gStationState[kMaxStationCount] = {StationUntried};

  StationState stateFromReason(NoDataReason reason) {
    switch (reason) {
      case NoDataNoMatch:   return StationNoMatch;
      case NoDataNoService: return StationNoService;
      case NoDataNetwork:   return StationNetwork;
      default:              return StationUntried;
    }
  }

  const char* stationStateText(uint8_t state) {
    switch (state) {
      case StationOk:        return "ok";
      case StationNoMatch:   return "no match";
      case StationNoService: return "no service";
      case StationNetwork:   return "network error";
      default:               return "not tried yet";
    }
  }

  // ── Main loop display state (never touched by fetch task) ──────────────────
  Departure lastFirst      = {"", "", -1};
  Departure lastSecond     = {"", "", -1};
  bool      lastHaveSecond = false;
  bool      everHadData    = false;
  bool      noDataVisible  = false;
  NoDataReason noDataReason = NoDataUnknown;  // kept so redraws keep the same label
  uint32_t  noDataStartMs  = 0;      // when the current station showed no data
  uint32_t  fetchFailStreakMs = 0;   // timestamp of first failure in current streak; 0 = none
  bool      graceShown    = false;   // (GRACE_DISPLAY_MODE==1) loading screen already painted

  size_t    stationIndex   = 0;
  uint32_t  lastFetchMs    = 0;
  uint32_t  lastCycleMs    = 0;
  uint8_t   failCycles[kMaxStationCount] = {0};  // consecutive no-data cycles per station

#if POPUP_KAWAII_ENABLED
  bool     gKawaiiPopupActive  = false;
  uint32_t gKawaiiPopupStartMs = 0;
#endif

  // ── Button state ───────────────────────────────────────────────────────────
  // Both are seeded from the actual pin in setup(), so the idle level is
  // whatever the wiring says it is; starting them at a guessed level made the
  // first loop see a phantom release and cycle a station at boot.
  bool     lastRawButton   = false;  // raw pin level, last time it changed
  bool     stableButton    = false;  // debounced "is pressed"
  uint32_t lastDebounceMs  = 0;
  uint32_t buttonPressMs   = 0;      // when the debounced button was pressed
  bool     longPressHandled = false;
#if !PURGE_EMOJI
  bool     kawaiiActive    = false;  // persistent kawaii mode toggled by long press
#else
  constexpr bool kawaiiActive = false;
#endif

  // ── Helpers ────────────────────────────────────────────────────────────────
  size_t stationCount() {
    if (gStationMutex) xSemaphoreTake(gStationMutex, portMAX_DELAY);
    size_t count = gStationCount;
    if (gStationMutex) xSemaphoreGive(gStationMutex);
    return count;
  }

  Station stationAt(size_t idx) {
    if (gStationMutex) xSemaphoreTake(gStationMutex, portMAX_DELAY);
    size_t count = gStationCount ? gStationCount : 1;
    Station station = gStations[idx % count];
    if (gStationMutex) xSemaphoreGive(gStationMutex);
    return station;
  }

  String escapeHtml(const String& value) {
    String out;
    out.reserve(value.length());
    for (size_t i = 0; i < value.length(); i++) {
      char c = value[i];
      if (c == '&') out += F("&amp;");
      else if (c == '<') out += F("&lt;");
      else if (c == '>') out += F("&gt;");
      else if (c == '"') out += F("&quot;");
      else out += c;
    }
    return out;
  }

  String stationListText() {
    String text;
    if (gStationMutex) xSemaphoreTake(gStationMutex, portMAX_DELAY);
    for (size_t i = 0; i < gStationCount; i++) {
      text += gStations[i].rbl;
      text += ",";
      text += gStations[i].stationName;
      text += ",";
      text += gStations[i].line;
      text += ",";
      text += gStations[i].towards;
      if (i + 1 < gStationCount) text += "\n";
    }
    if (gStationMutex) xSemaphoreGive(gStationMutex);
    return text;
  }

  bool isDigitsOnly(const String& value) {
    if (!value.length()) return false;
    for (size_t i = 0; i < value.length(); i++) {
      if (!isDigit((unsigned char)value[i])) return false;
    }
    return true;
  }

  bool isSingleLineFieldValid(const String& value, size_t maxBytes) {
    if (value.length() > maxBytes) return false;
    for (size_t i = 0; i < value.length(); i++) {
      char c = value[i];
      if (c == '\r' || c == '\n') return false;
    }
    return true;
  }

  String normalizeMatchText(const String& value) {
    String out;
    out.reserve(value.length() + 4);
    for (size_t i = 0; i < value.length();) {
      uint8_t c = static_cast<uint8_t>(value[i]);
      if (c == 0xC3 && i + 1 < value.length()) {
        uint8_t next = static_cast<uint8_t>(value[i + 1]);
        if (next == 0xA4 || next == 0x84) { out += "ae"; i += 2; continue; }
        if (next == 0xB6 || next == 0x96) { out += "oe"; i += 2; continue; }
        if (next == 0xBC || next == 0x9C) { out += "ue"; i += 2; continue; }
        if (next == 0x9F) { out += "ss"; i += 2; continue; }
      }
      if (c >= 'A' && c <= 'Z') out += static_cast<char>(c + ('a' - 'A'));
      else out += static_cast<char>(c);
      i++;
    }
    return out;
  }

  // Vienna towards texts carry transfer markers as standalone words: trailing
  // ("Hietzing U", "Floridsdorf S U") and, since 2026, leading ("Parlament,
  // U Volkstheater"). They carry no routing information; drop them so the
  // display stays clean.
  String stripTransferMarkers(const String& value) {
    String out;
    int start = 0;
    const int length = value.length();
    while (start < length) {
      int end = value.indexOf(' ', start);
      if (end < 0) end = length;
      String word = value.substring(start, end);
      if (word.length() && word != "U" && word != "S") {
        if (out.length()) out += ' ';
        out += word;
      }
      start = end + 1;
    }
    while (out.endsWith(",")) out.remove(out.length() - 1);
    return out;
  }

  bool stationIsValid(const Station& station) {
    if (!station.rbl.length() || station.rbl.length() > 8 || !isDigitsOnly(station.rbl)) return false;
    if (!isSingleLineFieldValid(station.stationName, kMaxStoredStationNameBytes)) return false;
    if (!isSingleLineFieldValid(station.line, kMaxStoredLineBytes)) return false;
    if (!isSingleLineFieldValid(station.towards, kMaxStoredTowardsBytes)) return false;
    return true;
  }

  bool parseStationLine(String row, Station& station) {
    row.trim();
    if (!row.length()) return false;

    int p1 = row.indexOf(',');
    int p2 = p1 >= 0 ? row.indexOf(',', p1 + 1) : -1;
    int p3 = p2 >= 0 ? row.indexOf(',', p2 + 1) : -1;

    station.rbl = p1 >= 0 ? row.substring(0, p1) : row;
    station.stationName = p1 >= 0 && p2 >= 0 ? row.substring(p1 + 1, p2) : "";
    station.line = p2 >= 0 && p3 >= 0 ? row.substring(p2 + 1, p3) : "";
    station.towards = p3 >= 0 ? row.substring(p3 + 1) : "";

    station.rbl.trim();
    station.stationName.trim();
    station.line.trim();
    station.towards.trim();
    return stationIsValid(station);
  }

  size_t parseStationList(const String& text, Station* stations, size_t maxCount) {
    size_t count = 0;
    size_t start = 0;
    while (start < text.length() && count < maxCount) {
      int end = text.indexOf('\n', start);
      String row = end >= 0 ? text.substring(start, end) : text.substring(start);
      row.replace("\r", "");
      Station station;
      if (parseStationLine(row, station)) {
        stations[count++] = station;
      }
      if (end < 0) break;
      start = end + 1;
    }
    return count;
  }

  void loadDefaultStations() {
    if (gStationMutex) xSemaphoreTake(gStationMutex, portMAX_DELAY);
    gStationCount = min(kDefaultStationCount, kMaxStationCount);
    for (size_t i = 0; i < gStationCount; i++) {
      gStations[i] = {
        kDefaultStations[i].rbl,
        kDefaultStations[i].stationName,
        kDefaultStations[i].line,
        kDefaultStations[i].towards
      };
    }
    if (gStationMutex) xSemaphoreGive(gStationMutex);
  }

#if STATION_CONFIG_MODE == 1
  void clearStoredStations() {
    if (!gPrefs.begin("stations", false)) return;
    gPrefs.clear();
    gPrefs.end();
  }

  void loadStoredStations() {
    loadDefaultStations();
    if (!gPrefs.begin("stations", true)) return;
    uint32_t version = gPrefs.getUInt("version", 0);
    String stored = gPrefs.getString("list", "");
    gPrefs.end();
    if (!stored.length()) return;
    if (version != kStorageVersion || stored.length() > kMaxStoredStationBytes) {
      clearStoredStations();
      return;
    }

    Station parsed[kMaxStationCount];
    size_t count = parseStationList(stored, parsed, kMaxStationCount);
    if (!count) {
      clearStoredStations();
      return;
    }

    if (gStationMutex) xSemaphoreTake(gStationMutex, portMAX_DELAY);
    gStationCount = count;
    for (size_t i = 0; i < count; i++) gStations[i] = parsed[i];
    if (gStationMutex) xSemaphoreGive(gStationMutex);
  }

  void saveStations(const String& text) {
    if (!gPrefs.begin("stations", false)) return;
    gPrefs.putUInt("version", kStorageVersion);
    gPrefs.putString("list", text);
    gPrefs.end();
  }

#if CONFIG_WEB_SERVER_ENABLED
  String configPageHtml(const String& message);
  void redrawCurrentScreen();

  bool mountDataFs(bool formatOnFail) {
    gDataFsReady = LittleFS.begin(formatOnFail);
    return gDataFsReady;
  }

  bool initDataFs() {
    return mountDataFs(true);
  }

  bool sendDataFile(const char* path) {
    if (!gDataFsReady) {
      return false;
    }

    File file = LittleFS.open(path, "r");
    if (!file) {
      return false;
    }

    gServer.streamFile(file, "text/plain; charset=utf-8");
    file.close();
    return true;
  }

  bool isAllowedUploadPath(const String& path) {
    return path == "line_options.csv" ||
           path.startsWith("stops/");
  }

  String normalizeUploadPath(const String& input) {
    String path = input;
    path.trim();
    path.replace('\\', '/');
    while (path.startsWith("/")) {
      path.remove(0, 1);
    }
    if (!path.length() || path.indexOf("..") >= 0 || path.indexOf("\0") >= 0) {
      return "";
    }
    String candidate = path;
    while (candidate.length()) {
      if (isAllowedUploadPath(candidate)) {
        return "/" + candidate;
      }
      int slash = candidate.indexOf('/');
      if (slash < 0) break;
      candidate = candidate.substring(slash + 1);
    }
    return "";
  }

  bool ensureParentDirectories(const String& path) {
    int slash = 1;
    while (slash >= 0) {
      slash = path.indexOf('/', slash);
      if (slash < 0) break;
      String dir = path.substring(0, slash);
      if (!LittleFS.exists(dir.c_str()) && !LittleFS.mkdir(dir.c_str())) {
        return false;
      }
      slash++;
    }
    return true;
  }

  void resetUploadState() {
    gUploadSucceeded = false;
    gUploadMessage = "";
    if (gUploadFile) {
      gUploadFile.close();
    }
    gUploadFile = File();
  }

  void handleUploadFile() {
    HTTPUpload& upload = gServer.upload();

    if (upload.status == UPLOAD_FILE_START) {
      resetUploadState();
      if (!gDataFsReady && !mountDataFs(false)) {
        gUploadMessage = "LittleFS is not mounted.";
        return;
      }
      String requestedPath = gServer.arg("path");
      if (!requestedPath.length()) requestedPath = upload.filename;
      String safePath = normalizeUploadPath(requestedPath);
      if (!safePath.length()) {
        gUploadMessage = "Invalid upload path.";
        return;
      }
      if (!ensureParentDirectories(safePath)) {
        gUploadMessage = "Could not create parent directory.";
        return;
      }
      gUploadFile = LittleFS.open(safePath.c_str(), "w");
      if (!gUploadFile) {
        gUploadMessage = "Could not open target file for writing.";
        return;
      }
      gUploadSucceeded = true;
    } else if (upload.status == UPLOAD_FILE_WRITE && gUploadFile) {
      gUploadFile.write(upload.buf, upload.currentSize);
      gUploadSucceeded = true;
    } else if (upload.status == UPLOAD_FILE_END) {
      if (gUploadFile) {
        gUploadFile.close();
        gUploadFile = File();
      }
      if (!gUploadSucceeded) {
        gUploadMessage = gUploadMessage.length() ? gUploadMessage : "Upload failed.";
      } else {
        gUploadMessage = "Upload complete.";
      }
    }
  }

  void handleUploadDone() {
    if (gUploadFile) {
      gUploadFile.close();
      gUploadFile = File();
    }
    if (!gUploadSucceeded) {
      String message = gUploadMessage.length() ? gUploadMessage : "Upload failed.";
      gServer.send(400, "text/html; charset=utf-8", configPageHtml(message));
      return;
    }
    gServer.send(200, "text/html; charset=utf-8", configPageHtml(gUploadMessage.length() ? gUploadMessage : "Upload complete."));
  }

  void handleUploadDonePlain() {
    if (gUploadFile) {
      gUploadFile.close();
      gUploadFile = File();
    }
    String message = gUploadMessage.length() ? gUploadMessage : (gUploadSucceeded ? "Upload complete." : "Upload failed.");
    gServer.send(gUploadSucceeded ? 200 : 400, "text/plain; charset=utf-8", message);
  }

  void remountDataFsAfterImageFailure() {
    mountDataFs(false);
  }

  void handleFsImageUpload() {
    HTTPUpload& upload = gServer.upload();

    if (upload.status == UPLOAD_FILE_START) {
      gFsImageUploadSucceeded = false;
      gFsImageUploadMessage = "";
      if (gDataFsReady) {
        LittleFS.end();
        gDataFsReady = false;
      }
      if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_SPIFFS)) {
        gFsImageUploadMessage = "Filesystem image init failed.";
        if (Update.hasError()) {
          gFsImageUploadMessage += " ";
          gFsImageUploadMessage += Update.errorString();
        }
        remountDataFsAfterImageFailure();
      }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      if (gFsImageUploadMessage.length()) return;
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
        gFsImageUploadMessage = "Filesystem image write failed.";
        if (Update.hasError()) {
          gFsImageUploadMessage += " ";
          gFsImageUploadMessage += Update.errorString();
        }
      }
    } else if (upload.status == UPLOAD_FILE_END) {
      if (gFsImageUploadMessage.length()) {
        remountDataFsAfterImageFailure();
        return;
      }
      if (!Update.end(true)) {
        gFsImageUploadMessage = "Filesystem image finalize failed.";
        if (Update.hasError()) {
          gFsImageUploadMessage += " ";
          gFsImageUploadMessage += Update.errorString();
        }
        remountDataFsAfterImageFailure();
      } else {
        gFsImageUploadSucceeded = true;
      }
    } else if (upload.status == UPLOAD_FILE_ABORTED) {
      Update.abort();
      gFsImageUploadMessage = "Filesystem image upload aborted.";
      remountDataFsAfterImageFailure();
    }
  }

  void handleFsImageUploadDone() {
    if (!gFsImageUploadSucceeded) {
      String message = gFsImageUploadMessage.length() ? gFsImageUploadMessage : "Filesystem image upload failed.";
      gServer.send(400, "text/plain; charset=utf-8", message);
      return;
    }

    gServer.send(200, "text/plain; charset=utf-8", "Filesystem image uploaded successfully. Rebooting...");
    delay(1000);
    ESP.restart();
  }

  void handleFirmwareUpload() {
    HTTPUpload& upload = gServer.upload();

    if (upload.status == UPLOAD_FILE_START) {
      gFirmwareUploadSucceeded = false;
      gFirmwareUploadMessage = "";
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
        gFirmwareUploadMessage = "Firmware update init failed.";
        if (Update.hasError()) {
          gFirmwareUploadMessage += " ";
          gFirmwareUploadMessage += Update.errorString();
        }
      }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      if (gFirmwareUploadMessage.length()) return;
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
        gFirmwareUploadMessage = "Firmware write failed.";
        if (Update.hasError()) {
          gFirmwareUploadMessage += " ";
          gFirmwareUploadMessage += Update.errorString();
        }
      }
    } else if (upload.status == UPLOAD_FILE_END) {
      if (gFirmwareUploadMessage.length()) return;
      if (!Update.end(true)) {
        gFirmwareUploadMessage = "Firmware end failed.";
        if (Update.hasError()) {
          gFirmwareUploadMessage += " ";
          gFirmwareUploadMessage += Update.errorString();
        }
      } else {
        gFirmwareUploadSucceeded = true;
      }
    } else if (upload.status == UPLOAD_FILE_ABORTED) {
      // Without this the updater stays "running" and every later upload fails
      // to begin until the device reboots.
      Update.abort();
      gFirmwareUploadMessage = "Firmware upload aborted.";
    }
  }

  void handleFirmwareUploadDone() {
    if (!gFirmwareUploadSucceeded) {
      String message = gFirmwareUploadMessage.length() ? gFirmwareUploadMessage : "Firmware upload failed.";
      gServer.send(400, "text/html; charset=utf-8", configPageHtml(message));
      return;
    }

    gServer.send(200, "text/plain; charset=utf-8", "Firmware uploaded successfully. Rebooting...");
    delay(1000);
    ESP.restart();
  }

  void clearStoredWifi() {
    if (!gPrefs.begin("wifi", false)) return;
    gPrefs.clear();
    gPrefs.end();
  }

  bool loadStoredWifi(String& ssid, String& pass) {
    if (!gPrefs.begin("wifi", true)) return false;
    uint32_t version = gPrefs.getUInt("version", 0);
    ssid = gPrefs.getString("ssid", "");
    pass = gPrefs.getString("pass", "");
    gPrefs.end();
    ssid.trim();
    if (!ssid.length() && !pass.length()) return false;
    if (version != kStorageVersion ||
        ssid.length() > kMaxStoredWifiSsidBytes ||
        pass.length() > kMaxStoredWifiPassBytes ||
        !isSingleLineFieldValid(ssid, kMaxStoredWifiSsidBytes) ||
        !isSingleLineFieldValid(pass, kMaxStoredWifiPassBytes)) {
      clearStoredWifi();
      ssid = "";
      pass = "";
      return false;
    }
    return ssid.length() > 0;
  }

  void saveStoredWifi(const String& ssid, const String& pass) {
    if (!gPrefs.begin("wifi", false)) return;
    gPrefs.putUInt("version", kStorageVersion);
    gPrefs.putString("ssid", ssid);
    gPrefs.putString("pass", pass);
    gPrefs.end();
  }

  // Neumorphic design system shared with the NTS Radio panels: one #e0e0e0
  // surface, a single light source from the top-left, no borders, no accent
  // colours, no bold. Both pages pull their chrome from here so they cannot
  // drift apart — see that project's readmes/DESIGN_SYSTEM.md for the tokens.
  String pageHead(const __FlashStringHelper* title) {
    String html;
    html.reserve(2200);
    html += F("<!doctype html><html lang='en'><head><meta charset='utf-8'>");
    html += F("<meta name='viewport' content='width=device-width,initial-scale=1'><title>");
    html += title;
    html += F("</title><style>");
    html += F(":root{--bg:#e0e0e0;--text:#2b2b2b;--sh:#bebebe;--sh2:#afafaf}");
    html += F("*{box-sizing:border-box}");
    html += F("body{margin:0;font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,Helvetica,Arial,sans-serif;");
    html += F("font-size:15px;font-weight:400;background:var(--bg);color:var(--text);");
    html += F("display:flex;flex-direction:column;align-items:center;padding:40px 16px 60px;min-height:100vh}");
    html += F(".wrap{width:100%;max-width:420px;display:flex;flex-direction:column;gap:20px}");
    html += F(".card{background:var(--bg);border-radius:22px;padding:20px 22px;");
    html += F("box-shadow:9px 9px 20px var(--sh),-4px -4px 10px #fff}");
    html += F("h1{font-size:20px;font-weight:700;margin:0 0 16px}");
    html += F(".label{font-size:15px;font-weight:400;margin-bottom:16px}");
    html += F(".hint{font-size:15px;font-weight:400;color:#555;line-height:1.45;margin-bottom:16px}");
    html += F(".row{display:flex;align-items:center;justify-content:space-between;gap:10px;margin-bottom:16px}");
    html += F(".row.end{justify-content:flex-end}");
    html += F(".card>*:last-child,form>*:last-child{margin-bottom:0}");
    html += F("#preview label{display:flex;align-items:center;gap:10px;margin:0 0 10px}");
    html += F("#preview label:last-child{margin-bottom:0}");
    html += F("button{font:inherit;font-weight:400;color:var(--text);cursor:pointer;border:none;background:var(--bg);");
    html += F("border-radius:14px;padding:12px 22px;box-shadow:6px 6px 13px var(--sh),-3px -3px 7px #fff;");
    html += F("transition:box-shadow .12s,transform .12s}");
    html += F("button:active{box-shadow:inset 5px 5px 10px var(--sh2),inset -2px -2px 6px #fff;transform:translateY(1px)}");
    html += F("button:disabled{opacity:.55;cursor:default}");
    html += F("input,select,textarea{display:block;width:100%;font:inherit;font-weight:400;color:var(--text);");
    html += F("border:none;background:var(--bg);border-radius:14px;padding:12px 14px;margin-bottom:16px;");
    html += F("box-shadow:inset 5px 5px 10px var(--sh),inset -3px -3px 7px #fff;-webkit-appearance:none;appearance:none}");
    html += F("input:focus,select:focus,textarea:focus{outline:none;");
    html += F("box-shadow:inset 6px 6px 12px var(--sh2),inset -3px -3px 7px #fff}");
    html += F("textarea{height:150px;resize:vertical;line-height:1.5;font-family:ui-monospace,Menlo,Consolas,monospace;font-size:13px}");
    // The native file input is hidden: a regular button opens it and the page shows
    // the picked file name, so there's no browser "no file chosen" text.
    html += F("input[type=file]{position:absolute;width:1px;height:1px;padding:0;margin:0;opacity:0;overflow:hidden}");
    html += F("input[type=checkbox]{display:inline-block;width:auto;margin:0;padding:0;box-shadow:none;");
    html += F("-webkit-appearance:auto;appearance:auto;accent-color:#8a8a8a}");
    html += F("form{margin:0}");
    html += F(".msg{border-radius:14px;padding:12px 14px;margin-bottom:16px;");
    html += F("box-shadow:inset 5px 5px 10px var(--sh),inset -3px -3px 7px #fff}");
    html += F("code{font-family:ui-monospace,Menlo,Consolas,monospace;font-size:13px}");
    html += F("</style></head><body><div class='wrap'>");
    return html;
  }

  const __FlashStringHelper* pageFoot() {
    return F("</div></body></html>");
  }

  // A scan blocks for a few seconds, and phones fire several captive-portal
  // probes that all land on the setup page, so scan once at portal start and
  // again only when asked to.
  void scanSetupNetworks() {
    gSetupSsidCount = 0;
    int found = WiFi.scanNetworks();
    for (int i = 0; i < found && gSetupSsidCount < kMaxSetupSsids; i++) {
      String ssid = WiFi.SSID(i);
      if (!ssid.length()) continue;
      bool duplicate = false;  // mesh networks report one SSID per access point
      for (size_t j = 0; j < gSetupSsidCount; j++) {
        if (gSetupSsids[j] == ssid) { duplicate = true; break; }
      }
      if (!duplicate) gSetupSsids[gSetupSsidCount++] = ssid;
    }
    WiFi.scanDelete();
  }

  String wifiSetupPageHtml(const String& message = "") {
    String html = pageHead(F("Tramagotchi Setup"));
    html.reserve(html.length() + 2400);

    html += F("<div class='card'><h1>Tramagotchi</h1>");
    html += F("<div class='hint'>by Officina Canfora</div>");
    html += F("<div class='hint'>Enter the WiFi network this device should use. ");
    html += F("It will save the credentials and restart.</div>");
    if (message.length()) {
      html += F("<div class='msg'>");
      html += escapeHtml(message);
      html += F("</div>");
    }
    html += F("</div>");

    html += F("<form method='post' action='/wifi-save' accept-charset='UTF-8'><div class='card'>");
    html += F("<div class='label'>Available WiFi networks</div>");
    if (gSetupSsidCount > 0) {
      html += F("<select id='ssid_select' onchange='document.getElementById(\"ssid\").value=this.value'>");
      html += F("<option value=''>Choose a network...</option>");
      for (size_t i = 0; i < gSetupSsidCount; i++) {
        html += F("<option value='");
        html += escapeHtml(gSetupSsids[i]);
        html += F("'>");
        html += escapeHtml(gSetupSsids[i]);
        html += F("</option>");
      }
      html += F("</select>");
    } else {
      html += F("<div class='hint'>No networks found. You can still type the SSID manually.</div>");
    }
    html += F("<div class='label'>WiFi name / SSID</div><input id='ssid' name='ssid' required autofocus>");
    html += F("<div class='label'>Password</div><input name='pass' type='password'>");
    html += F("<div class='row end'><button type='button' onclick='location.href=\"/?rescan=1\"'>Scan again</button>");
    html += F("<button type='submit'>Save and restart</button></div>");
    html += F("</div></form>");

    html += F("<div class='card'><div class='hint'>Setup address: http://192.168.4.1/</div></div>");
    html += pageFoot();
    return html;
  }

  void handleWifiSetupRoot() {
    if (gServer.hasArg("rescan")) scanSetupNetworks();
    gServer.send(200, "text/html; charset=utf-8", wifiSetupPageHtml());
  }

  void handleWifiSave() {
    String ssid = gServer.arg("ssid");
    String pass = gServer.arg("pass");
    ssid.trim();
    if (!ssid.length() ||
        ssid.length() > kMaxStoredWifiSsidBytes ||
        pass.length() > kMaxStoredWifiPassBytes ||
        !isSingleLineFieldValid(ssid, kMaxStoredWifiSsidBytes) ||
        !isSingleLineFieldValid(pass, kMaxStoredWifiPassBytes)) {
      gServer.send(400, "text/html; charset=utf-8", wifiSetupPageHtml("Invalid WiFi credentials format."));
      return;
    }

    saveStoredWifi(ssid, pass);
    gServer.send(200, "text/html; charset=utf-8", wifiSetupPageHtml("Saved. Restarting now."));
    delay(700);
    ESP.restart();
  }

  void startWifiSetupPortal() {
#if WIFI_SETUP_AP_ENABLED
    IPAddress apIp(WIFI_SETUP_AP_IP);
    WiFi.disconnect(true);
    WiFi.mode(WIFI_AP_STA);
    display_status("Scanning WiFi");
    scanSetupNetworks();  // before the AP is up, so no client is waiting on it
    WiFi.softAPConfig(apIp, apIp, IPAddress(255, 255, 255, 0));
    const char* apPass = WIFI_SETUP_AP_PASS;
    bool started = apPass && apPass[0]
      ? WiFi.softAP(WIFI_SETUP_AP_SSID, apPass)
      : WiFi.softAP(WIFI_SETUP_AP_SSID);
    if (!started) {
      display_status(WIFI_FAILED_TEXT);
      return;
    }

    gServer.on("/", HTTP_GET, handleWifiSetupRoot);
    gServer.on("/wifi-save", HTTP_POST, handleWifiSave);
    gServer.onNotFound(handleWifiSetupRoot);
    gServer.begin();
    gDnsServer.start(53, "*", apIp);
    gSetupPortalActive = true;
    display_status("Setup WiFi");
#else
    display_status(WIFI_FAILED_TEXT);
#endif
  }

  String configPageHtml(const String& message = "") {
    String html = pageHead(F("Tramagotchi Config"));
    html.reserve(html.length() + 11000);

    html += F("<div class='card'><h1>Tramagotchi</h1>");
    html += F("<div class='hint'>by Officina Canfora</div>");
    if (message.length()) {
      html += F("<div class='msg'>");
      html += escapeHtml(message);
      html += F("</div>");
    }
    html += F("</div>");

    html += F("<div class='card'><div class='label'>Line</div><select id='lines'></select>");
    html += F("<div class='label'>Stop</div><select id='stops'><option value=''>Choose a line first</option></select>");
    html += F("<div class='label'>Directions</div>");
    html += F("<div id='preview' class='hint'>Select a line and stop to list its directions.</div>");
    html += F("<div class='row end'><button type='button' onclick='addPreviewLines()'>Add</button></div>");
    html += F("<div class='hint' id='addHint'></div></div>");

    html += F("<div class='card'><div class='label'>Station list</div>");
    html += F("<form method='post' action='/save' accept-charset='UTF-8'><textarea id='list' name='list'>");
    html += escapeHtml(stationListText());
    html += F("</textarea><div class='row end'><button type='submit'>Save</button></div></form></div>");

    html += F("<div class='card'><div class='label'>Stop data (");
    html += gDataVersion.length() ? escapeHtml(gDataVersion) : String(F("version unknown"));
    html += F(")</div><form method='post' action='/data-update' ");
    html += F("onsubmit='const b=this.querySelector(\"button\");b.disabled=true;b.textContent=\"Updating...\"'>");
    html += F("<div class='row end'><button type='submit'>Update stop data</button></div></form></div>");

    html += F("<div class='card'><div class='label'>Firmware OTA upload (.bin)</div>");
    html += F("<form id='fwForm' method='post' action='/firmware' enctype='multipart/form-data'>");
    html += F("<input type='file' id='fw' name='file' accept='.bin'>");
    html += F("<div class='row'><button type='button' onclick='document.getElementById(\"fw\").click()'>Choose file</button>");
    html += F("<button type='submit'>Upload firmware</button></div><div id='fwName' class='hint'></div></form></div>");

    html += F("<div class='card'><div class='label'>Device</div><div class='hint'>Current IP: ");
    html += WiFi.localIP().toString();
    html += F("</div></div>");
    html += F("<script>");
    html += F("let allLines=[],allStops=[],previewRows=[];const lineSel=document.getElementById('lines'),sel=document.getElementById('stops'),list=document.getElementById('list'),preview=document.getElementById('preview');");
    html += F("function loadText(u){return fetch(u).then(r=>r.text().then(t=>{if(!r.ok)throw new Error(u+': '+t);return t}))}function dataFail(){stopPlaceholder('No stop data');preview.textContent='Data files missing. Upload the LittleFS image.'}");
    html += F("loadText('/lines').then(t=>{allLines=t.trim().split('\\n').map(l=>l.trim()).filter(Boolean).map(l=>{const p=l.indexOf('|');return{id:l.slice(0,p),n:l.slice(p+1)}});renderLines()}).catch(dataFail);");
    html += F("function loadStops(){const lineId=lineSel.value;allStops=[];previewRows=[];preview.textContent='Select a line and stop to list its directions.';if(!lineId){stopPlaceholder('Choose a line first');return}stopPlaceholder('Loading stops...');loadText('/stops?lineId='+encodeURIComponent(lineId)).then(t=>{if(lineSel.value!==lineId)return;allStops=[];t.trim().split('\\n').forEach(l=>{l=l.trim();if(!l)return;const p=l.indexOf('|'),p2=l.indexOf('|',p+1),p3=l.indexOf('|',p2+1);if(p>0&&p2>p)allStops.push({line:lineId,r:l.slice(0,p),n:l.slice(p+1,p2),d:p3>p2?l.slice(p2+1,p3):l.slice(p2+1)})});render()}).catch(dataFail)}");
    html += F("const hiddenStopPatterns=['abstellplatz','hastus','umleitung'];");
    html += F("function visibleStop(s){const n=s.n.toLowerCase();return !hiddenStopPatterns.some(p=>n.includes(p))}");
    html += F("function renderLines(){lineSel.innerHTML='<option value=\"\">Choose a line...</option>';allLines.forEach(l=>{const o=document.createElement('option');o.value=l.id;o.textContent=l.n;lineSel.appendChild(o)})}");
    html += F("function stopPlaceholder(text){sel.innerHTML='';const o=document.createElement('option');o.value='';o.textContent=text;sel.appendChild(o)}");
    html += F("function render(){stopPlaceholder('Pick a station...');previewRows=[];preview.textContent='Select a stop to list its directions.';const lineId=lineSel.value,seen={};for(const s of allStops){if(s.line!==lineId||!visibleStop(s)||seen[s.n])continue;seen[s.n]=1;");
    html += F("const o=document.createElement('option');o.value=s.n;o.textContent=s.n;sel.appendChild(o)}}");
    html += F("lineSel.addEventListener('change',loadStops);");
    html += F("function esc(s){return s.replaceAll('&','&amp;').replaceAll('<','&lt;').replaceAll('>','&gt;')}");
    html += F("function renderPreview(status){let h=status?'<div class=\"hint\">'+esc(status)+'</div>':'';previewRows.forEach((r,i)=>{const label=r.towards?esc(r.line)+' -> '+esc(r.towards):esc(r.line);h+='<label><input type=\"checkbox\" class=\"pr\" value=\"'+i+'\" checked> '+label+'</label><br>'});preview.innerHTML=h||'No preview rows.'}");
    html += F("sel.addEventListener('change',buildDirections);function lineName(){const o=lineSel.options[lineSel.selectedIndex];return o?o.textContent:''}");
    html += F("function buildDirections(){const o=sel.options[sel.selectedIndex];if(!o||!o.value){previewRows=[];preview.textContent='Select a stop to list its directions.';return}const name=o.value,lineId=lineSel.value,ln=lineName();const dirs=[];allStops.forEach(s=>{if(s.line!==lineId||s.n!==name)return;const exact=s.d.indexOf('~')<0;s.d.split('~').forEach(t=>{t=t.trim();if(!t)return;let e=dirs.find(x=>x.t===t);if(!e)dirs.push({t:t,rbl:s.r,exact:exact});else if(exact&&!e.exact){e.rbl=s.r;e.exact=true}})});previewRows=dirs.map(d=>({rbl:d.rbl,line:ln,towards:d.t}));renderPreview(previewRows.length?'Directions from stop data. Tick and Add.':'No direction data for this stop.')}");
    html += F("function ascii(v){return v.replaceAll('\\u00e4','ae').replaceAll('\\u00f6','oe').replaceAll('\\u00fc','ue').replaceAll('\\u00c4','Ae').replaceAll('\\u00d6','Oe').replaceAll('\\u00dc','Ue').replaceAll('\\u00df','ss')}");
    html += F("function clean(v){return ascii(v).replaceAll(',',' ').replace(/\\s+/g,' ').trim()}");
    html += F("function stripDir(v){return v.trim().split(/\\s+/).filter(w=>w!=='U'&&w!=='S').join(' ').replace(/,+$/,'')}");
    html += F("function appendRow(row){list.value=(list.value.trim()?list.value.trim()+'\\n':'')+row}");
    html += F("list.form.addEventListener('submit',()=>{list.value=list.value.split('\\n').map(r=>r.split(',').map(clean).join(',')).join('\\n')});");
    html += F("function addPreviewLines(){const o=sel.options[sel.selectedIndex];if(!o||!o.value)return;");
    html += F("if(!previewRows.length){document.getElementById('addHint').textContent='No preview rows available yet.';return;}");
    html += F("let rows=[];document.querySelectorAll('.pr:checked').forEach(cb=>{const r=previewRows[+cb.value];if(/^\\d+$/.test(r.rbl))rows.push(clean(r.rbl)+','+clean(o.textContent)+','+clean(r.line)+','+clean(stripDir(r.towards)))});");
    html += F("if(rows.length){list.value=(list.value.trim()?list.value.trim()+'\\n':'')+rows.join('\\n');document.getElementById('addHint').textContent='Added '+rows.length+' row(s).'}}");
    html += F("const fw=document.getElementById('fw'),fwName=document.getElementById('fwName');");
    html += F("fw.addEventListener('change',()=>{fwName.textContent=fw.files.length?fw.files[0].name:''});");
    html += F("document.getElementById('fwForm').addEventListener('submit',e=>{if(!fw.files.length){e.preventDefault();fwName.textContent='Choose a .bin file first.'}});");
    html += F("</script>");
    html += pageFoot();
    return html;
  }

  void handleConfigRoot() {
    gServer.send(200, "text/html; charset=utf-8", configPageHtml());
  }

  void handleConfigStatus() {
    String body;
    body.reserve(256);
    body += F("name=");
    body += DEVICE_LOCAL_HOSTNAME;
    body += F(".local\nip=");
    body += WiFi.localIP().toString();
    body += F("\nrssi=");
    body += WiFi.RSSI();
    body += F("\nstations=");
    body += stationCount();
    body += F("\nmode=");
    body += STATION_CONFIG_MODE;
    body += F("\n");
    if (gStationMutex) xSemaphoreTake(gStationMutex, portMAX_DELAY);
    for (size_t i = 0; i < gStationCount; i++) {
      body += F("station=");
      body += gStations[i].rbl;
      body += F(",");
      body += gStations[i].line;
      body += F(",");
      body += stationStateText(gStationState[i]);
      body += F("\n");
    }
    if (gStationMutex) xSemaphoreGive(gStationMutex);
    gServer.send(200, "text/plain; charset=utf-8", body);
  }

  // Per-line files avoid scanning or transferring the full stop dataset.
  bool streamLineStops(const String& lineId) {
    if (!gDataFsReady || !isDigitsOnly(lineId)) return false;
    String path = "/stops/";
    path += lineId;
    path += ".csv";
    File file = LittleFS.open(path, "r");
    if (!file) return false;
    gServer.streamFile(file, "text/plain; charset=utf-8");
    file.close();
    return true;
  }

  void handleStopOptions() {
    String lineId = gServer.arg("lineId");
    lineId.trim();
    if (!lineId.length()) {
      gServer.send(400, "text/plain", "Missing lineId.");
      return;
    }
    if (!streamLineStops(lineId)) {
      gServer.send(503, "text/plain", "LittleFS data missing. Run pio run -t uploadfs.");
    }
  }

  void handleLineOptions() {
    if (!sendDataFile("/line_options.csv")) {
      gServer.send(503, "text/plain", "LittleFS data missing. Run pio run -t uploadfs.");
    }
  }

  void handleConfigSave() {
    String text = gServer.arg("list");
    if (text.length() > kMaxStoredStationBytes) {
      gServer.send(400, "text/html; charset=utf-8", configPageHtml("Station list is too large for safe storage."));
      return;
    }
    Station parsed[kMaxStationCount];
    size_t count = parseStationList(text, parsed, kMaxStationCount);
    String trimmed = text;
    trimmed.trim();
    // An empty list is a valid choice (clears all stops); text without a single
    // valid row is a typo and must not wipe the saved list.
    if (!count && trimmed.length()) {
      gServer.send(400, "text/html; charset=utf-8", configPageHtml("No valid RBL entries found. Nothing saved."));
      return;
    }

    if (gStationMutex) xSemaphoreTake(gStationMutex, portMAX_DELAY);
    gStationCount = count;
    for (size_t i = 0; i < count; i++) gStations[i] = parsed[i];
    if (gStationMutex) xSemaphoreGive(gStationMutex);

    if (count) saveStations(stationListText());
    else clearStoredStations();  // falls back to STATION_LIST, which is empty by default
    stationIndex = 0;
    for (size_t i = 0; i < kMaxStationCount; i++) failCycles[i] = 0;
    lastFetchMs = millis() - kFetchIntervalMs;
    lastCycleMs = millis();
    fetchFailStreakMs = 0;
    noDataVisible = false;
    graceShown = false;
    gFetchRequested = false;
    if (gMutex) {
      xSemaphoreTake(gMutex, portMAX_DELAY);
      gPending.fresh = false;
      gPending.succeeded = false;
      xSemaphoreGive(gMutex);
    }
    display_show_loading();
    gServer.send(200, "text/html; charset=utf-8", configPageHtml(count
      ? "Saved. Display will use the new station list."
      : "Station list cleared."));
  }

  // ── Stop data update (DATA_UPDATE_URL) ───────────────────────────────────────
  // The source folder holds version.txt (one line, e.g. "2026-11-01") and
  // littlefs.bin, the image `pio run -t buildfs` builds for this partition table.
  // The image is only downloaded when its version differs from the installed one.
  String readDataVersion() {
    if (!gDataFsReady) return "";
    File file = LittleFS.open("/version.txt", "r");
    if (!file) return "";
    String version = file.readStringUntil('\n');
    file.close();
    version.trim();
    return version;
  }

  bool fetchText(const String& url, String& out) {
    WiFiClientSecure secureClient;
    WiFiClient plainClient;
    const bool https = url.startsWith("https://");
    if (https) secureClient.setInsecure();  // public data, same as the departures API
    WiFiClient& client = https ? static_cast<WiFiClient&>(secureClient) : plainClient;

    HTTPClient http;
    http.setTimeout(kRequestTimeoutMs);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    if (!http.begin(client, url)) return false;
    http.setUserAgent(kUserAgent);
    int code = http.GET();
    if (code == HTTP_CODE_OK) out = http.getString();
    http.end();
    out.trim();
    return code == HTTP_CODE_OK;
  }

  // Runs on the main task, so the display freezes on a status screen while the
  // image downloads (well under a minute). Returns a message for the config page.
  String runDataUpdate(String baseUrl) {
    baseUrl.trim();
    if (!baseUrl.length()) return "No update source set yet.";
    if (WiFi.status() != WL_CONNECTED) return "WiFi is not connected.";
    if (!baseUrl.endsWith("/")) baseUrl += "/";

    String remoteVersion;
    if (!fetchText(baseUrl + "version.txt", remoteVersion) || !remoteVersion.length()) {
      return "Could not reach the update source.";
    }
    if (remoteVersion == gDataVersion) {
      return "Stop data is up to date (" + gDataVersion + ").";
    }

    LOGF("[data] updating %s -> %s\n", gDataVersion.c_str(), remoteVersion.c_str());
    display_status("updating data");
    if (gDataFsReady) {
      LittleFS.end();  // the partition is rewritten underneath, so unmount first
      gDataFsReady = false;
    }

    WiFiClientSecure secureClient;
    WiFiClient plainClient;
    const bool https = baseUrl.startsWith("https://");
    if (https) secureClient.setInsecure();
    WiFiClient& client = https ? static_cast<WiFiClient&>(secureClient) : plainClient;
    httpUpdate.rebootOnUpdate(false);
    httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    t_httpUpdate_return result = httpUpdate.updateSpiffs(client, baseUrl + "littlefs.bin");

    mountDataFs(false);  // never format here: a failed download is fixed by retrying
    gDataVersion = readDataVersion();
    if (kawaiiActive) display_show_kawaii();
    else redrawCurrentScreen();

    if (result == HTTP_UPDATE_OK) return "Stop data updated (" + gDataVersion + ").";
    return "Stop data update failed: " + httpUpdate.getLastErrorString() + ". Try again.";
  }

  void handleDataUpdate() {
    // Optional "base" parameter: try another source (e.g. a test server) without
    // reflashing. No new exposure: /fsimage already accepts any image.
    String base = gServer.arg("base");
    base.trim();
    String message = runDataUpdate(base.length() ? base : String(DATA_UPDATE_URL));
    LOGF("[data] %s\n", message.c_str());
    gServer.send(200, "text/html; charset=utf-8", configPageHtml(message));
  }

  void startConfigServer() {
    if (STATION_CONFIG_MODE != 1 || WiFi.status() != WL_CONNECTED) return;
    gServer.on("/", HTTP_GET, handleConfigRoot);
    gServer.on("/config", HTTP_GET, handleConfigRoot);
    gServer.on("/status", HTTP_GET, handleConfigStatus);
    gServer.on("/lines", HTTP_GET, handleLineOptions);
    gServer.on("/stops", HTTP_GET, handleStopOptions);
    gServer.on("/save", HTTP_POST, handleConfigSave);
    gServer.on("/data-update", HTTP_POST, handleDataUpdate);
    gServer.on("/upload", HTTP_POST, handleUploadDone, handleUploadFile);
    gServer.on("/upload-file", HTTP_POST, handleUploadDonePlain, handleUploadFile);
    gServer.on("/fsimage", HTTP_POST, handleFsImageUploadDone, handleFsImageUpload);
    gServer.on("/firmware", HTTP_POST, handleFirmwareUploadDone, handleFirmwareUpload);
    gServer.onNotFound(handleConfigRoot);
    gServer.begin();
  }
#endif
#endif

  void waitWithLoading(uint32_t durationMs) {
    uint32_t start = millis();
    while (millis() - start < durationMs) {
      display_update_animation();
      delay(kLoadingTickMs);
    }
  }

  void applyOverrides(Departure& dep, const Station& s) {
    if (dep.countdown < 0) return;
    if (s.line.length()) dep.line = s.line;
    if (s.towards.length()) dep.towards = s.towards;
  }

  void applyStationLabels(String& line, String& towards, const Station& s) {
    if (s.line.length()) line = s.line;
    if (s.towards.length()) towards = s.towards;
  }

  bool equalsIgnoreCase(const String& value, const String& expected) {
    if (!expected.length()) return true;
    return normalizeMatchText(value) == normalizeMatchText(expected);
  }

  bool containsIgnoreCase(const String& value, const String& expected) {
    if (!expected.length()) return true;
    String haystack = normalizeMatchText(value);
    String needle = normalizeMatchText(expected);
    return haystack.indexOf(needle) >= 0;
  }

  bool departureIsBlacklisted(const String& lineName, const String& towards) {
    String combined = lineName;
    combined += " ";
    combined += towards;
    for (size_t i = 0; kDepartureBlacklist[i]; i++) {
      const char* pattern = kDepartureBlacklist[i];
      if (!pattern || !pattern[0]) continue;
      if (containsIgnoreCase(combined, String(pattern))) return true;
    }
    return false;
  }

  // The RBL already pins down platform + direction, so only the line name is
  // matched. The stored towards is a display label, never a filter — matching
  // it against live API text proved fragile (marker suffixes, renamings).
  bool stationLineMatches(const Station& s, const String& lineName) {
    return equalsIgnoreCase(lineName, s.line);
  }

  struct StationResult {
    Departure    first;
    Departure    second;
    Departure    label;   // line/direction for the no-data screen
    bool         ok;
    NoDataReason reason;
  };

  // ── Fetch every station in one request (called from fetch task only) ───────
  // The API accepts repeated rbl= parameters, so all stops cost a single TLS
  // handshake per cycle instead of one each. Monitors are attributed back to
  // stations via locationStop.properties.attributes.rbl; stops with nothing
  // running are simply absent from the response.
  //
  // Distinguishing the failure kinds matters: a filter that matches nothing
  // looks identical to a dead network unless they are reported separately.
  void fetchAllStations(StationResult* out, size_t count) {
    for (size_t i = 0; i < count; i++) {
      out[i] = {{"", "", -1}, {"", "", -1}, {"", "", -1}, false, NoDataNetwork};
    }
    if (!count || WiFi.status() != WL_CONNECTED) return;

    Station stations[kMaxStationCount];
    if (gStationMutex) xSemaphoreTake(gStationMutex, portMAX_DELAY);
    for (size_t i = 0; i < count; i++) stations[i] = gStations[i];
    if (gStationMutex) xSemaphoreGive(gStationMutex);

    // Several rows may share an RBL (same platform, different line): ask once.
    String url = "https://www.wienerlinien.at/ogd_realtime/monitor?";
    size_t appended = 0;
    for (size_t i = 0; i < count; i++) {
      bool duplicate = false;
      for (size_t j = 0; j < i; j++) {
        if (stations[j].rbl == stations[i].rbl) { duplicate = true; break; }
      }
      if (duplicate) continue;
      if (appended++) url += "&";
      url += "rbl=";
      url += stations[i].rbl;
    }
    if (!appended) return;

    LOGF("[fetch] GET %s\n", url.c_str());

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.setTimeout(kRequestTimeoutMs);
    if (!http.begin(client, url)) { LOGF("[fetch] http.begin failed\n"); return; }
    http.setUserAgent(kUserAgent);

    uint32_t t0 = millis();
    int code = http.GET();
    LOGF("[fetch] HTTP %d in %lu ms\n", code, (unsigned long)(millis() - t0));
    if (code != HTTP_CODE_OK) { http.end(); return; }

    // Keep only the fields we read, so a verbose response can't exhaust the heap.
    JsonDocument filter;
    filter["data"]["monitors"][0]["locationStop"]["properties"]["attributes"]["rbl"] = true;
    filter["data"]["monitors"][0]["lines"][0]["name"] = true;
    filter["data"]["monitors"][0]["lines"][0]["towards"] = true;
    filter["data"]["monitors"][0]["lines"][0]["departures"]["departure"][0]["departureTime"]["countdown"] = true;

    // Stream straight from the socket so the whole body isn't held in RAM alongside the doc.
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, http.getStream(),
        DeserializationOption::Filter(filter), DeserializationOption::NestingLimit(64));
    http.end();
    if (err) { LOGF("[fetch] JSON error: %s\n", err.c_str()); return; }

    JsonObject data = doc["data"];
    if (data.isNull()) { LOGF("[fetch] no data object\n"); return; }
    JsonArray monitors = data["monitors"];
    if (monitors.isNull()) { LOGF("[fetch] no monitors array\n"); return; }
    LOGF("[fetch] monitors: %u\n", (unsigned)monitors.size());

    // The request itself worked, so from here on an empty result is a service
    // or filter question, never a network one.
    for (size_t i = 0; i < count; i++) out[i].reason = NoDataNoService;

    for (size_t i = 0; i < count; i++) {
      const Station& s = stations[i];
      const long wantRbl = s.rbl.toInt();
      bool sawAnyLine = false;      // stop reported something, matching or not
      bool sawMatchingLine = false; // the configured line is served here
      Departure best   = {"", "", -1};
      Departure second = {"", "", -1};

      for (JsonObject monitor : monitors) {
        // rbl comes back as a number, so it has to be read as one.
        long monitorRbl = monitor["locationStop"]["properties"]["attributes"]["rbl"] | 0L;
        if (monitorRbl != wantRbl) continue;

        JsonArray lines = monitor["lines"];
        if (lines.isNull()) continue;
        for (JsonObject line : lines) {
          String lineName = line["name"]    | "";
          String towards  = stripTransferMarkers(line["towards"] | "");
          if (departureIsBlacklisted(lineName, towards)) continue;
          sawAnyLine = true;
          if (!stationLineMatches(s, lineName)) continue;
          sawMatchingLine = true;

          if (!out[i].label.line.length() && !out[i].label.towards.length()) {
            out[i].label = {lineName, towards, -1};
            applyStationLabels(out[i].label.line, out[i].label.towards, s);
          }

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
        out[i].first  = best;
        out[i].second = second;
        out[i].ok     = true;
        out[i].reason = NoDataUnknown;
      } else if (sawMatchingLine) {
        out[i].reason = NoDataNoService;
      } else if (sawAnyLine) {
        out[i].reason = NoDataNoMatch;
      }

      if (out[i].ok) {
        LOGF("[fetch]   rbl=%s line=%s -> %s %s in %d min%s\n",
             s.rbl.c_str(), s.line.c_str(), out[i].first.line.c_str(),
             out[i].first.towards.c_str(), out[i].first.countdown,
             out[i].second.countdown >= 0 ? "" : " (no second departure)");
      } else {
        LOGF("[fetch]   rbl=%s line=%s -> %s\n", s.rbl.c_str(), s.line.c_str(),
             stationStateText(stateFromReason(out[i].reason)));
      }
    }
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
        result.noDataLine = "";
        result.noDataTowards = "";
        result.reason     = NoDataUnknown;
        result.fresh      = false;

        size_t count = stationCount();
        if (count > kMaxStationCount) count = kMaxStationCount;

        StationResult results[kMaxStationCount];
        fetchAllStations(results, count);
        for (size_t i = 0; i < count; i++) {
          gStationState[i] = results[i].ok ? StationOk : stateFromReason(results[i].reason);
        }

        // Show the requested station if it has data, otherwise the next one that
        // does — same behaviour as before, now off a single batched response.
        for (size_t attempt = 0; attempt < count; attempt++) {
          size_t idx = (startIdx + attempt) % count;
          if (results[idx].ok) {
            result.first      = results[idx].first;
            result.second     = results[idx].second;
            result.haveSecond = result.second.countdown >= 0;
            result.succeeded  = true;
            result.stationIdx = idx;
            break;
          }
          if (idx == startIdx) {
            result.noDataLine = results[idx].label.line;
            result.noDataTowards = results[idx].label.towards;
            result.reason = results[idx].reason;
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

  bool tryConnectWifi(const char* ssid, const char* pass) {
    if (!ssid || !ssid[0]) return false;
    for (uint32_t attempt = 0; attempt < WIFI_ATTEMPTS_PER_NETWORK; attempt++) {
      WiFi.begin(ssid, pass);
      uint32_t t0 = millis();
      while (WiFi.status() != WL_CONNECTED && millis() - t0 < kWifiTimeoutMs) {
        waitWithLoading(kLoadingTickMs);
      }
      if (WiFi.status() == WL_CONNECTED) return true;
      WiFi.disconnect(false); waitWithLoading(500);
    }
    return false;
  }

  // ── WiFi (blocking, only called during setup) ──────────────────────────────
  bool connectWifi() {
    if (WiFi.status() == WL_CONNECTED) return true;
    uint32_t loadingStart = millis();
    display_show_loading();
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.setSleep(false);
    WiFi.disconnect(false); waitWithLoading(200);
#if WIFI_USE_STATIC_IP
    IPAddress localIp(WIFI_STATIC_LOCAL_IP);
    IPAddress gateway(WIFI_STATIC_GATEWAY);
    IPAddress subnet(WIFI_STATIC_SUBNET);
    IPAddress dns1(WIFI_STATIC_DNS1);
    IPAddress dns2(WIFI_STATIC_DNS2);
    WiFi.config(localIp, gateway, subnet, dns1, dns2);
#endif
    waitWithLoading(500);

    auto awaitMinLoading = [&]() {
      uint32_t elapsed = millis() - loadingStart;
      if (elapsed < LOADING_MIN_MS) waitWithLoading(LOADING_MIN_MS - elapsed);
    };

#if STATION_CONFIG_MODE == 1 && CONFIG_WEB_SERVER_ENABLED
    String storedSsid;
    String storedPass;
    if (loadStoredWifi(storedSsid, storedPass)) {
      if (tryConnectWifi(storedSsid.c_str(), storedPass.c_str())) {
        awaitMinLoading();
        display_status(WIFI_CONNECTED_TEXT); delay(500);
        return true;
      }
    }
#endif

    for (size_t offset = 0; offset < kWifiCredentialCount; offset++) {
      const WifiCredential& c = kWifiCredentials[offset];
      if (tryConnectWifi(c.ssid, c.pass)) {
        awaitMinLoading();
        display_status(WIFI_CONNECTED_TEXT); delay(500);
        return true;
      }
    }
    awaitMinLoading();
    display_status(WIFI_FAILED_TEXT); delay(2000);
    return false;
  }

  void startMdns() {
    if (WiFi.status() != WL_CONNECTED) return;
    if (!MDNS.begin(DEVICE_LOCAL_HOSTNAME)) return;
#if STATION_CONFIG_MODE == 1 && CONFIG_WEB_SERVER_ENABLED
    MDNS.addService("http", "tcp", 80);
#endif
  }

  void setupOta() {
    if (WiFi.status() != WL_CONNECTED) return;
    ArduinoOTA.setHostname(DEVICE_LOCAL_HOSTNAME);
    ArduinoOTA.onStart([]() {
      display_status("OTA update");
    });
    ArduinoOTA.onEnd([]() {
      display_status("rebooting");
    });
    ArduinoOTA.onError([](ota_error_t error) {
      String message = "OTA failed: ";
      if (error == OTA_AUTH_ERROR) message += "Auth failed";
      else if (error == OTA_BEGIN_ERROR) message += "Begin failed";
      else if (error == OTA_CONNECT_ERROR) message += "Connect failed";
      else if (error == OTA_RECEIVE_ERROR) message += "Receive failed";
      else if (error == OTA_END_ERROR) message += "End failed";
      else message += "Unknown";
      display_status(message.c_str());
    });
    ArduinoOTA.begin();
  }

  void redrawCurrentScreen() {
    if (noDataVisible) {
      Station s = stationAt(stationIndex);
      display_no_data(s.line.c_str(), s.towards.c_str(), noDataReason);
    } else if (everHadData) {
      display_departures(lastFirst, lastSecond, lastHaveSecond);
    } else {
      display_show_loading();
    }
  }
}

void setup() {
#if SERIAL_DEBUG_ENABLED
  Serial.begin(115200);
  delay(300);  // let the USB CDC link come up before the first log line
  LOGF("\n[boot] Tramagotchi starting\n");
#endif
  display_init();
#if SPLASH_ENABLED
  display_play_boot_splash(BOOT_SPLASH_DURATION_MS);
#endif

  gMutex = xSemaphoreCreateMutex();
  gStationMutex = xSemaphoreCreateMutex();
#if STATION_CONFIG_MODE == 1
  loadStoredStations();
#else
  loadDefaultStations();
#endif
#if STATION_CONFIG_MODE == 1 && CONFIG_WEB_SERVER_ENABLED
  initDataFs();
  gDataVersion = readDataVersion();
#endif

  bool wifiConnected = connectWifi();
  LOGF("[boot] WiFi %s, ip=%s, stations=%u\n",
       wifiConnected ? "connected" : "FAILED",
       WiFi.localIP().toString().c_str(), (unsigned)stationCount());
  if (wifiConnected) {
    startMdns();
    setupOta();
#if STATION_CONFIG_MODE == 1 && CONFIG_WEB_SERVER_ENABLED
    startConfigServer();
#endif
  } else {
#if STATION_CONFIG_MODE == 1 && CONFIG_WEB_SERVER_ENABLED
    startWifiSetupPortal();
#endif
  }

  // 12KB stack: fetchOne heap-allocates JSON doc, stack usage is modest
  xTaskCreate(fetchTask, "fetch", 12288, nullptr, 1, nullptr);

  lastFetchMs = millis() - kFetchIntervalMs;  // trigger fetch immediately
  lastCycleMs = millis();

#if !NO_BUTTON_ATTACHED
  // Pull the pin to the *idle* level so the released state is defined: a button
  // to 3V3 needs a pull-down, one to GND needs a pull-up.
  pinMode(TOUCH_PIN, TOUCH_ACTIVE_LOW ? INPUT_PULLUP : INPUT_PULLDOWN);
  lastRawButton  = (bool)digitalRead(TOUCH_PIN);
  stableButton   = TOUCH_ACTIVE_LOW ? !lastRawButton : lastRawButton;
  lastDebounceMs = millis();
  LOGF("[boot] button pin %d idle=%s -> %s\n", TOUCH_PIN,
       lastRawButton ? "HIGH" : "LOW", stableButton ? "PRESSED" : "released");
#endif
}

void loop() {
  uint32_t now = millis();

  // ── Button-cycle station ────────────────────────────────────────────────────
  size_t activeStationCount = stationCount();

#if STATION_CONFIG_MODE == 1 && CONFIG_WEB_SERVER_ENABLED
  gServer.handleClient();
  if (gSetupPortalActive) gDnsServer.processNextRequest();
  if (gSetupPortalActive) {
    delay(5);
    return;
  }
#endif

  if (WiFi.status() == WL_CONNECTED) {
    ArduinoOTA.handle();
  }

#if STATION_CONFIG_MODE == 1 && CONFIG_WEB_SERVER_ENABLED
  // ── Stop data: check the update source by itself now and then ───────────────
  // Only version.txt is fetched unless a newer image is published.
  if (DATA_UPDATE_AUTO && DATA_UPDATE_URL[0] && WiFi.status() == WL_CONNECTED &&
      now - gLastDataCheckMs >= (gDataCheckedOnce ? kDataCheckIntervalMs : kDataFirstCheckDelayMs)) {
    gLastDataCheckMs = now;
    gDataCheckedOnce = true;
    String message = runDataUpdate(DATA_UPDATE_URL);
    LOGF("[data] auto check: %s\n", message.c_str());
  }
#endif

  // ── Button cycling (short press cycles; long press toggles kawaii) ────────────
#if !NO_BUTTON_ATTACHED
  {
    bool raw = (bool)digitalRead(TOUCH_PIN);
    bool active = TOUCH_ACTIVE_LOW ? !raw : raw;
    if (raw != lastRawButton) {
      lastRawButton  = raw;
      lastDebounceMs = now;
    }
    if (now - lastDebounceMs >= kDebounceMs) {
      if (active != stableButton) {
        stableButton = active;
        if (active) {
          buttonPressMs = now;
          longPressHandled = false;
        } else {
          if (!longPressHandled && activeStationCount > 1 && !kawaiiActive) {
            LOGF("[button] short press -> station %u\n",
                 (unsigned)((stationIndex + 1) % activeStationCount));
            stationIndex = (stationIndex + 1) % activeStationCount;
            lastFetchMs  = 0;  // force immediate fetch for the new station
            fetchFailStreakMs = 0;
            graceShown        = false;
          }
        }
      }

#if !PURGE_EMOJI
      if (stableButton && !longPressHandled && now - buttonPressMs >= kKawaiiToggleLongMs) {
        longPressHandled = true;
        kawaiiActive = !kawaiiActive;
        LOGF("[button] long press -> kawaii %s\n", kawaiiActive ? "ON" : "OFF");
        if (kawaiiActive) {
          display_show_kawaii();
        } else {
          display_stop_kawaii();
          redrawCurrentScreen();
        }
      }
#endif
    }
  }
#endif  // !NO_BUTTON_ATTACHED

  // ── Auto-cycle station (advances index + triggers fetch, no blocking) ────────
  if (!kawaiiActive && activeStationCount > 1) {
    if (now - lastCycleMs >= STATION_CYCLE_INTERVAL_MS) {
      lastCycleMs  = now;
      stationIndex = (stationIndex + 1) % activeStationCount;
      lastFetchMs  = 0;  // force immediate fetch for the new station
      fetchFailStreakMs = 0;
      graceShown        = false;
    }
  }

  // ── Auto-cycle if no-data shown for the interval ────────────────────────────
  if (!kawaiiActive && noDataVisible && activeStationCount > 1) {
    if (now - noDataStartMs >= STATION_CYCLE_INTERVAL_MS) {
      stationIndex = (stationIndex + 1) % activeStationCount;
      lastCycleMs  = now;
      lastFetchMs  = 0;  // force immediate fetch for the new station
      noDataVisible = false;
      fetchFailStreakMs = 0;
      graceShown        = false;
    }
  }

  // ── Request fetch when due (just sets a flag, never blocks) ───────────────
  // Poll fast while a failure streak is unresolved and no-data isn't shown yet,
  // so a slightly-slow fetch can recover within the grace window.
  uint32_t fetchInterval =
      (fetchFailStreakMs != 0 && !noDataVisible) ? kRetryIntervalMs : kFetchIntervalMs;
  if (!kawaiiActive && !gFetchRequested && (now - lastFetchMs >= fetchInterval)) {
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
        failCycles[stationIndex] = 0;  // station is alive again: unmute it
        everHadData    = true;
        noDataVisible  = false;
        fetchFailStreakMs = 0;
        graceShown        = false;
        lastCycleMs    = millis();  // stay on this station for the full interval
        display_departures(lastFirst, lastSecond, lastHaveSecond);
      } else {
        size_t   failIdx = gPending.stationIdx;
        uint32_t nowMs   = millis();
        if (fetchFailStreakMs == 0) {
          // start of a failure streak: count one no-data cycle for this station
          fetchFailStreakMs = nowMs;
          if (failCycles[failIdx] < 255) failCycles[failIdx]++;
        }

        if (failCycles[failIdx] > kNoDataSkipCycles && activeStationCount > 1) {
          // No data for more than kNoDataSkipCycles cycles: skip this station's no-data
          // screen and move on. It stays in the rotation, so it keeps being retried and
          // is unmuted automatically once it returns departures again.
          noDataVisible     = false;
          graceShown        = false;
          fetchFailStreakMs = 0;
          stationIndex      = (failIdx + 1) % activeStationCount;
          lastCycleMs       = millis();
          lastFetchMs       = 0;  // fetch the next station immediately
        } else if (nowMs - fetchFailStreakMs >= kNoDataThresholdMs) {
          // threshold exceeded: commit to the no-data screen
          if (!noDataVisible) {
            noDataStartMs = nowMs;
            Station currentStation = stationAt(failIdx);
            const char* noDataLine = gPending.noDataLine.length()
              ? gPending.noDataLine.c_str()
              : currentStation.line.c_str();
            const char* noDataTowards = gPending.noDataTowards.length()
              ? gPending.noDataTowards.c_str()
              : currentStation.towards.c_str();
            noDataReason = gPending.reason;
            display_no_data(noDataLine, noDataTowards, noDataReason);
          }
          noDataVisible = true;
        } else {
          // grace window: do NOT show no-data yet
#if GRACE_DISPLAY_MODE == 1
          if (!graceShown) { display_show_loading(); graceShown = true; }
#endif
          // GRACE_DISPLAY_MODE == 0: hold previous screen — draw nothing
        }
      }
    }
    xSemaphoreGive(gMutex);
  }

  // ── Animation ──────────────────────────────────────────────────────────────
  bool skipNormal = false;

  // ── Persistent kawaii mode ──────────────────────────────────────────────────
  if (kawaiiActive) {
    display_update_kawaii_animation();
    skipNormal = true;
  }

#if POPUP_KAWAII_ENABLED
  if (!skipNormal) {
    static uint32_t sLastKawaiiMs = 0;
    if (!gKawaiiPopupActive && now - sLastKawaiiMs >= KAWAII_POPUP_INTERVAL_MS) {
      sLastKawaiiMs       = now;
      gKawaiiPopupActive  = true;
      gKawaiiPopupStartMs = now;
      display_show_kawaii();
    }
    if (gKawaiiPopupActive) {
      display_update_kawaii_animation();
      if (now - gKawaiiPopupStartMs >= KAWAII_POPUP_DURATION_MS) {
        gKawaiiPopupActive = false;
        display_stop_kawaii();
        if (noDataVisible) {
          Station s = stationAt(stationIndex);
          display_no_data(s.line.c_str(), s.towards.c_str(), noDataReason);
        } else if (everHadData) {
          display_departures(lastFirst, lastSecond, lastHaveSecond);
        }
      }
      skipNormal = true;
    }
  }
#endif

  // Keep the loading screen animated during the grace window (mode 1 only).
#if GRACE_DISPLAY_MODE == 1
  if (!skipNormal && fetchFailStreakMs != 0 && !noDataVisible) {
    display_update_animation();
    skipNormal = true;
  }
#endif

  if (!skipNormal) {
    if (noDataVisible) {
      display_update_no_data();
    } else if (lastFirst.countdown == 0 && everHadData) {
      display_update_animation();
    }
  }

  delay(5);
}
