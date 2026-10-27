#pragma once

#include <Arduino.h>

// One press signal, whatever the board's hardware happens to be: a GPIO button
// on the C3; the touchscreen or the PWR button (plus BOOT) on the S3. main.cpp
// debounces and times whatever this reports, so it does not care which.

// Which S3 source acts as the button (NVS settings/input). The C3 ignores it.
enum InputMode : uint8_t {
  InputTap = 0,     // tap or hold anywhere on the screen
  InputButton = 1,  // the PWR button
};

void input_init();
void input_set_mode(InputMode mode);
InputMode input_mode();

// True while a press is in progress. BOOT counts in either mode on the S3.
bool input_pressed();

// Whether the touch controller started up. False on boards without one, and on
// the S3 if the controller did not answer.
bool input_touch_ready();
// Whether the PMU (and with it the PWR button) answered. False on the C3.
bool input_pmu_ready();
