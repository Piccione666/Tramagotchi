#pragma once

#include "board_config.h"

// ── Layout scaling ────────────────────────────────────────────────────────────
//
// Every pixel measurement and text size in this file was tuned on the C3's
// 240x240 panel. UI_PX and UI_TEXT rescale them to whatever panel is fitted, so
// the same layout keeps its proportions on the S3's 466x466 AMOLED. On a
// 240x240 board the scale is 1:1 and nothing changes.
//
// Rounding matters here: truncating would leave text size 1 at 1 instead of 2,
// so the half-denominator below rounds to nearest.
// ─────────────────────────────────────────────────────────────────────────────
#define UI_BASE_DIM 240
#define UI_SCALE(v) (((int32_t)(v) * LCD_WIDTH + UI_BASE_DIM / 2) / UI_BASE_DIM)
#define UI_PX(v)    ((int16_t)UI_SCALE(v))
#define UI_TEXT(v)  ((uint8_t)(UI_SCALE(v) < 1 ? 1 : UI_SCALE(v)))


// ── Content feature switches (each independent) ───────────────────────────────
//
//  the no-data screen only appears when ALL configured stations fail.
// ─────────────────────────────────────────────────────────────────────────────
#define PURGE_EMOJI              0  // 1 = remove all emoji content and functions
#define SPLASH_ENABLED           1
#define LOADING_KAWAII_ENABLED   (PURGE_EMOJI ? 0 : 0)
#define POPUP_KAWAII_ENABLED     (PURGE_EMOJI ? 0 : 1)
#define NO_DATA_MODE             (PURGE_EMOJI ? 0 : 2)  // 0=static text  1=DVD ghost  2=sad emoji
#define GRACE_DISPLAY_MODE       0  // grace window: 0=hold previous screen  1=loading animation

// Boot splash
#define BOOT_SPLASH_DURATION_MS 1500
#define BOOT_SPLASH_TEXT "OFFICINA CANFORA"
#define BOOT_SPLASH_TEXT_SIZE UI_TEXT(2)
#define BOOT_SPLASH_LINE_EXTRA_PX UI_PX(4)

// Status messages
#define WIFI_CONNECTED_TEXT "connected"
#define WIFI_FAILED_TEXT "wifi failed"

// Loading / zero-countdown animation
#define LOADING_ANIMATION_FRAME_MS 500
#define LOADING_PIXEL_SIZE UI_PX(26)
#define LOADING_PIXEL_SPACING UI_PX(13)
#define LOADING_CENTER_Y_OFFSET UI_PX(2)

// No-data screen
// The reason texts replace NO_DATA_INFO_TEXT when the cause is known. They are
// only drawn in the text modes (NO_DATA_MODE 0/1) — mode 2 shows the emoji and
// deliberately says nothing.
#define NO_DATA_INFO_TEXT "-no data-"
#define NO_DATA_NO_MATCH_TEXT "-no match-"    // API has departures, none on this line
#define NO_DATA_NO_SERVICE_TEXT "-no service-" // line not running right now
#define NO_DATA_NETWORK_TEXT "-offline-"       // request/WiFi failed
#define NO_DATA_BRAND_TEXT "Officina Canfora"
#define NO_DATA_ROUTE_TEXT_SIZE UI_TEXT(1)
#define NO_DATA_INFO_TEXT_SIZE UI_TEXT(2)
#define NO_DATA_BRAND_TEXT_SIZE UI_TEXT(1)
#define NO_DATA_LINE_GAP_PX UI_PX(6)
#define NO_DATA_MOVE_INTERVAL_MS 60
#define NO_DATA_START_X UI_PX(18)
#define NO_DATA_START_Y UI_PX(72)
#define NO_DATA_SPEED_X UI_PX(2)
#define NO_DATA_SPEED_Y UI_PX(2)
#define NO_DATA_CENTER_TEXT 1
#define NO_DATA_USE_CIRCULAR_BOUNDS 1
#define NO_DATA_CIRCULAR_MARGIN_PX UI_PX(0)

// No-data ghost trail
#define NO_DATA_GHOST_COUNT 5       // ring slots; sized for lifetime / add-interval
#define NO_DATA_GHOST_MIN_BRIGHTNESS 30  // brightness when fully faded (0-255)
#define NO_DATA_GHOST_SPACING UI_PX(5)      // min pixel distance before recording a new ghost
#define NO_DATA_GHOST_LIFETIME_MS 1000   // ms for a ghost to fade from white to min brightness
#define NO_DATA_GHOST_REDRAW_MS 100  // ms between ghost brightness refreshes


// ── Loading screen timing ─────────────────────────────────────────────────────
#define LOADING_MIN_MS           2000    // minimum ms the loading screen stays visible

// ── Kawaii appearance ─────────────────────────────────────────────────────────
// Largest integer size at which a 5-character face (kKawaiiMaxFaceLen, 6 px per
// character at size 1) still fits the width: 8 on the 240 px C3, 15 on the S3.
#define KAWAII_TEXT_SIZE         ((uint8_t)(LCD_WIDTH / (6 * 5)))
#if !PURGE_EMOJI
#define NO_DATA_SAD_EMOJI        ">_<"  // emoji shown when NO_DATA_MODE == 2
#endif

// ── Kawaii timing ─────────────────────────────────────────────────────────────
#define KAWAII_FRAME_A_MS        3000    // ms the main expression is shown
#define KAWAII_FRAME_B_MS        600     // ms the alternate expression is shown
#define KAWAII_POPUP_INTERVAL_MS (20UL * 60UL * 1000UL)
#define KAWAII_POPUP_DURATION_MS 12000
#define KAWAII_TOGGLE_LONG_MS    1200    // hold ≥ this many ms to toggle permanent mode

// Departure screen
#define DEPARTURE_COUNTDOWN_TEXT_SIZE UI_TEXT(8)
#define DEPARTURE_LINE_TEXT_SIZE UI_TEXT(4)
#define DEPARTURE_TOWARDS_TEXT_SIZE UI_TEXT(2)
#define DEPARTURE_SECOND_COUNTDOWN_TEXT_SIZE UI_TEXT(3)
#define DEPARTURE_COUNTDOWN_Y_OFFSET UI_PX(2)
#define DEPARTURE_LINE_Y_OFFSET UI_PX(32)
#define DEPARTURE_TOWARDS_Y_OFFSET UI_PX(60)
#define DEPARTURE_SECOND_COUNTDOWN_Y_OFFSET UI_PX(75)

// Generic status/debug text
#define STATUS_TEXT_SIZE UI_TEXT(2)
