# Tramagotchi (ESP32-C3 and ESP32-S3)

A round display showing the next two Wiener Linien departures for a few configured stops, cycling
between them. One firmware, two builds: `Tramagotchi_C3` (ESP32-C3, 240x240 GC9A01 TFT) and
`Tramagotchi_S3` (DHX-ESP32-S3-AM175, 466x466 AMOLED). Brand: Officina Canfora (never "Canfomo").

**The documentation lives in the vault note**
`C:\Users\paolo\Desktop\02_LIBRARYS\Obsidian Vault\Doku\tramagotchi\Tramagotchi.md`: hardware,
commands, firmware architecture, web interface, data pipeline, API notes, troubleshooting, To Do
and history. Read it before larger changes. At the end of each session update its status block
(last three steps, next steps), tick/add its To Do items, add a Version history row, and fix
anything that went stale. Never put secrets there or here.

## Rules

- Networking, parsing and scheduling in `src/main.cpp`; all drawing in `src/display.cpp`, written
  once against `include/screen.h` (TFT_eSPI on the C3, Arduino_GFX on the S3). What differs per
  board: `include/board_config.h`, `include/pinout.h`, `include/screen.h`, `src/input.cpp`.
  Feature switches live in headers; `platformio.ini` holds only toolchain/library config and the
  C3's TFT wiring.
- Never edit files under `.pio/libdeps`: the TFT setup is in `build_flags`.
- Every full-screen clear goes through `clearScreen()`, never `tft.fillScreen`.
- Towards is only a display label, never a filter.
- WiFi credentials only in the gitignored `include/secrets.h`; never stage it. Mask values when
  printing secrets-adjacent output (e.g. `sed -E 's/"[^"]*"/"<v>"/'`).
- `tools/*.ps1` are BOM-less, so PowerShell 5.1 reads them as ANSI: keep them pure ASCII.
- The config page stays minimal, with no explanatory text. `README.md` is the end-user guide: web
  interface only, very short, non-technical.
- The flasher's setup fields (WiFi, stops) are `bundle_extras.json`; their NVS keys must match
  `src/main.cpp` (namespaces `wifi`, `stations` and their `version` values).

## Build

- `pio run -e Tramagotchi_C3` / `-e Tramagotchi_S3`. USB: `-t upload --upload-port COMxx`, data
  `-t uploadfs`. OTA: `-t uploadota`, `-t uploadfsota`. Port: `pio device list`, VID:PID `303A:1001`.
- Build from PowerShell (or VS Code), not Git Bash: the S3 build fails there.
- The two builds need different Arduino cores (C3: 2.0.14 on `espressif32@6.5.0`; S3: 3.3.7 on
  pioarduino) in the same `framework-arduinoespressif32` package folder, so each overwrites the
  other. The C3 reinstalls its own; before an S3 build after a C3 one, run
  `pio pkg install -e Tramagotchi_S3`, or the build stops with "expected str ... not NoneType".
- Bundle for the flasher: build both (`pio run -e ...`, then `-t buildfs` for each), then
  `python scripts/make_stations_tree.py` and `python scripts/make_bundle.py`. Files are named like
  Tami Remote: `firmware/Tramagotchi_C3_v1.0_2026-10-27.bin`, `firmware/Tramagotchi_bundle_v1.0_<date>.zip`.
  The version counts up per source change (`firmware_version.json`, keep it in git).
- Flashing firmware was blocked by the permission classifier on 2026-10-02 (`uploadfs` was allowed):
  ask the user to flash, or to add a permission rule, instead of retrying.
- No `python3`: use `python` or `~/.platformio/penv/Scripts/python.exe` (pyserial, esptool).
