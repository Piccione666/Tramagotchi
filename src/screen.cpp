#include "screen.h"

#if defined(BOARD_TRAMAGOTCHI_S3)

bool Screen::init() {
  _bus = new Arduino_ESP32QSPI(LCD_CS, LCD_SCLK,
                               LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
  _gfx = new Arduino_CO5300(_bus, LCD_RESET, 0 /* rotation */,
                            LCD_WIDTH, LCD_HEIGHT,
                            LCD_COL_OFFSET, LCD_ROW_OFFSET, 0, 0);
  if (!_gfx->begin()) return false;

  _gfx->fillScreen(TFT_BLACK);
  setBrightness(LCD_BRIGHTNESS);
  return true;
}

void Screen::setRotation(uint8_t rotation) {
  if (_gfx) _gfx->setRotation(rotation);
}

int16_t Screen::width() const { return _gfx ? _gfx->width() : LCD_WIDTH; }
int16_t Screen::height() const { return _gfx ? _gfx->height() : LCD_HEIGHT; }

void Screen::fillScreen(uint16_t colour) {
  if (_gfx) _gfx->fillScreen(colour);
}

void Screen::fillRect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t colour) {
  if (_gfx) _gfx->fillRect((int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h, colour);
}

void Screen::drawCircle(int32_t x, int32_t y, int32_t r, uint16_t colour) {
  if (_gfx) _gfx->drawCircle((int16_t)x, (int16_t)y, (int16_t)r, colour);
}

void Screen::setTextSize(uint8_t size) {
  _textSize = size ? size : 1;
  if (_gfx) _gfx->setTextSize(_textSize);
}

void Screen::setTextColor(uint16_t fg, uint16_t bg) {
  if (_gfx) _gfx->setTextColor(fg, bg);
}

void Screen::setTextDatum(uint8_t datum) { _datum = datum; }

int16_t Screen::textWidth(const char* text) const {
  if (!text) return 0;
  return (int16_t)(strlen(text) * kGlyphWidth * _textSize);
}

int16_t Screen::textWidth(const String& text) const {
  return (int16_t)(text.length() * kGlyphWidth * _textSize);
}

int16_t Screen::fontHeight() const {
  return (int16_t)(kGlyphHeight * _textSize);
}

void Screen::drawString(const char* text, int32_t x, int32_t y) {
  if (!_gfx || !text) return;

  const int16_t w = textWidth(text);
  const int16_t h = fontHeight();

  // Adafruit_GFX places the cursor at the top-left of the text, so shift the
  // anchor point to the requested datum.
  int32_t left = x;
  switch (_datum) {
    case TC_DATUM: case MC_DATUM: case BC_DATUM: left = x - w / 2; break;
    case TR_DATUM: case MR_DATUM: case BR_DATUM: left = x - w;     break;
    default: break;
  }

  int32_t top = y;
  switch (_datum) {
    case ML_DATUM: case MC_DATUM: case MR_DATUM: top = y - h / 2; break;
    case BL_DATUM: case BC_DATUM: case BR_DATUM: top = y - h;     break;
    default: break;
  }

  _gfx->setCursor((int16_t)left, (int16_t)top);
  _gfx->print(text);
}

void Screen::drawString(const String& text, int32_t x, int32_t y) {
  drawString(text.c_str(), x, y);
}

void Screen::setBrightness(uint8_t brightness) {
  if (_gfx) _gfx->setBrightness(brightness);
}

#endif  // BOARD_TRAMAGOTCHI_S3
