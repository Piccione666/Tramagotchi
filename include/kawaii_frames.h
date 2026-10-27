#pragma once

#include <Arduino.h>

// Kawaii faces live in data/kawaii.txt, not here: the file ships in the LittleFS
// image, so the stop data update (GitHub Pages) delivers face changes without a
// firmware flash. Without the file (or without a valid line) kawaii mode is off.
//
// Format: one pair per line, "main brief", split by whitespace, '#' starts a comment.
// Each kawaii screen picks one pair at random and alternates its two faces: the main
// face for KAWAII_FRAME_A_MS, then the brief one for KAWAII_FRAME_B_MS.
// Faces from different pairs are never combined.

constexpr uint8_t kKawaiiMaxPairs   = 16;  // pairs kept from /kawaii.txt
constexpr uint8_t kKawaiiMaxFaceLen = 5;   // chars per face: 5 x 48 px fills the 240 px display
