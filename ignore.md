# AGENTS.md

## Project Overview
- Purpose: ESP32-C3 TFT monitor that shows the next two Wiener Linien departures.
- Behavior: Polls the Wiener Linien realtime monitor API every 15 seconds, parses JSON, and displays countdowns.
- Display: 240x240 TFT using TFT_eSPI.

## Environment
- OS: Windows
- Shell: PowerShell
- Build tool: PlatformIO
- Framework: Arduino

## Key Files
- `src/main.cpp`: WiFi, HTTP fetch, JSON parsing, scheduling.
- `src/display.cpp`: All TFT drawing and layout logic.
- `include/display.h`: Display API and `Departure` struct.
- `include/config.h`: WiFi credentials, `RBL_ID`, optional `LINE_FILTER`.
- `platformio.ini`: PlatformIO config (uses `src` as source dir).
- `wienerlinien-echtzeitdaten-dokumentation.pdf`: Data format reference.

## Build & Run
- Build: `pio run`
- Upload: `pio run -t upload`
- Monitor: `pio device monitor`

## Project Conventions
- Keep networking and parsing in `src/main.cpp`.
- Keep all graphics in `src/display.cpp`.
- Update display sizing or layout only in `src/display.cpp`.
- `RBL_ID` in `include/config.h` must match the stop, e.g. `"1470"`.
- `LINE_FILTER` can be empty for “next two across all lines”.
