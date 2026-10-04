#pragma once

// Text-based emoji shown on the kawaii screens.
// Each string is auto-scaled to fill the display width.
// Two different frames are picked at random and alternated per KAWAII_FRAME_A_MS / KAWAII_FRAME_B_MS.
// Add, remove, or change strings freely — just keep kKawaiiFrameCount in sync.

constexpr uint8_t kKawaiiFrameCount = 10;

static const char* const kKawaiiFrames[kKawaiiFrameCount] = {
  "^_^",   // happy
  "^w^",   // uwu
  "-_-",   // unimpressed
  "o_O",   // surprised
  "._.",   // gentle / sad
  "*_*",   // amazed
  "x_x",   // dizzy
  ";_;",   // crying
  "-.-",   // sleepy
  "*u*",   // content
};