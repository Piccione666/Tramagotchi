# DHX-ESP32-S3-AM175 — hardware reference

Drop this file into any project that targets this board. Everything marked
**Verified** was read off the device itself with a probe sketch on 2026-09-11,
not copied from a vendor datasheet. Everything marked **Unverified** is a
plausible assumption that has not been proven — treat it as a starting guess and
confirm before relying on it.

The board is an ESP32-S3 with a 1.75-inch round AMOLED. It is not a Waveshare
product, but it is closely derived from the **Waveshare ESP32-S3-Touch-AMOLED-1.75**:
the I2C pins and the peripheral line-up match that board exactly. Two things
differ, so do not assume the Waveshare documentation applies wholesale:

- Flash is **32MB**, where the Waveshare board has 16MB.
- **No PCF85063 real-time clock** answered on the I2C bus. The Waveshare board has one at 0x51.

When searching for documentation, search for the Waveshare part, not "DHX" —
the DHX name returns nothing.

---

## Silicon — Verified

| Property | Value |
| --- | --- |
| SoC | ESP32-S3, dual core Xtensa LX7 |
| Revision | v0.2 (esptool) |
| Clock | 240 MHz |
| Flash | 32 MB, quad I/O (per eFuse), manufacturer `0xC8`, device `0x4019` |
| PSRAM | 8 MB, **octal** |
| USB | Native USB Serial/JTAG, VID `303A`, PID `1001` |

PSRAM is octal, so the build must use `qio_opi`. With the default `qio_qspi`
the chip boots but `psramFound()` returns false and you silently lose all 8MB.

GPIO 26–37 are consumed by the in-package flash and octal PSRAM. Never drive
them. GPIO 19 and 20 are the native USB pins and carry the serial console.

---

## Display — Verified

1.75-inch round AMOLED, **466 × 466**, CO5300 controller on a QSPI bus. Pin map
and geometry were confirmed visually: a circle of radius 232 drawn at the centre
touches all four edges evenly, and corner markers all land on screen.

| Signal | GPIO |
| --- | --- |
| CS | 12 |
| SCLK | 38 |
| D0 | 4 |
| D1 | 5 |
| D2 | 6 |
| D3 | 7 |
| RST | 39 |

The panel sits at a **column offset of 6** inside the controller's memory. Omit
it and the whole image shifts sideways.

There is no backlight pin. Brightness is a panel command, `setBrightness(0-255)`.

### Driver choice

Use **Arduino_GFX** (`moononournation/GFX Library for Arduino`). Its
`Arduino_CO5300` driver is the only one confirmed working here.

```cpp
Arduino_DataBus *bus = new Arduino_ESP32QSPI(12, 38, 4, 5, 6, 7);
Arduino_CO5300  *gfx = new Arduino_CO5300(bus, 39, 0, 466, 466, 6, 0, 0, 0);
gfx->begin();
gfx->setBrightness(160);
```

Declare the pointer as `Arduino_CO5300*`, **not** `Arduino_GFX*`.
`setBrightness` is declared on the OLED branch of the hierarchy, so a base-class
pointer fails to compile.

**LovyanGFX does not work here.** Its `Panel_CO5300` is gated behind
`LGFX_USE_QSPI`, which needs an ESP-IDF QSPI path not available under this
Arduino setup, and its CO5300 defaults are sized for a 502x410 LilyGO panel
anyway.

---

## I2C bus — Verified

**SDA = GPIO 15, SCL = GPIO 14.** Found by sweeping every plausible pin pair.
Five devices answered:

| Address | Chip | Role |
| --- | --- | --- |
| 0x5A | CST9217 | Capacitive touch controller |
| 0x34 | AXP2101 | Power management |
| 0x6B | QMI8658 | 6-axis IMU |
| 0x18 | ES8311 | Audio codec |
| 0x40 | ES7210 | Microphone ADC |

Nothing answered at 0x51, so assume there is no RTC.

The panel powers up without touching the AXP2101, so PMU configuration is not
required just to get a picture.

---

## Touch — CST9217

The controller is **Verified** present at 0x5A and initializes successfully.
Its two control pins are **Unverified**: they are taken from the Waveshare pin
map, and because the chip already responds without them, a wrong value would not
have shown up as a failure.

| Signal | GPIO | Status |
| --- | --- | --- |
| INT | 11 | Unverified |
| RESET | 40 | Unverified |

### Read protocol

Write the 16-bit register `0xD000`, read a 15-byte block, then acknowledge by
writing `0xAB` back to the same register. Without that acknowledgement the
controller stops producing new reports.

In the returned block: byte 5 low 7 bits is the finger count, byte 6 must equal
`0xAB` for the frame to be valid, and the top bit of byte 4 flags a palm or
cover gesture rather than a real touch.

**Critical gotcha.** The read must end the address write with a **full stop**,
not a repeated start:

```cpp
Wire.endTransmission(true);   // correct
Wire.endTransmission(false);  // fails: requestFrom() returns i2cWriteReadNonStop Error -1
```

This cost real debugging time. With a repeated start the controller responds to
an address probe but every data read fails, so the chip looks present and dead
at the same time.

---

## Buttons

There is **no free user button and no spare GPIO** for one — the display and the
I2C bus take everything convenient. Available inputs:

- **Touchscreen.** The natural primary control.
- **BOOT button on GPIO 0**, active low with an internal pull-up. Usable at
  runtime, but holding it during power-up enters firmware download mode instead
  of running the sketch, so it is a poor primary control.
- **PWR button**, wired to the AXP2101 rather than to a GPIO. Read it through
  the PMU's interrupt registers over I2C. **Unverified** — not exercised.
- **8-pin header** exposing GPIO 16, 17 and 18 plus a UART, if you want to wire
  your own button. **Unverified.**

---

## Unverified pin assignments

These come from the Waveshare pin map and were never exercised. The I2C and
display pins matched that map exactly, so these are likely right, but confirm
before trusting them.

| Function | GPIO |
| --- | --- |
| I2S MCLK / BCLK / WS / DOUT / DIN | 42 / 9 / 45 / 10 / 8 |
| Speaker amplifier enable | 46 |
| SD card CLK / CMD / DATA / CS | 2 / 1 / 3 / 41 |

---

## Toolchain

Arduino_GFX's CO5300 driver needs **ESP32 Arduino core 3.x** (it includes
`esp32-hal-periman.h`). The official PlatformIO `espressif32` platform still
ships core 2.x and **will not build this**, failing with a missing header.

Use the **pioarduino** fork. Known working: platform `53.03.13`, which provides
arduino-esp32 3.1.3 on ESP-IDF 5.3.

PlatformIO rejects `platform = espressif32@53.03.13` with `SemanticVersionError:
Invalid simple block` because of the leading zeros. Reference it another way —
by install path, or by release URL:

```ini
platform = https://github.com/pioarduino/platform-espressif32/releases/download/53.03.13/platform-espressif32.zip
board = esp32-s3-devkitc-1
framework = arduino

board_build.arduino.memory_type = qio_opi
board_upload.flash_size = 32MB
board_upload.maximum_size = 33554432

build_flags =
    -DBOARD_HAS_PSRAM
    -DARDUINO_USB_MODE=1
    -DARDUINO_USB_CDC_ON_BOOT=1
```

Both USB flags are needed for `pio device monitor` to work over the same cable
used for flashing.

The first build downloads roughly 1GB of toolchains and takes a long time with
no console output. It is not hung. Compilation itself wants about 1GB of RAM per
parallel job, so on a loaded machine pass `-j 2` or it dies with
`cc1plus.exe: out of memory`.

---

## Factory firmware

The board shipped with a large application, almost certainly a Xiaozhi-style AI
assistant: a 9MB app partition beside 9MB of assets. Its original partition
table was:

| Partition | Type | Offset | Size |
| --- | --- | --- | --- |
| nvsfactory | nvs | 0x9000 | 200 K |
| nvs | nvs | 0x3B000 | 840 K |
| otadata | ota | 0x10D000 | 8 K |
| phy_init | phy | 0x10F000 | 4 K |
| factory | app | 0x110000 | 9 M |
| ota_0 | app | 0xA10000 | 3.9 M |
| ota_1 | app | 0xE00000 | 3.9 M |
| assets | spiffs | 0x11F0000 | 9 M |
| storage | spiffs | 0x1AF0000 | 5 M |

The factory application and assets were **not** backed up and are gone. Only the
region below `0x110000` was saved, which covers the bootloader, the original
partition table and both NVS regions, including the per-device data that cannot
be re-downloaded.

Flash reads over this USB link run at roughly 70 kbit/s, so a full 32MB dump
takes about two hours. Budget for that before erasing anything you want back.

---

## Gotchas worth remembering

- `Wire.end()` followed by `Wire.begin()` in a loop **deadlocks** the ESP32 I2C
  driver after a few iterations. To scan many candidate pin pairs, bit-bang the
  lines instead.
- Calling `gfx->begin()` twice in one boot aborts with `ESP_ERR_INVALID_STATE`.
  The QSPI bus can only be initialized once per reset.
- The USB Serial/JTAG port stays enumerated even when the firmware has crashed
  or hung, so a visible COM port proves nothing about the sketch.
- Print diagnostics from `loop()` with an explicit flush rather than from
  `setup()`. If a sketch hangs during setup, a console attached afterwards sees
  only silence.
- AMOLED panels burn in. Avoid a bright, static image for long periods; this
  board defaults to brightness 160 of 255 for that reason.
