#pragma once

// One drawing surface for both boards.
//
// The C3 drives a 240x240 SPI TFT through TFT_eSPI. The S3's 466x466 AMOLED has
// a CO5300 controller on a QSPI bus, which TFT_eSPI cannot speak, so it goes
// through Arduino_GFX instead. Arduino_GFX inherits Adafruit_GFX's cursor-based
// text model and has no concept of a datum, so the S3 path adds the handful of
// TFT_eSPI text calls that src/display.cpp relies on: drawString with a datum,
// textWidth and fontHeight.
//
// Both paths expose the same API, so display.cpp is written once against it.

#include <Arduino.h>
#include "board_config.h"

#if defined(BOARD_TRAMAGOTCHI_C3)

#include <TFT_eSPI.h>
using Screen = TFT_eSPI;

#else  // BOARD_TRAMAGOTCHI_S3

#include <Arduino_GFX_Library.h>

// TFT_eSPI's names, so display.cpp reads the same on both boards.
#ifndef TFT_BLACK
#define TFT_BLACK  0x0000
#define TFT_WHITE  0xFFFF
#define TFT_YELLOW 0xFFE0
#endif

#ifndef TL_DATUM
#define TL_DATUM 0  // top left
#define TC_DATUM 1  // top centre
#define TR_DATUM 2  // top right
#define ML_DATUM 3  // middle left
#define MC_DATUM 4  // middle centre
#define MR_DATUM 5  // middle right
#define BL_DATUM 6  // bottom left
#define BC_DATUM 7  // bottom centre
#define BR_DATUM 8  // bottom right
#endif

class Screen {
public:
  bool init();

  void setRotation(uint8_t rotation);
  int16_t width() const;
  int16_t height() const;

  void fillScreen(uint16_t colour);
  void fillRect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t colour);
  void drawCircle(int32_t x, int32_t y, int32_t r, uint16_t colour);

  void setTextSize(uint8_t size);
  void setTextColor(uint16_t fg, uint16_t bg);
  void setTextDatum(uint8_t datum);

  void drawString(const char* text, int32_t x, int32_t y);
  void drawString(const String& text, int32_t x, int32_t y);

  int16_t textWidth(const char* text) const;
  int16_t textWidth(const String& text) const;
  int16_t fontHeight() const;

  // AMOLED brightness is a panel command; there is no backlight pin.
  void setBrightness(uint8_t brightness);

  static uint16_t color565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
  }

private:
  // The built-in 5x7 glyph occupies a 6x8 cell once inter-character spacing is
  // included, which is what both Adafruit_GFX and TFT_eSPI's font 1 assume.
  static constexpr int16_t kGlyphWidth = 6;
  static constexpr int16_t kGlyphHeight = 8;

  Arduino_DataBus* _bus = nullptr;
  // Concrete type, not Arduino_GFX*: setBrightness is declared on the OLED
  // branch of the hierarchy, not on the common base.
  Arduino_CO5300* _gfx = nullptr;
  uint8_t _textSize = 1;
  uint8_t _datum = TL_DATUM;
};

#endif  // board selection
