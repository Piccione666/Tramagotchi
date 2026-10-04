#pragma once

// ── GPIO pin assignments ──────────────────────────────────────────────────────
//
//  Pin  Label       Wiring
//  ─────────────────────────────────────────────────────────────────────────────
//  5    TOUCH_PIN   Button/touch pad to 3V3 — pressed reads HIGH (active high).
//                   Short press: cycle to next station (always active + auto-cycling)
//                   Hold ≥ KAWAII_TOGGLE_LONG_MS: toggle kawaii mode
// ─────────────────────────────────────────────────────────────────────────────
// TOUCH_ACTIVE_LOW also picks the internal pull resistor, so the idle level is
// always defined: 1 → button to GND + INPUT_PULLUP, 0 → button to 3V3 +
// INPUT_PULLDOWN. Setting it wrong leaves the pin at one level in both states
// and the button never registers.
#define TOUCH_PIN        5
#define TOUCH_ACTIVE_LOW 0   // 0 = pressed when HIGH (button to 3V3 + INPUT_PULLDOWN)
#define NO_BUTTON_ATTACHED 0 // 1 = no button wired; all button handling is skipped
