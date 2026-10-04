#pragma once

// ── WiFi credentials ──────────────────────────────────────────────────────────
// Up to three networks (WIFI_SSID/_PASS, _2, _3), tried in order. They live in
// include/secrets.h, which is gitignored; copy secrets.example.h to start one.
// WIFI_SSID / WIFI_PASS can also be set via platformio.ini build flags.
#if __has_include("secrets.h")
#include "secrets.h"
#endif

// mDNS hostname — device is reachable at http://<hostname>.local/
#define DEVICE_LOCAL_HOSTNAME "Tramagotchi"

// ── WiFi connection behavior ──────────────────────────────────────────────────
// Keep WIFI_ATTEMPTS_PER_NETWORK low when several SSIDs are listed,
// otherwise the fallback AP wait can take a long time.
#define WIFI_CONNECT_TIMEOUT_MS   5000   // ms before considering a connection attempt failed
#define WIFI_ATTEMPTS_PER_NETWORK 3

// ── Fallback WiFi setup AP ────────────────────────────────────────────────────
// If all credentials fail, the device opens this access point.
// Connect to it and open http://192.168.4.1/ to enter your WiFi credentials.
#define WIFI_SETUP_AP_ENABLED 1
#define WIFI_SETUP_AP_SSID    "Tramagotchi"
#define WIFI_SETUP_AP_PASS    ""               // empty = open AP (min 8 chars for WPA2)
#define WIFI_SETUP_AP_IP      192, 168, 4, 1

// ── Optional static IP ────────────────────────────────────────────────────────
// Prefer a DHCP reservation in your router. Only set this if the address is
// guaranteed free in your LAN.
#define WIFI_USE_STATIC_IP   0
#define WIFI_STATIC_LOCAL_IP 192, 168, 0, 80
#define WIFI_STATIC_GATEWAY  192, 168, 0, 1   // router IP
#define WIFI_STATIC_SUBNET   255, 255, 255, 0
#define WIFI_STATIC_DNS1     192, 168, 0, 1   // often the router
#define WIFI_STATIC_DNS2     1, 1, 1, 1        // fallback

// ── Station configuration ─────────────────────────────────────────────────────
// STATION_CONFIG_MODE
//   0 = compiled STATION_LIST from stations.h (simple, no web UI)
//   1 = web-editable list stored in ESP32 flash
//       After connecting, open http://<device-ip>/ on a phone or PC.
#define STATION_CONFIG_MODE       1
#define CONFIG_WEB_SERVER_ENABLED 1
#define STATION_CONFIG_MAX_COUNT  5

// ── Stop data updates ─────────────────────────────────────────────────────────
// Folder URL that holds version.txt and littlefs.bin (made by
// tools/update_wl_data.ps1 + `pio run -t buildfs`), e.g.
// "https://<user>.github.io/<repo>/". Used by the config page's "Update stop
// data" button and the automatic check. Only the stop lists are replaced; saved
// stations and WiFi stay. Empty = no update source yet.
#define DATA_UPDATE_URL          ""
#define DATA_UPDATE_AUTO         1    // 1 = check by itself: 5 min after boot, then every interval
#define DATA_UPDATE_CHECK_HOURS  24

// ── Serial diagnostics ────────────────────────────────────────────────────────
// 1 = log WiFi state and every fetch cycle (request, HTTP code, per-station
// result) to USB serial at 115200 baud. Costs nothing when off.
#define SERIAL_DEBUG_ENABLED 1

// ── Station cycling ───────────────────────────────────────────────────────────
// Auto-cycle every STATION_CYCLE_INTERVAL_MS; button press also cycles.
// Hold button ≥ KAWAII_TOGGLE_LONG_MS to toggle kawaii mode.
#define STATION_CYCLE_INTERVAL_MS  6000
#define NO_DATA_THRESHOLD_MS       1000    // hold + fast-retry this long before showing no-data
