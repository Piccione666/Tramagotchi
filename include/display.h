#pragma once

#include <Arduino.h>

struct Departure {
  String line;
  String towards;
  int countdown;
};

void display_init();
void display_show_loading();
void display_status(const char* message);
void display_departures(const Departure& first, const Departure& second, bool haveSecond);
void display_no_data(const char* line, const char* towards);
void display_update_animation();
void display_debug(const char* message);
