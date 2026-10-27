#pragma once

#include "board_config.h"

// ── Input sources ─────────────────────────────────────────────────────────────
//
// Short press: cycle to the next station. Hold ≥ KAWAII_TOGGLE_LONG_MS: toggle
// kawaii mode. src/input.cpp turns each board's hardware into one "pressed"
// signal, which main.cpp debounces and times.
// ─────────────────────────────────────────────────────────────────────────────

#if defined(BOARD_TRAMAGOTCHI_S3)

// The S3 has no free GPIO for a button. It has a touchscreen, the PWR button
// (on the AXP2101) and BOOT. Which of touch and PWR acts as the button is a
// setting on the config page ("Button", NVS settings/input); BOOT works in both.
#define INPUT_USE_TOUCH 1

// The onboard BOOT button. Holding it while the board powers up enters firmware
// download mode instead of running the firmware, so it is only the fallback.
#define INPUT_USE_BOOT_BUTTON 1
#define BOOT_BUTTON_PIN 0  // active low, with an internal pull-up

// Ignore touches that report more contact than a fingertip, which are usually
// the device sitting face down rather than a deliberate press.
#define TOUCH_MAX_POINTS 2

// How often the touch controller and the PMU are polled, in milliseconds. The
// debounce in main.cpp runs on top of this, so it does not need to be fast.
#define TOUCH_POLL_INTERVAL_MS 20

// A PWR press is held at least this long, so a tap that starts and ends between
// two polls still outlasts main.cpp's 50 ms debounce.
#define PMU_MIN_PRESS_MS 80

#elif defined(BOARD_TRAMAGOTCHI_C3)

// ── GPIO pin assignments ──────────────────────────────────────────────────────
//
//  Pin  Label       Wiring
//  ─────────────────────────────────────────────────────────────────────────────
//  5    TOUCH_PIN   Button/touch pad to 3V3 — pressed reads HIGH (active high).
// ─────────────────────────────────────────────────────────────────────────────
// TOUCH_ACTIVE_LOW also picks the internal pull resistor, so the idle level is
// always defined: 1 → button to GND + INPUT_PULLUP, 0 → button to 3V3 +
// INPUT_PULLDOWN. Setting it wrong leaves the pin at one level in both states
// and the button never registers.
#define INPUT_USE_TOUCH 0
#define INPUT_USE_BOOT_BUTTON 0
#define TOUCH_PIN        5
#define TOUCH_ACTIVE_LOW 0   // 0 = pressed when HIGH (button to 3V3 + INPUT_PULLDOWN)

#endif

#define NO_BUTTON_ATTACHED 0 // 1 = no input wired at all; all press handling is skipped
