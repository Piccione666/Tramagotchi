#pragma once

#include <Arduino.h>

struct Departure {
  String line;
  String towards;
  int countdown;
};

void display_init();
void display_play_boot_splash(uint32_t durationMs);
void display_show_loading();
void display_status(const char* message);
void display_departures(const Departure& first, const Departure& second, bool haveSecond);
// Why no departures are shown. Text modes print a matching label; the emoji
// mode (NO_DATA_MODE == 2) ignores it and just shows the sad face.
enum NoDataReason : uint8_t {
  NoDataUnknown = 0,
  NoDataNoMatch,     // stop has departures, but none on the configured line
  NoDataNoService,   // line exists here but has no departures right now
  NoDataNetwork,     // WiFi/HTTP/JSON failure
};

void display_no_data(const char* line, const char* towards, NoDataReason reason = NoDataUnknown);
void display_update_no_data();
void display_update_animation();
void display_debug(const char* message);

// Kawaii face animation
void display_show_kawaii();
void display_update_kawaii_animation();
void display_stop_kawaii();
bool display_is_kawaii_active();
