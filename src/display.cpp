#include <TFT_eSPI.h>
#include <cmath>
#include "display.h"
#include "config.h"

namespace {
  TFT_eSPI tft = TFT_eSPI();
  uint8_t gAnimationCycle = 255;

  void display_blink_animation(int16_t cx, int16_t cy) {
    // 4 pixels in rectangular array, only top-right and bottom-left visible
    // Sized to match the large text (7.7)
    const int16_t pixel_size = 24;
    const int16_t spacing = 12;  // gap between pixels
    const int16_t grid_width = pixel_size * 2 + spacing;  // total width of grid
    const int16_t grid_height = pixel_size * 2 + spacing;

    // Center the grid at (cx, cy)
    int16_t grid_left = cx - grid_width / 2;
    int16_t grid_top = cy - grid_height / 2;

    // Positions in grid
    int16_t tr_x = grid_left + pixel_size + spacing;
    int16_t bl_x = grid_left;

    int16_t t_y = grid_top;
    int16_t b_y = grid_top + pixel_size + spacing;

    // Blinking pattern: cycle every 1000ms, offset by 500ms
    uint32_t cycle = millis() % 1000;
    bool top_right_on = cycle < 500;   // Top-right on for first 500ms
    bool bottom_left_on = cycle >= 500; // Bottom-left on for next 500ms

    // Clear both pixel areas first
    tft.fillRect(tr_x, t_y, pixel_size, pixel_size, TFT_BLACK);
    tft.fillRect(bl_x, b_y, pixel_size, pixel_size, TFT_BLACK);

    // Draw top-right if on
    if (top_right_on) {
      tft.fillRect(tr_x, t_y, pixel_size, pixel_size, TFT_WHITE);
    }

    // Draw bottom-left if on
    if (bottom_left_on) {
      tft.fillRect(bl_x, b_y, pixel_size, pixel_size, TFT_WHITE);
    }
  }
}

float fit_text_size(const char* text, float start_size, int16_t radius, int16_t distance_from_center) {
  // For circular display: calculate available width at the given distance from center
  // Using circle equation: x^2 + y^2 = r^2
  // Available width = 2 * sqrt(r^2 - d^2)

  int16_t d = distance_from_center;
  int16_t r = radius;

  // Avoid sqrt of negative number
  if (d >= r) {
    return 0.5;  // Text won't fit
  }

  // Calculate max width at this distance from center
  int16_t max_width = 2 * sqrt(r * r - d * d) - 4;  // -4 for padding

  float size = start_size;
  const float min_size = 0.5;

  while (size > min_size) {
    tft.setTextSize(size);
    int16_t width = tft.textWidth(text);
    if (width <= max_width) {
      return size;
    }
    size -= 0.1;
  }
  return min_size;
}

void display_update_animation() {
  // Only redraw when state actually changes; no clear+redraw on every loop call.
  uint32_t now = millis();
  uint8_t cycle = (now / 500) % 2;  // 0 = top-right, 1 = bottom-left

  if (cycle == gAnimationCycle) return;
  gAnimationCycle = cycle;

  const int16_t cx = tft.width() / 2;
  const int16_t cy = tft.height() / 2 + 2;

  const int16_t pixel_size = 24;
  const int16_t spacing = 12;
  const int16_t grid_width = pixel_size * 2 + spacing;
  const int16_t grid_height = pixel_size * 2 + spacing;

  int16_t grid_left = cx - grid_width / 2;
  int16_t grid_top  = cy - grid_height / 2;

  int16_t tr_x = grid_left + pixel_size + spacing;
  int16_t bl_x = grid_left;
  int16_t t_y  = grid_top;
  int16_t b_y  = grid_top + pixel_size + spacing;

  // Draw the new active pixel first, then clear the old one.
  if (cycle == 0) {
    tft.fillRect(tr_x, t_y, pixel_size, pixel_size, TFT_WHITE);
    tft.fillRect(bl_x, b_y, pixel_size, pixel_size, TFT_BLACK);
  } else {
    tft.fillRect(bl_x, b_y, pixel_size, pixel_size, TFT_WHITE);
    tft.fillRect(tr_x, t_y, pixel_size, pixel_size, TFT_BLACK);
  }
}

void display_init() {
  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);
}

void display_show_loading() {
  tft.fillScreen(TFT_BLACK);
  gAnimationCycle = 255;  // Force immediate draw on the next animation update.
  display_update_animation();
}

void display_status(const char* message) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(2);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(message, tft.width() / 2, tft.height() / 2);
}

void display_debug(const char* message) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.setTextSize(2);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(message, tft.width() / 2, tft.height() / 2);
}

void display_departures(const Departure& first, const Departure& second, bool haveSecond) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);

  const int16_t cx = tft.width() / 2;
  const int16_t cy = tft.height() / 2;
  const int16_t radius = min(cx, cy) - 4;

  // Circular frame
  tft.drawCircle(cx, cy, radius, TFT_DARKGREY);

  // First departure - large countdown in center or animation if at 0
  if (TEST_ANIMATION_MODE || first.countdown == 0) {
    display_blink_animation(cx, cy + 2);
  } else {
    String firstCountdown = String(first.countdown);
    tft.setTextSize(7.7);
    tft.drawString(firstCountdown, cx, cy + 2);
  }

  // Line (top) and direction (bottom) around the circle
  tft.setTextSize(3.85);
  tft.drawString(first.line, cx, cy - radius + 30);

  // Direction with dynamic sizing based on circular display
  // Text is at distance (radius - 52) from center
  float towardSize = fit_text_size(first.towards.c_str(), 2.112, radius, radius - 52);
  tft.setTextSize(towardSize);
  tft.drawString(first.towards, cx, cy - radius + 52);

  // Second departure (smaller, below the first)
  if (haveSecond) {
    tft.setTextSize(3.4375);
    tft.drawString(String(second.countdown), cx, cy + 70);
  // tft.setTextSize(2);
  // tft.drawString(second.towards, cx, cy + 70);
  }
}

void display_no_data(const char* line, const char* towards) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);

  const int16_t cx = tft.width() / 2;
  const int16_t cy = tft.height() / 2;
  const int16_t radius = min(cx, cy) - 4;

  // Circular frame
  tft.drawCircle(cx, cy, radius, TFT_DARKGREY);

  // Show "No data" message in center
  tft.setTextSize(2);
  tft.drawString("No data", cx, cy + 2);

  // Show stop name with dynamic sizing based on circular display
  float nameSize = fit_text_size(towards, 3.85, radius, radius - 30);
  tft.setTextSize(nameSize);
  tft.drawString(towards, cx, cy - radius + 30);
}
