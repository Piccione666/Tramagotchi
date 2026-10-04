# Tramagotchi — working notes

Claude's single source of truth for this project: state, architecture, commands, gotchas, TODOs.
**Maintain it:** tick/add TODOs as work happens, append to the session log, fix anything that
went stale. Never put secrets here (WiFi credentials live in the gitignored `include/secrets.h`).
When reporting secrets-adjacent output, mask the values (e.g. `sed -E 's/"[^"]*"/"<v>"/'`).

## What it is

ESP32-C3 + 240x240 round GC9A01 TFT showing the next two Wiener Linien departures for a few
configured stops, cycling between them. Web config UI, captive setup portal, OTA, a "kawaii"
emoji mode. Brand: Officina Canfora (never "Canfomo").

## Hardware

- Board `esp32-c3-devkitm-1`, native USB Serial/JTAG (VID:PID `303A:1001`).
- Display GC9A01 over SPI: MISO -1, MOSI 3, SCLK 4, CS 0, DC 1, RST 2, 40 MHz. Fonts GLCD,
  2, 4, 6, 7, 8, GFXFF, SMOOTH_FONT. Configured via `build_flags` in platformio.ini
  (`-DUSER_SETUP_LOADED=1`, `USER_SETUP_ID=46`), against a stock pinned TFT_eSPI 2.5.43. Never edit
  files under `.pio/libdeps`. (Before 2026-10-02 the setup was hand-edited there; the build_flags
  version was proven instruction- and data-identical by objdump diff of TFT_eSPI.cpp.o.)
- Bring-up sketch: `examples/screen_test/Screen.cpp` (not built; swap in for src/main.cpp).
- Button on GPIO 5 to 3V3, active high, INPUT_PULLDOWN (`include/pinout.h`).
- Flash layout (`partitions_custom.csv`, verified on device 2026-10-02): nvs 0x9000/0x5000,
  otadata 0xE000/0x2000, app0 0x10000/0x140000, app1 0x150000/0x140000,
  spiffs(LittleFS) 0x290000/0x170000.

## Commands (this machine: Windows, PowerShell 5.1 + Git Bash)

- Build: `pio run` · Flash firmware USB: `pio run -t upload --upload-port COMxx`
- Flash data (LittleFS) USB: `pio run -t uploadfs --upload-port COMxx`. Only rewrites the
  spiffs partition; NVS (stations, WiFi) survives.
- OTA: `pio run -t uploadota` (firmware) and `pio run -t uploadfsota` (LittleFS). The latter is
  built into platform espressif32 6.5.0 (the builder adds `--spiffs`); `tools/configure_ota_target.py`
  sets espota + `Tramagotchi.local` for both. Web alternatives: `/firmware`, `/fsimage`.
- Permissions: flashing firmware (`pio run -t upload`) was blocked by the auto-mode classifier as a
  "production deploy" on 2026-10-02. `uploadfs` (data only) was allowed. Ask the user to flash, or to
  add a permission rule, instead of retrying.
- Find the port: `pio device list`, look for `303A:1001` (was COM23). COM8/12/15/17 are
  Bluetooth, not the ESP.
- Serial log: `SERIAL_DEBUG_ENABLED 1` prints `[boot]`, `[fetch]`, `[button]` at 115200. Read it
  non-interactively with pyserial from `~/.platformio/penv/Scripts/python.exe`.
- Device HTTP: `http://tramagotchi.local/` (mDNS resolves from this PC); `/status` gives ip/rssi/stations.
- Read the partition table: `python ~/.platformio/packages/tool-esptoolpy/esptool.py --chip esp32c3
  --port COMxx read_flash 0x8000 0xC00 pt.bin`, then parse the 32-byte entries (magic AA50).
- Refresh WL static data: `tools/update_wl_data.ps1`, then `uploadfs`.
- No `python3` here: use `python` (3.11, has pypdf) or the penv python (has pyserial, esptool).
  `pio` is on PATH. The Read tool can't render PDFs (no poppler), so use pypdf.

## Firmware architecture

Conventions: networking/parsing/scheduling in `src/main.cpp`, all drawing in `src/display.cpp`.
Feature switches live in headers; platformio.ini holds only toolchain/library config (including
the TFT wiring).

**Config headers**

- `include/secrets.h` (gitignored; template `include/secrets.example.h`): `WIFI_SSID[_2|_3]`/
  `WIFI_PASS…`. Pulled in by config.h via `__has_include`; missing slots default to "" in main.cpp.
- `include/config.h`: `DEVICE_LOCAL_HOSTNAME`
  "Tramagotchi", connect timeout/attempts, setup AP (open "Tramagotchi", 192.168.4.1), static IP
  (off), `STATION_CONFIG_MODE` (0 = compiled list, 1 = web/NVS), `STATION_CONFIG_MAX_COUNT` 5,
  `SERIAL_DEBUG_ENABLED`, `STATION_CYCLE_INTERVAL_MS` 6000, `NO_DATA_THRESHOLD_MS` 1000.
- `include/graphics_config.h`: `PURGE_EMOJI`, `SPLASH_ENABLED`, `POPUP_KAWAII_ENABLED` (every
  20 min for 12 s), `NO_DATA_MODE` (0 text, 1 DVD-bounce ghost, 2 sad emoji), `GRACE_DISPLAY_MODE`,
  plus all text sizes and offsets.
- `include/stations/stations.h`: `STATION_PRESET` 0–4 with `STATION_LIST` `X(rbl,name,line,towards)`,
  and `DEPARTURE_BLACKLIST` `B("…")` (substring match on "line towards").
  **Default is 0 = no stations** (user's choice 2026-10-02: the product ships empty and stops are
  added on the web page). Dev presets kept: 1 TDG11 = 1470/49 "Volkstheater", 4933/U3 "Simmering",
  157/51A "Hietzing"; 2 RL39 = 352, 4433; 3 OWA = 1379, 7472, 1392; 4 PHILO = 1421, 2263, 1420, 2262.
  An empty STATION_LIST compiles to a zero-length array (GCC extension, no warning).
- Zero stations: the device just shows the no-data screen (sad face). The user explicitly said
  that's fine, so don't add a special "no stations" screen.
- `include/kawaii_frames.h`: emoji strings for kawaii mode.

**Boot (`setup`)**: display_init → splash → stations from NVS (else STATION_LIST) → mount
LittleFS (format on fail) → connectWifi (NVS creds first, then config.h creds, 3 tries each)
→ mDNS + ArduinoOTA + config web server. If WiFi fails → open setup AP + captive DNS portal.
The portal scans networks once before the AP starts (`scanSetupNetworks`, deduped, max 20) and
again only on `/?rescan=1` (the "Scan again" button). Never scan per request: captive-portal probes
all land on the setup page.

**Fetch (`fetchTask`, FreeRTOS, 12 KB stack)**: one HTTPS GET per cycle,
`/ogd_realtime/monitor?rbl=A&rbl=B…` (unique RBLs), streamed into a filtered JsonDocument.
For each station it takes the two soonest countdowns where the line name matches (case- and
umlaut-insensitive; an empty line matches any). **Towards is only a display label, never a filter.**
Results go to `gPending` under `gMutex`; `loop()` consumes them. NoDataReason: NoMatch (stop has
departures but not this line), NoService (line is here but nothing is running), Network.

**loop()**: web server/DNS, ArduinoOTA, button (short = next station, ≥1.3 s = toggle kawaii),
auto-cycle every 6 s, fetch every 15 s (500 ms retries during a failure streak), no-data grace
window, a station is skipped after 3 no-data cycles but stays in the rotation, kawaii popup.

**Storage (NVS/Preferences)**: ns `stations` {version, list}, ns `wifi` {version, ssid, pass};
`kStorageVersion` 1 (a mismatch clears the namespace). List format: one `RBL,StationName,Line,Towards`
per line, max 5 rows / 768 bytes. Saving an empty list is allowed: it clears the `stations`
namespace, so the device falls back to STATION_LIST (empty by default). Text with no valid row is
still rejected, so a typo can't wipe the list.

**Web endpoints** (only when STA is connected): `/` `/config` (UI), `/status`, `/lines`,
`/stops?lineId=`, POST `/save`, `/upload` `/upload-file` (single data file, path must be
`line_options.csv` or `stops/…`), `/fsimage` (whole LittleFS image), `/firmware`. **No auth
anywhere** (deferred). The setup portal serves `/` and `/wifi-save`. The live check (`/preview`,
"Check live") was removed 2026-10-02 at the user's request because it didn't work.
The UI's look is a neumorphic design shared with the NTS Radio project (its `readmes/DESIGN_SYSTEM.md`),
with user-requested deviations: the h1 title is 20px bold.

Config page (user-tuned 2026-10-02; keep it minimal, no explanatory text):
title + "by Officina Canfora" only → Line dropdown → Stop dropdown (first entry "Pick a station...",
or "Choose a line first" / "Loading stops..." / "No stop data"; depot/HASTUS/detour stops hidden; no
search field) → direction checkboxes → Add → "Station list" textarea (no hint text) → Save →
"Firmware OTA upload (.bin)" with plain "Choose file" + "Upload firmware" buttons (native file input
hidden off-screen; the picked name shows below; submitting without a file is blocked in JS) → Device IP.
The setup portal page still has its one-line intro under the brand line.

**Stop data update** (`runDataUpdate` in main.cpp, POST `/data-update`, the `Stop data (<version>)`
card with an "Update stop data" button): `DATA_UPDATE_URL` in config.h is a folder URL with
`version.txt` + `littlefs.bin`. The device fetches version.txt and downloads the image only if it
differs from the local `/version.txt`. It uses `httpUpdate.updateSpiffs()` (needs Content-Length;
GitHub Pages sends it), which writes only the data partition, so NVS survives and there's no
reboot. LittleFS is unmounted first and remounted with formatOnFail=false. It runs on the main
task: the display shows "updating data" and the page waits. Auto-check: `DATA_UPDATE_AUTO` 1,
5 min after boot, then every `DATA_UPDATE_CHECK_HOURS` (24), skipped while DATA_UPDATE_URL is "".
An optional POST field `base` overrides the URL for testing (no new exposure: /fsimage already
takes any image). `tools/update_wl_data.ps1` writes `data/version.txt` (yyyy-MM-dd); without it in
the image, every check would re-download. The URL is still empty, so the button answers
"No update source set yet."

**Testing the web UI without flashing:** rebuild the page from the `F("…")` literals of `pageHead` and
`configPageHtml` (decode `\\`, `\"`, `\n`), load it in jsdom (`npm install jsdom@24` in the
scratchpad) with `window.fetch` mocked to serve `data/line_options.csv` and `data/stops/<id>.csv`,
then drive the selects and assert. Beware: conditional literals (e.g. the `msg` div) get included
unconditionally. This caught nothing broken on 2026-10-02 (21/21 checks), but it's the fastest
safety net for the inline JS.

**display.cpp**: `display_departures` (big countdown, line on top, towards auto-fit to the
circle via `fit_text_size`, second countdown below), `display_no_data` + `display_update_no_data`
(bouncing block with ghost trail, or emoji), loading blink animation (also used at countdown 0),
kawaii frames, boot splash, `displayText()` turns UTF-8 umlauts into ASCII for the TFT font.
All full-screen clears go through `clearScreen()`, which also invalidates the "departures on
screen" cache. `display_departures` returns early when the drawn fields (first
line/towards/countdown, second countdown, haveSecond) are unchanged, which prevents refresh
flicker. Any new screen must use `clearScreen()`, not `tft.fillScreen`.
Transfer markers: `stripTransferMarkers()` (C++) and `stripDir()` (JS) drop standalone "U"/"S"
words plus trailing commas, so they handle both "Hietzing U" and "Parlament, U Volkstheater".

## Data pipeline (static OGD → LittleFS → config UI picker only)

The display never reads these files; it only uses the live API by RBL. The files only feed the
web UI's line → stop → direction picker.
1. `tools/update_wl_data.ps1` downloads from `https://www.wienerlinien.at/ogd_realtime/doku/ogd/`
   into `wl_ogv_resources/`: haltepunkte (StopID=RBL, DIVA, StopText), haltestellen, linien,
   fahrwegverlaeufe (LineID, PatternID, StopSeqCount, StopID, Direction), steige, verbindungen,
   and the API doc PDF. It validates each file's first line because the site has bot protection.
2. `generate_direction_options.ps1` → `wl_ogv_resources/line_stop_directions.csv`
   (`LineID|StopID|LineText|Terminal`). The terminus per direction is the end of the longest
   pattern (ties go to the most frequent), so short-turn termini collapse.
3. `generate_line_options.ps1` → `data/line_options.csv` (`LineID|LineText`, realtime lines only,
   sorted by SortingHelp) and `data/stops/<LineID>.csv` (`StopID|StopName|Dir1~Dir2|DIVA`; the DIVA
   column has been unused since the live check was removed, but it's harmless).
   It deletes stale stop files first.

- Names stay UTF-8 in data/ on purpose; the config page converts umlauts when a row is added.
  The scripts are BOM-less, so PS 5.1 reads them as ANSI: keep them pure ASCII (no umlaut literals).
- Deterministic: re-running on the June inputs reproduced the old `data/` byte-for-byte.
- On-device regeneration was rejected: about 2 MB of raw CSV, joins across 87k rows, ~200 KB free
  heap with TLS, LittleFS 1.47 MB with ~0.9 MB used. A hosted option is in the backlog.

## Wiener Linien API notes

- Doc: `wl_ogv_resources/wienerlinien-echtzeitdaten-dokumentation.pdf`, V1.5 (21.05.2026).
  V1.3→1.5 changes are additive only (vehicle.onStop/cooling, lines.platform; trafficInfo
  relatedLines/Stops are now arrays).
- Fields the firmware uses: `monitors[].locationStop.properties.attributes.rbl` (a number),
  `lines[].name`, `lines[].towards`, `departures.departure[].departureTime.countdown`.
- Disruptions: `/ogd_realtime/trafficInfoList?relatedLine=49` (useful to explain "no match").
- `towards` naming changed in 2026: "Ring, Volkstheater U" → "Parlament, U Volkstheater"
  (the U marker is now a prefix). Also WLB → "LB".

## Known temporary situations

- Line 49 is suspended 2026-09-18 → 2026-10-31 (track works); the 46 runs over its route and serves
  RBL 1470. Preset 1's row 1470/49 shows "no match" until then. That's expected, not a bug.

## TODO

Open: next steps

- [ ] **Flash the 2026-10-02 firmware** (`pio run -t upload --upload-port COMxx`, user runs it or grants
      permission) and verify: the screen still works (TFT build_flags), no flicker on unchanged
      refreshes, serial `[fetch]` log OK. Setup portal "Scan again" and aborted-upload recovery are
      untested on hardware (testing them needs forced WiFi failure / a cancelled upload).
- [ ] Before the first commit: `git add` the untracked files the build needs: `include/pinout.h`,
      `include/graphics_config.h`, `include/kawaii_frames.h`, `include/stations/stations.h`,
      `include/secrets.example.h`, `partitions_custom.csv`, `tools/`, `data/`, `CLAUDE.md`. Decide on
      `wl_ogv_resources/` (2.6 MB of downloadable source data). Never add `include/secrets.h`.
- [ ] After the user's flash: confirm the config page changes look right on the phone (dropdown
      placeholder, single "Choose file" button, bold title).

Deferred by the user (2026-10-02: "fix everything except 3")

- [ ] Auth: `ArduinoOTA.setPassword`, `gServer.authenticate()` on /firmware, /fsimage, /upload*,
      /save; consider a WPA2 password for the setup AP. Today anyone on the LAN can reflash the device.

Cleanup / nice to have

- [ ] `ignore.md` is an outdated AGENTS.md draft (mentions RBL_ID/LINE_FILTER) superseded by this
      file. Ask before deleting.
- [x] README.md is the **end-user guide**: web interface only, no VS Code/flashing (user's
      requirement). Keep it very short and non-technical; developer info belongs here.
- [ ] Optional: `-DDISABLE_ALL_LIBRARY_WARNINGS` silences TFT_eSPI's DMA/TOUCH_CS build warnings.

Done (2026-10-02)

- [x] WiFi credentials moved to the gitignored `include/secrets.h` (+ `secrets.example.h`); the index
      was re-staged so no staged file contains credentials (checked with `git grep --cached`).
- [x] TFT setup moved to `build_flags`; TFT_eSPI 2.5.43 and ArduinoJson 7.4.2 pinned exactly.
      The hand-edited lib copy is backed up in that session's scratchpad (no longer needed).
- [x] Setup portal scans once (plus a "Scan again" button) instead of on every request.
- [x] `handleFirmwareUpload` handles `UPLOAD_FILE_ABORTED` and skips writes after an error.
- [x] `uploadfsota`: no change needed. It's built into the platform; the review claim was wrong.
- [x] Removed the unused `include/stations/{line,station}_options.h` and empty `data/stations/`.
- [x] Transfer-marker stripping handles the prefix style (C++ and JS); stations.h comment updated.
- [x] ArduinoJson 7 `JsonDocument` everywhere (no deprecated types left).
- [x] Generator scripts: removed the dead umlaut code (output proven byte-identical).
- [x] `test/Screen.cpp` → `examples/screen_test/Screen.cpp` (git mv).
- [x] Display flicker: unchanged departures are no longer redrawn.
- [x] Brand fixed to "Canfora" on both web pages.
- [x] Config page cleanup per user (see "Config page" above); `/preview` + live check removed.

Backlog / ideas

- [x] Stop data update, ESP side (2026-10-02, user chose option 1 "hosted image"). See
      "Stop data update" above. Built and page-tested; **not yet tested on hardware**.
- [ ] Hardware test of the stop data update, after the user flashes this firmware: from
      the scratchpad `datatest/` (littlefs.bin + version.txt) run
      `python -m http.server 8000 --bind 172.20.10.4`, then
      `curl -X POST http://tramagotchi.local/data-update -d base=http://172.20.10.4:8000/`.
      Expect "Stop data updated (2026-10-02)" and the card label showing that version. The PC was
      172.20.10.4 and the device 172.20.10.2 (iPhone hotspot); Windows Firewall may ask once.
      Status 2026-10-02: the firmware is flashed; "No update source set yet." was confirmed on the
      device. The download test was blocked by Windows Firewall: WLAN "Can-fi" is a Private network
      and python.exe only has inbound Allow rules for Public. Waiting for the user to allow Python on
      Private networks (or test later against GitHub Pages). Don't change firewall rules yourself.
- [ ] Hosting (the user does this later): a GitHub repo with Pages, plus a monthly workflow on
      windows-latest: `pip install platformio`, `powershell tools/update_wl_data.ps1`, `pio run -t buildfs`,
      publish `.pio/build/esp32-c3-devkitm-1/littlefs.bin` + `data/version.txt` to the Pages root.
      Then set `DATA_UPDATE_URL` to `https://<user>.github.io/<repo>/` and flash once.
- [ ] Re-run the data update every few months (last: 2026-10-02).
- [ ] After 2026-10-31: confirm RBL 1470/49 departures are back.

## Session log

- **2026-10-02**: Refreshed WL OGD data (June → Oct 2026: lines −71E −41E +26E +51B +78A, WLB→LB;
  24 new and 77 changed stops; 70 stop files changed). Added `tools/update_wl_data.ps1`. Checked the
  device partition table, flashed LittleFS over USB (COM23), and confirmed via `/lines` and `/stops`
  that the device serves the new data. Device firmware was not reflashed. Did a full project review
  (findings are in the TODO list above) and created this file.
- **2026-10-02 (later)**: Fixed every review item except auth (see TODO "Done"). Verified the
  clean-library build with TFT build_flags against the old hand-edited build: TFT_eSPI.cpp.o
  instructions and data are identical (`USER_SETUP_ID=46` was needed for getSetup/verifySetupID).
  Build: RAM 14.1 %, flash 84.7 % (1,109,670 / 1,310,720 B). Flashing the firmware was blocked by
  the permission classifier, so the device still runs the old firmware with the new data.
- **2026-10-02 (UI pass)**: README rewritten as an end-user guide (web interface only). Config
  page: removed the live check (UI + `/preview` + helpers), the search field, the station-list hint,
  the intro text and the firmware hint; the stop list is now a dropdown with "Pick a station...";
  the file input became a plain button; the brand is "Canfora"; the title is bigger and bold.
  Build OK (flash 84.0 %). Page JS verified in jsdom (21/21). The user flashes via USB themselves.
- **2026-10-02 (stations)**: Default stations removed (new `STATION_PRESET 0`, empty); saving an
  empty list now clears the stored stations. If the user's device still shows the old 3 stops after
  flashing, they're saved in NVS: clear the list on the web page and press Save.
- **2026-10-02 (stop data update)**: Built the ESP side of the web-triggered stop data update (see
  above) plus the `data/version.txt` written by update_wl_data.ps1. Build OK (flash 84.9 %, no
  main.cpp warnings); page test now 24/24. Hardware test pending: the device runs the previous
  flash (checked via its page: no "Update stop data" card yet).
