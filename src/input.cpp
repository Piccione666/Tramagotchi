#include "input.h"

#include "board_config.h"
#include "config.h"
#include "pinout.h"

#if defined(BOARD_TRAMAGOTCHI_S3)
#include <Wire.h>
#endif

namespace {

InputMode gMode = InputTap;

#if defined(BOARD_TRAMAGOTCHI_S3)

// ── Minimal CST9217 reader ────────────────────────────────────────────────────
//
// Only "is a finger down" is needed, not coordinates, so this talks to the
// controller directly rather than pulling in a full touch library. The register
// layout follows the CST92xx protocol: write the 16-bit address 0xD000, read a
// status block, then acknowledge it by writing 0xAB back to the same address.
// Without that acknowledgement the controller stops producing new reports.

constexpr uint16_t kRegRead = 0xD000;
constexpr uint8_t  kAck     = 0xAB;
constexpr size_t   kReportBytes = TOUCH_MAX_POINTS * 5 + 5;

bool gTouchReady = false;
bool gTouchHeld = false;
uint32_t gLastTouchPollMs = 0;

bool readReport(uint8_t* buffer, size_t length) {
  Wire.beginTransmission(TOUCH_I2C_ADDRESS);
  Wire.write((uint8_t)(kRegRead >> 8));
  Wire.write((uint8_t)(kRegRead & 0xFF));
  // A full stop, not a repeated start. This controller rejects the combined
  // write-read transaction: requestFrom() then fails with i2cWriteReadNonStop
  // Error -1 on every poll.
  if (Wire.endTransmission(true) != 0) return false;

  if (Wire.requestFrom((uint8_t)TOUCH_I2C_ADDRESS, (uint8_t)length) != length) return false;
  for (size_t i = 0; i < length; i++) buffer[i] = (uint8_t)Wire.read();
  return true;
}

void acknowledgeReport() {
  Wire.beginTransmission(TOUCH_I2C_ADDRESS);
  Wire.write((uint8_t)(kRegRead >> 8));
  Wire.write((uint8_t)(kRegRead & 0xFF));
  Wire.write(kAck);
  Wire.endTransmission(true);
}

// Returns the number of fingers currently reported, or 0.
uint8_t readFingerCount() {
  uint8_t buffer[kReportBytes] = {0};
  if (!readReport(buffer, sizeof(buffer))) return 0;
  acknowledgeReport();

  // A stale or empty frame repeats the acknowledgement byte, or reports nothing
  // in the first slot. Byte 6 must carry the marker for the frame to be valid.
  if (buffer[0] == kAck || buffer[0] == 0x00 || buffer[6] != kAck) return 0;

  // The top nibble of byte 4 signals a palm or cover gesture rather than a tap.
  if ((buffer[4] & 0xF0) && ((buffer[4] >> 7) == 0x01)) return 0;

  uint8_t points = buffer[5] & 0x7F;
  if (points > TOUCH_MAX_POINTS) return 0;
  return points;
}

void touchInit() {
  pinMode(TP_INT, INPUT_PULLUP);

  // Hardware reset: the controller needs a low pulse and about 50ms to boot.
  pinMode(TP_RESET, OUTPUT);
  digitalWrite(TP_RESET, LOW);
  delay(20);
  digitalWrite(TP_RESET, HIGH);
  delay(60);

  // Confirm the controller answers at all before trusting it.
  Wire.beginTransmission(TOUCH_I2C_ADDRESS);
  gTouchReady = (Wire.endTransmission() == 0);

  if (gTouchReady) {
    uint8_t discard[kReportBytes];
    if (readReport(discard, sizeof(discard))) acknowledgeReport();
  }
}

bool touchPressed() {
  if (!gTouchReady) return false;

  uint32_t now = millis();
  if (now - gLastTouchPollMs < TOUCH_POLL_INTERVAL_MS) return gTouchHeld;
  gLastTouchPollMs = now;

  gTouchHeld = (readFingerCount() > 0);
  return gTouchHeld;
}

// ── PWR button through the AXP2101 ────────────────────────────────────────────
//
// The PMU latches a flag for each edge of the PWRON line; writing a flag back
// clears it. A press edge starts a press, a release edge ends it once it has
// lasted PMU_MIN_PRESS_MS (both can arrive in the same poll on a quick tap).

bool gPmuReady = false;
bool gPmuHeld = false;
bool gPmuReleasePending = false;
uint32_t gPmuPressMs = 0;
uint32_t gLastPmuPollMs = 0;

bool pmuRead(uint8_t reg, uint8_t& value) {
  Wire.beginTransmission(PMU_I2C_ADDRESS);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((uint8_t)PMU_I2C_ADDRESS, (uint8_t)1) != 1) return false;
  value = (uint8_t)Wire.read();
  return true;
}

bool pmuWrite(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(PMU_I2C_ADDRESS);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

void pmuInit() {
  uint8_t enable = 0;
  gPmuReady = pmuRead(PMU_REG_IRQ_ENABLE2, enable);
  if (!gPmuReady) return;
  // Latch both edges, and drop any press flagged before the firmware started.
  pmuWrite(PMU_REG_IRQ_ENABLE2, enable | PMU_PKEY_PRESS_BIT | PMU_PKEY_RELEASE_BIT);
  pmuWrite(PMU_REG_IRQ_STATUS2, PMU_PKEY_PRESS_BIT | PMU_PKEY_RELEASE_BIT);
}

bool pmuPressed() {
  if (!gPmuReady) return false;

  uint32_t now = millis();
  if (now - gLastPmuPollMs >= TOUCH_POLL_INTERVAL_MS) {
    gLastPmuPollMs = now;
    uint8_t flags = 0;
    if (pmuRead(PMU_REG_IRQ_STATUS2, flags)) {
      flags &= (PMU_PKEY_PRESS_BIT | PMU_PKEY_RELEASE_BIT);
      if (flags) {
        pmuWrite(PMU_REG_IRQ_STATUS2, flags);
#if SERIAL_DEBUG_ENABLED
        Serial.printf("[button] PWR flags 0x%02X\n", flags);
#endif
      }
      if ((flags & PMU_PKEY_PRESS_BIT) && !gPmuHeld) {
        gPmuHeld = true;
        gPmuPressMs = now;
      }
      if (flags & PMU_PKEY_RELEASE_BIT) gPmuReleasePending = gPmuHeld;
    }
  }
  if (gPmuReleasePending && now - gPmuPressMs >= PMU_MIN_PRESS_MS) {
    gPmuHeld = false;
    gPmuReleasePending = false;
  }
  return gPmuHeld;
}

void bootButtonInit() { pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP); }
bool bootButtonPressed() { return digitalRead(BOOT_BUTTON_PIN) == LOW; }

#endif  // BOARD_TRAMAGOTCHI_S3

}  // namespace

void input_init() {
#if defined(BOARD_TRAMAGOTCHI_S3)
  Wire.begin(IIC_SDA, IIC_SCL, 400000U);
  touchInit();
  pmuInit();
  bootButtonInit();
#elif !NO_BUTTON_ATTACHED
  // Pull the pin to the *idle* level so the released state is defined: a button
  // to 3V3 needs a pull-down, one to GND needs a pull-up.
  pinMode(TOUCH_PIN, TOUCH_ACTIVE_LOW ? INPUT_PULLUP : INPUT_PULLDOWN);
#endif
}

void input_set_mode(InputMode mode) { gMode = mode == InputButton ? InputButton : InputTap; }
InputMode input_mode() { return gMode; }

bool input_pressed() {
#if defined(BOARD_TRAMAGOTCHI_S3)
  bool source = gMode == InputButton ? pmuPressed() : touchPressed();
  return source || bootButtonPressed();
#elif !NO_BUTTON_ATTACHED
  bool raw = (bool)digitalRead(TOUCH_PIN);
  return TOUCH_ACTIVE_LOW ? !raw : raw;
#else
  return false;
#endif
}

bool input_touch_ready() {
#if defined(BOARD_TRAMAGOTCHI_S3)
  return gTouchReady;
#else
  return false;
#endif
}

bool input_pmu_ready() {
#if defined(BOARD_TRAMAGOTCHI_S3)
  return gPmuReady;
#else
  return false;
#endif
}
