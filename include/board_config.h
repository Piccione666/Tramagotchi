#pragma once

// ── Board profile ─────────────────────────────────────────────────────────────
//
// BOARD_TRAMAGOTCHI_S3 — DHX-ESP32-S3-AM175
//   1.75" round AMOLED, 466x466, CO5300 controller on a QSPI bus.
//   Everything below was read off the board itself with a probe sketch, not copied
//   from a datasheet (references/DHX-ESP32-S3-AM175.md), except the PMU's button flags. Its I2C bus and chip set match the Waveshare
//   ESP32-S3-Touch-AMOLED-1.75 exactly; the flash is 32MB rather than 16MB.
//
//   Confirmed on the device:
//     ESP32-S3 dual core @ 240MHz, 32MB flash, 8MB octal PSRAM
//     I2C on SDA 15 / SCL 14 carrying:
//       0x5A CST9217 touch      0x34 AXP2101 power management
//       0x6B QMI8658 IMU        0x18 ES8311 codec      0x40 ES7210 mic ADC
//
// BOARD_TRAMAGOTCHI_C3 — the original ESP32-C3 with a 240x240 SPI TFT.
// ─────────────────────────────────────────────────────────────────────────────

#if !defined(BOARD_TRAMAGOTCHI_S3) && !defined(BOARD_TRAMAGOTCHI_C3)
#error "No board profile selected. Define BOARD_TRAMAGOTCHI_S3 or BOARD_TRAMAGOTCHI_C3 in platformio.ini."
#endif

#if defined(BOARD_TRAMAGOTCHI_S3)

#define BOARD_NAME "DHX-ESP32-S3-AM175"

// ── Display: CO5300 AMOLED over QSPI ──────────────────────────────────────────
#define LCD_WIDTH   466
#define LCD_HEIGHT  466
#define LCD_SCLK    38
#define LCD_CS      12
#define LCD_SDIO0   4
#define LCD_SDIO1   5
#define LCD_SDIO2   6
#define LCD_SDIO3   7
#define LCD_RESET   39
// The 466-wide panel is addressed inside a wider controller memory; every
// column is shifted right by this much. Getting it wrong skews the image.
#define LCD_COL_OFFSET 6
#define LCD_ROW_OFFSET 0

// AMOLED brightness is a panel command, not a backlight pin. 0-255.
// AMOLED panels also burn in, so avoid a permanently bright static image.
#define LCD_BRIGHTNESS 160

// ── I2C bus shared by touch, PMU, IMU and audio ───────────────────────────────
#define IIC_SDA 15
#define IIC_SCL 14

// ── Touch: CST9217 ────────────────────────────────────────────────────────────
#define TOUCH_I2C_ADDRESS 0x5A
#define TP_INT   11
#define TP_RESET 40

// ── Power management: AXP2101 ─────────────────────────────────────────────────
#define PMU_I2C_ADDRESS 0x34
// The PWR button is wired to the AXP2101, not to a GPIO: its presses latch IRQ
// flags that are read (and cleared by writing them back) over I2C. Register and
// bits follow XPowersLib's AXP2101 map (IRQ status 2); unverified on this board.
// [button] lines on the serial log show the raw flags when the button is used.
#define PMU_REG_IRQ_ENABLE2  0x41
#define PMU_REG_IRQ_STATUS2  0x49
#define PMU_PKEY_RELEASE_BIT 0x01  // PWRON positive edge
#define PMU_PKEY_PRESS_BIT   0x02  // PWRON negative edge

#elif defined(BOARD_TRAMAGOTCHI_C3)

#define BOARD_NAME "ESP32-C3 + 240x240 TFT"

#define LCD_WIDTH  240
#define LCD_HEIGHT 240

#endif
