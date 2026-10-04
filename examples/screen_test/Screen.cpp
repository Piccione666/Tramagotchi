// Standalone display bring-up sketch: prints "Screen Works" if the TFT wiring
// and the TFT_eSPI build_flags in platformio.ini are right. Not part of the
// firmware build; to run it, temporarily swap it in for src/main.cpp.
#include <Arduino.h>
#include <TFT_eSPI.h> // Include the graphics library

TFT_eSPI tft = TFT_eSPI();

void setup() {
  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(10, 10);
  tft.print("Screen Works");
}

void loop() {
  // put your main code here, to run repeatedly:
}

// no-op
