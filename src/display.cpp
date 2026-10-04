#include <TFT_eSPI.h>
#include <cmath>
#include "display.h"
#include "config.h"
#include "graphics_config.h"
#if !PURGE_EMOJI
#include "kawaii_frames.h"
#endif

namespace {
  TFT_eSPI tft = TFT_eSPI();
  uint8_t gAnimationCycle = 255;
  bool gNoDataActive = false;
  int16_t gNoDataX = NO_DATA_START_X;
  int16_t gNoDataY = NO_DATA_START_Y;
  int8_t gNoDataDx = NO_DATA_SPEED_X;
  int8_t gNoDataDy = NO_DATA_SPEED_Y;
  int16_t gNoDataPrevX = -1;
  int16_t gNoDataPrevY = -1;
  uint32_t gNoDataLastMoveMs = 0;
  String gNoDataRouteText;

  struct GhostPos { int16_t x, y; uint32_t addedMs; };
  GhostPos gGhostRing[NO_DATA_GHOST_COUNT] = {};
  uint8_t  gGhostFill = 0;
  uint8_t  gGhostHead = 0;
  uint32_t gLastGhostDrawMs = 0;
  int16_t  gLastGhostX = -1;
  int16_t  gLastGhostY = -1;

  bool    gKawaiiActive      = false;
  uint8_t gKawaiiFrameA      = 0;
  uint8_t gKawaiiFrameB      = 1;
  bool    gKawaiiShowingA    = true;
  uint32_t gKawaiiLastSwapMs = 0;

  // What the departure screen currently shows. Any other screen goes through
  // clearScreen(), which drops this, so a later identical result still redraws.
  bool      gDeparturesShown   = false;
  Departure gShownFirst        = {"", "", -1};
  Departure gShownSecond       = {"", "", -1};
  bool      gShownHaveSecond   = false;

  void clearScreen() {
    tft.fillScreen(TFT_BLACK);
    gDeparturesShown = false;
  }

  bool departuresOnScreen(const Departure& first, const Departure& second, bool haveSecond) {
    if (!gDeparturesShown || haveSecond != gShownHaveSecond) return false;
    if (first.countdown != gShownFirst.countdown || first.line != gShownFirst.line ||
        first.towards != gShownFirst.towards) return false;
    // Only the second countdown is drawn, so that's all that has to match.
    return !haveSecond || second.countdown == gShownSecond.countdown;
  }

#if !PURGE_EMOJI
  void drawKawaiiFrameAt(uint8_t idx) {
    const char* emoji = kKawaiiFrames[idx % kKawaiiFrameCount];
    clearScreen();
    tft.setTextSize(KAWAII_TEXT_SIZE);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.drawString(emoji, tft.width() / 2, tft.height() / 2);
  }
#endif

  constexpr uint32_t kNoDataMoveIntervalMs = NO_DATA_MOVE_INTERVAL_MS;
  constexpr float kNoDataRouteTextSize = NO_DATA_ROUTE_TEXT_SIZE;
  constexpr float kNoDataInfoTextSize = NO_DATA_INFO_TEXT_SIZE;
  constexpr float kNoDataBrandTextSize = NO_DATA_BRAND_TEXT_SIZE;
  constexpr int16_t kNoDataLineGap = NO_DATA_LINE_GAP_PX;
  const char* kNoDataInfoText = NO_DATA_INFO_TEXT;

  // Set from the reason passed to display_no_data(); drives the middle line.
  const char* gNoDataInfoText = NO_DATA_INFO_TEXT;

  const char* noDataInfoTextFor(NoDataReason reason) {
    switch (reason) {
      case NoDataNoMatch:   return NO_DATA_NO_MATCH_TEXT;
      case NoDataNoService: return NO_DATA_NO_SERVICE_TEXT;
      case NoDataNetwork:   return NO_DATA_NETWORK_TEXT;
      default:              return kNoDataInfoText;
    }
  }
  const char* kNoDataBrandText = NO_DATA_BRAND_TEXT;
  const char* kBrandText = BOOT_SPLASH_TEXT;

  void setConfiguredTextSize(float size) {
    uint8_t roundedSize = static_cast<uint8_t>(roundf(size));
    tft.setTextSize(max<uint8_t>(1, roundedSize));
  }

  String displayText(const char* text) {
    String result;
    if (!text) return result;

    for (const char* p = text; *p;) {
      uint8_t c = static_cast<uint8_t>(*p);

      if (c == 0xC3 && p[1]) {
        uint8_t next = static_cast<uint8_t>(p[1]);
        if (next == 0xA4) { result += "ae"; p += 2; continue; } // ae
        if (next == 0xB6) { result += "oe"; p += 2; continue; } // oe
        if (next == 0xBC) { result += "ue"; p += 2; continue; } // ue
        if (next == 0x84) { result += "Ae"; p += 2; continue; } // Ae
        if (next == 0x96) { result += "Oe"; p += 2; continue; } // Oe
        if (next == 0x9C) { result += "Ue"; p += 2; continue; } // Ue
        if (next == 0x9F) { result += "ss"; p += 2; continue; } // ss
      }

      result += *p;
      p++;
    }
    return result;
  }

  String displayText(const String& text) {
    return displayText(text.c_str());
  }

  int16_t displayTextWidth(const char* text) {
    String converted = displayText(text);
    return tft.textWidth(converted);
  }

  int16_t displayTextWidth(const String& text) {
    String converted = displayText(text);
    return tft.textWidth(converted);
  }

  void display_blink_animation(int16_t cx, int16_t cy) {
    // 4 pixels in rectangular array, only top-right and bottom-left visible
    // Sized to match the large text (7.7)
    const int16_t pixel_size = LOADING_PIXEL_SIZE;
    const int16_t spacing = LOADING_PIXEL_SPACING;  // gap between pixels
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

    uint32_t cycle = millis() % (LOADING_ANIMATION_FRAME_MS * 2);
    bool top_right_on = cycle < LOADING_ANIMATION_FRAME_MS;
    bool bottom_left_on = cycle >= LOADING_ANIMATION_FRAME_MS;

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

  int16_t noDataBlockWidth() {
    int16_t routeWidth = 0;
    if (gNoDataRouteText.length() > 0) {
      setConfiguredTextSize(kNoDataRouteTextSize);
      routeWidth = displayTextWidth(gNoDataRouteText);
    }

    setConfiguredTextSize(kNoDataInfoTextSize);
    int16_t infoWidth = displayTextWidth(gNoDataInfoText);
    setConfiguredTextSize(kNoDataBrandTextSize);
    int16_t brandWidth = displayTextWidth(kNoDataBrandText);
    return max(routeWidth, max(infoWidth, brandWidth));
  }

  int16_t noDataRouteLineHeight() {
    if (gNoDataRouteText.length() == 0) return 0;
    setConfiguredTextSize(kNoDataRouteTextSize);
    return tft.fontHeight();
  }

  int16_t noDataInfoLineHeight() {
    setConfiguredTextSize(kNoDataInfoTextSize);
    return tft.fontHeight();
  }

  int16_t noDataBrandLineHeight() {
    setConfiguredTextSize(kNoDataBrandTextSize);
    return tft.fontHeight();
  }

  int16_t noDataBlockHeight() {
    int16_t height = noDataInfoLineHeight() + kNoDataLineGap + noDataBrandLineHeight();
    if (gNoDataRouteText.length() > 0) {
      height += noDataRouteLineHeight() + kNoDataLineGap;
    }
    return height;
  }

  bool pointInsideNoDataCircle(float x, float y) {
    float cx = (tft.width() - 1) / 2.0f;
    float cy = (tft.height() - 1) / 2.0f;
    float radius = (min(tft.width(), tft.height()) / 2.0f) - NO_DATA_CIRCULAR_MARGIN_PX - 1.0f;
    float dx = x - cx;
    float dy = y - cy;
    return (dx * dx + dy * dy) <= (radius * radius);
  }

  bool noDataRectInsideVisibleArea(int16_t x, int16_t y, int16_t blockWidth, int16_t blockHeight) {
#if NO_DATA_USE_CIRCULAR_BOUNDS
    float left = x;
    float top = y;
    float right = x + blockWidth - 1;
    float bottom = y + blockHeight - 1;
    return pointInsideNoDataCircle(left, top) &&
           pointInsideNoDataCircle(right, top) &&
           pointInsideNoDataCircle(left, bottom) &&
           pointInsideNoDataCircle(right, bottom);
#else
    return x >= 0 &&
           y >= 0 &&
           x + blockWidth <= tft.width() &&
           y + blockHeight <= tft.height();
#endif
  }

  void resetNoDataPosition() {
    gNoDataX = NO_DATA_START_X;
    gNoDataY = NO_DATA_START_Y;
    gNoDataDx = NO_DATA_SPEED_X;
    gNoDataDy = NO_DATA_SPEED_Y;
  }

  int8_t randomSignedSpeed(int8_t speed) {
    if (speed == 0) return 0;
    int8_t magnitude = abs(speed);
    return random(2) == 0 ? -magnitude : magnitude;
  }

  void randomizeNoDataPosition(int16_t blockWidth, int16_t blockHeight) {
    int16_t maxX = max<int16_t>(0, tft.width() - blockWidth);
    int16_t maxY = max<int16_t>(0, tft.height() - blockHeight);

    for (uint8_t attempt = 0; attempt < 40; attempt++) {
      int16_t x = random(maxX + 1);
      int16_t y = random(maxY + 1);
      if (noDataRectInsideVisibleArea(x, y, blockWidth, blockHeight)) {
        gNoDataX = x;
        gNoDataY = y;
        gNoDataDx = randomSignedSpeed(NO_DATA_SPEED_X);
        gNoDataDy = randomSignedSpeed(NO_DATA_SPEED_Y);
        return;
      }
    }

    resetNoDataPosition();
    if (!noDataRectInsideVisibleArea(gNoDataX, gNoDataY, blockWidth, blockHeight)) {
      gNoDataX = max<int16_t>(0, (tft.width() - blockWidth) / 2);
      gNoDataY = max<int16_t>(0, (tft.height() - blockHeight) / 2);
    }
  }

  void advanceNoDataPosition(int16_t blockWidth, int16_t blockHeight) {
    int16_t nextX = gNoDataX + gNoDataDx;
    int16_t nextY = gNoDataY + gNoDataDy;

    if (!noDataRectInsideVisibleArea(nextX, gNoDataY, blockWidth, blockHeight)) {
      gNoDataDx = -gNoDataDx;
      nextX = gNoDataX + gNoDataDx;
    }
    if (!noDataRectInsideVisibleArea(gNoDataX, nextY, blockWidth, blockHeight)) {
      gNoDataDy = -gNoDataDy;
      nextY = gNoDataY + gNoDataDy;
    }
    if (!noDataRectInsideVisibleArea(nextX, nextY, blockWidth, blockHeight)) {
      gNoDataDx = -gNoDataDx;
      gNoDataDy = -gNoDataDy;
      nextX = gNoDataX + gNoDataDx;
      nextY = gNoDataY + gNoDataDy;
    }

    if (noDataRectInsideVisibleArea(nextX, nextY, blockWidth, blockHeight)) {
      gNoDataX = nextX;
      gNoDataY = nextY;
    }
  }

  void drawNoDataBlock(int16_t x, int16_t y, uint16_t color) {
    tft.setTextColor(color, TFT_BLACK);
    int16_t blockWidth = noDataBlockWidth();
    int16_t cursorY = y;
#if NO_DATA_CENTER_TEXT
    tft.setTextDatum(TC_DATUM);
    int16_t centerX = x + blockWidth / 2;
    if (gNoDataRouteText.length() > 0) {
      setConfiguredTextSize(kNoDataRouteTextSize);
      tft.drawString(displayText(gNoDataRouteText), centerX, cursorY);
      cursorY += noDataRouteLineHeight() + kNoDataLineGap;
    }
    setConfiguredTextSize(kNoDataInfoTextSize);
    tft.drawString(displayText(gNoDataInfoText), centerX, cursorY);
    cursorY += noDataInfoLineHeight() + kNoDataLineGap;
    setConfiguredTextSize(kNoDataBrandTextSize);
    tft.drawString(displayText(kNoDataBrandText), centerX, cursorY);
#else
    tft.setTextDatum(TL_DATUM);
    if (gNoDataRouteText.length() > 0) {
      setConfiguredTextSize(kNoDataRouteTextSize);
      tft.drawString(displayText(gNoDataRouteText), x, cursorY);
      cursorY += noDataRouteLineHeight() + kNoDataLineGap;
    }
    setConfiguredTextSize(kNoDataInfoTextSize);
    tft.drawString(displayText(gNoDataInfoText), x, cursorY);
    cursorY += noDataInfoLineHeight() + kNoDataLineGap;
    setConfiguredTextSize(kNoDataBrandTextSize);
    tft.drawString(displayText(kNoDataBrandText), x, cursorY);
#endif
  }

  String noDataRouteText(const char* line, const char* towards) {
    String route;
    if (line && line[0]) route += line;
    if (towards && towards[0]) {
      if (route.length() > 0) route += " ";
      route += towards;
    }
    return route;
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
    setConfiguredTextSize(size);
    int16_t width = displayTextWidth(text);
    if (width <= max_width) {
      return size;
    }
    size -= 0.1;
  }
  return min_size;
}

void display_update_animation() {
#if LOADING_KAWAII_ENABLED
  display_update_kawaii_animation();
  return;
#endif
  // Only redraw when state actually changes; no clear+redraw on every loop call.
  uint32_t now = millis();
  uint8_t cycle = (now / LOADING_ANIMATION_FRAME_MS) % 2;  // 0 = top-right, 1 = bottom-left

  if (cycle == gAnimationCycle) return;
  gAnimationCycle = cycle;

  const int16_t cx = tft.width() / 2;
  const int16_t cy = tft.height() / 2 + LOADING_CENTER_Y_OFFSET;

  const int16_t pixel_size = LOADING_PIXEL_SIZE;
  const int16_t spacing = LOADING_PIXEL_SPACING;
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
  clearScreen();
  randomSeed(micros());
}

void display_play_boot_splash(uint32_t durationMs) {
  clearScreen();

  tft.setTextDatum(MC_DATUM);
  setConfiguredTextSize(BOOT_SPLASH_TEXT_SIZE);

  int16_t lineHeight = tft.fontHeight() + BOOT_SPLASH_LINE_EXTRA_PX;
  int16_t firstY = lineHeight / 2;
  int16_t lastY = tft.height() - lineHeight / 2;
  uint16_t lineCount = max<uint16_t>(1, ((lastY - firstY) / lineHeight) + 1);
  uint32_t stepMs = durationMs / lineCount;
  if (stepMs == 0) stepMs = 1;

  for (uint16_t line = 0; line < lineCount; line++) {
    float blend = (lineCount <= 1) ? 1.0f : static_cast<float>(line) / static_cast<float>(lineCount - 1);
    uint8_t gray = static_cast<uint8_t>(roundf(102.0f + blend * (255.0f - 102.0f)));
    tft.setTextColor(tft.color565(gray, gray, gray), TFT_BLACK);
    tft.drawString(displayText(kBrandText), tft.width() / 2, firstY + line * lineHeight);
    delay(stepMs);
  }

  uint32_t elapsed = stepMs * lineCount;
  if (durationMs > elapsed) delay(durationMs - elapsed);
}

void display_show_loading() {
#if LOADING_KAWAII_ENABLED
  display_show_kawaii();
  return;
#endif
  clearScreen();
  gAnimationCycle = 255;  // Force immediate draw on the next animation update.
  display_update_animation();
}

void display_status(const char* message) {
  clearScreen();
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  setConfiguredTextSize(STATUS_TEXT_SIZE);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(displayText(message), tft.width() / 2, tft.height() / 2);
}

void display_debug(const char* message) {
  clearScreen();
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  setConfiguredTextSize(STATUS_TEXT_SIZE);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(displayText(message), tft.width() / 2, tft.height() / 2);
}

void display_departures(const Departure& first, const Departure& second, bool haveSecond) {
  // Every fetch reports a result; blanking and redrawing an unchanged screen
  // shows up as a visible flicker on the panel.
  if (departuresOnScreen(first, second, haveSecond)) return;
  gNoDataActive = false;
  clearScreen();
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);

  const int16_t cx = tft.width() / 2;
  const int16_t cy = tft.height() / 2;
  const int16_t radius = min(cx, cy) - 4;

  // Circular frame
  tft.drawCircle(cx, cy, radius, TFT_BLACK);

  // First departure - large countdown in center or animation if at 0
  if (first.countdown == 0) {
    display_blink_animation(cx, cy + DEPARTURE_COUNTDOWN_Y_OFFSET);
  } else {
    String firstCountdown = String(first.countdown);
    setConfiguredTextSize(DEPARTURE_COUNTDOWN_TEXT_SIZE);
    tft.drawString(firstCountdown, cx, cy + DEPARTURE_COUNTDOWN_Y_OFFSET);
  }

  // Line (top) and direction (bottom) around the circle
  setConfiguredTextSize(DEPARTURE_LINE_TEXT_SIZE);
  tft.drawString(displayText(first.line), cx, cy - radius + DEPARTURE_LINE_Y_OFFSET);

  // Direction with dynamic sizing based on circular display
  // Text is at distance (radius - 52) from center
  String displayTowards = displayText(first.towards);
  float towardSize = fit_text_size(displayTowards.c_str(), DEPARTURE_TOWARDS_TEXT_SIZE, radius, radius - DEPARTURE_TOWARDS_Y_OFFSET);
  setConfiguredTextSize(towardSize);
  tft.drawString(displayTowards, cx, cy - radius + DEPARTURE_TOWARDS_Y_OFFSET);

  // Second departure (smaller, below the first)
  if (haveSecond) {
    setConfiguredTextSize(DEPARTURE_SECOND_COUNTDOWN_TEXT_SIZE);
    tft.drawString(String(second.countdown), cx, cy + DEPARTURE_SECOND_COUNTDOWN_Y_OFFSET);
  // tft.setTextSize(2);
  // tft.drawString(second.towards, cx, cy + 70);
  }

  gDeparturesShown = true;
  gShownFirst      = first;
  gShownSecond     = second;
  gShownHaveSecond = haveSecond;
}

void display_no_data(const char* line, const char* towards, NoDataReason reason) {
  clearScreen();
  gNoDataActive    = true;
  gNoDataRouteText = noDataRouteText(line, towards);
  gNoDataInfoText  = noDataInfoTextFor(reason);
#if NO_DATA_MODE == 2
  tft.setTextSize(KAWAII_TEXT_SIZE);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(NO_DATA_SAD_EMOJI, tft.width() / 2, tft.height() / 2);
#else
  int16_t bw = noDataBlockWidth();
  int16_t bh = noDataBlockHeight();
  #if NO_DATA_MODE == 1
    randomizeNoDataPosition(bw, bh);
    gNoDataPrevX      = -1;
    gNoDataPrevY      = -1;
    gNoDataLastMoveMs = millis() - kNoDataMoveIntervalMs;
    gGhostFill        = 0;
    gGhostHead        = 0;
    gLastGhostDrawMs  = 0;
    gLastGhostX       = -1;
    gLastGhostY       = -1;
    display_update_no_data();
  #else
    gNoDataX = max<int16_t>(0, (tft.width()  - bw) / 2);
    gNoDataY = max<int16_t>(0, (tft.height() - bh) / 2);
    drawNoDataBlock(gNoDataX, gNoDataY, TFT_WHITE);
  #endif
#endif
}

#if !PURGE_EMOJI
void display_show_kawaii() {
  gKawaiiFrameA = random(kKawaiiFrameCount);
  do { gKawaiiFrameB = random(kKawaiiFrameCount); } while (gKawaiiFrameB == gKawaiiFrameA);
  gKawaiiShowingA   = true;
  gKawaiiActive     = true;
  gNoDataActive     = false;
  gKawaiiLastSwapMs = millis();
  drawKawaiiFrameAt(gKawaiiFrameA);
}

void display_stop_kawaii() {
  gKawaiiActive = false;
  clearScreen();
}

bool display_is_kawaii_active() {
  return gKawaiiActive;
}

void display_update_kawaii_animation() {
  if (!gKawaiiActive) return;
  uint32_t now = millis();
  uint32_t frameMs = gKawaiiShowingA ? KAWAII_FRAME_A_MS : KAWAII_FRAME_B_MS;
  if (now - gKawaiiLastSwapMs < frameMs) return;
  gKawaiiLastSwapMs = now;
  gKawaiiShowingA   = !gKawaiiShowingA;
  drawKawaiiFrameAt(gKawaiiShowingA ? gKawaiiFrameA : gKawaiiFrameB);
}
#else
void display_show_kawaii() {}
void display_stop_kawaii() {}
bool display_is_kawaii_active() { return false; }
void display_update_kawaii_animation() {}
#endif

void display_update_no_data() {
  if (!gNoDataActive) return;
#if NO_DATA_MODE != 1
  return;
#endif

  uint32_t now = millis();
  int16_t blockWidth  = noDataBlockWidth();
  int16_t blockHeight = noDataBlockHeight();

  bool timeToMove   = !gNoDataLastMoveMs || (now - gNoDataLastMoveMs >= kNoDataMoveIntervalMs);
  bool timeToRedraw = timeToMove || (now - gLastGhostDrawMs >= NO_DATA_GHOST_REDRAW_MS);
  if (!timeToRedraw) return;
  gLastGhostDrawMs = now;

  if (timeToMove) {
    gNoDataLastMoveMs = now;

    bool addedToRing = false;
    if (gNoDataPrevX >= 0) {
      bool addGhost = (gLastGhostX < 0);
      if (!addGhost) {
        int16_t dx = gNoDataPrevX - gLastGhostX;
        int16_t dy = gNoDataPrevY - gLastGhostY;
        addGhost = (dx * dx + dy * dy) >= (NO_DATA_GHOST_SPACING * NO_DATA_GHOST_SPACING);
      }
      if (addGhost) {
        uint8_t newHead = (gGhostHead + 1) % NO_DATA_GHOST_COUNT;
        if (gGhostFill >= NO_DATA_GHOST_COUNT) {
          // Ring full: erase the slot we're about to overwrite (safety fallback)
          tft.fillRect(gGhostRing[newHead].x - 2, gGhostRing[newHead].y - 2,
                       blockWidth + 4, blockHeight + 4, TFT_BLACK);
        } else {
          gGhostFill++;
        }
        gGhostHead = newHead;
        gGhostRing[gGhostHead] = {gNoDataPrevX, gNoDataPrevY, now};
        gLastGhostX = gNoDataPrevX;
        gLastGhostY = gNoDataPrevY;
        addedToRing = true;
      }
    }

    if (!addedToRing && gNoDataPrevX >= 0) {
      tft.fillRect(gNoDataPrevX - 2, gNoDataPrevY - 2, blockWidth + 4, blockHeight + 4, TFT_BLACK);
    }

    advanceNoDataPosition(blockWidth, blockHeight);
    gNoDataPrevX = gNoDataX;
    gNoDataPrevY = gNoDataY;
  }

  // Expire ghosts whose lifetime has elapsed
  while (gGhostFill > 0) {
    uint8_t oldestIdx = (gGhostHead + NO_DATA_GHOST_COUNT - (gGhostFill - 1)) % NO_DATA_GHOST_COUNT;
    if (now - gGhostRing[oldestIdx].addedMs >= NO_DATA_GHOST_LIFETIME_MS) {
      tft.fillRect(gGhostRing[oldestIdx].x - 2, gGhostRing[oldestIdx].y - 2,
                   blockWidth + 4, blockHeight + 4, TFT_BLACK);
      gGhostFill--;
    } else {
      break;
    }
  }

  // Redraw ghosts with smooth time-based brightness fade
  for (uint8_t i = 0; i < gGhostFill; i++) {
    uint8_t idx = (gGhostHead + NO_DATA_GHOST_COUNT - (gGhostFill - 1 - i)) % NO_DATA_GHOST_COUNT;
    uint32_t age = now - gGhostRing[idx].addedMs;
    float t = min(1.0f, (float)age / NO_DATA_GHOST_LIFETIME_MS);
    uint8_t brightness = 255 - (uint8_t)(t * (255 - NO_DATA_GHOST_MIN_BRIGHTNESS));
    drawNoDataBlock(gGhostRing[idx].x, gGhostRing[idx].y,
                    tft.color565(brightness, brightness, brightness));
  }

  drawNoDataBlock(gNoDataX, gNoDataY, TFT_WHITE);
}
