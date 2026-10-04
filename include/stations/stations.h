#pragma once

// ── Departure blacklist ───────────────────────────────────────────────────────
// Patterns matched case-insensitively as SUBSTRINGS of "<line> <towards>".
// Any departure containing one of these is skipped — keep patterns specific,
// short patterns like " U" match almost every Vienna direction text
// ("Parlament, U Volkstheater", "Hietzing U", ...) and silently hide whole lines.
// Transfer markers (standalone "U" / "S" words) are already stripped for
// display, so they never need to be blacklisted. Example: B("Zug faellt aus")
#define DEPARTURE_BLACKLIST

// ── Station list ──────────────────────────────────────────────────────────────
// Select a preset:
//   0 = none: the device starts empty and stops are added on the web page
//   1 = TDG11
//   2 = RL39
//   3 = OWA
//   4 = PHILO
#define STATION_PRESET 0

// Format: X(RBL, stationName, lineFilter, towardsFilter)
//   RBL           Vienna transit stop ID (find via the web config UI)
//   stationName   optional label/reference name; leave empty if unused
//   lineFilter    leave empty to accept any line; non-empty filters + labels
//   towardsFilter leave empty to accept any direction
// Add more X(…) rows to enable station cycling.

#if STATION_PRESET == 0
  #define STATION_LIST
#elif STATION_PRESET == 1
  #define STATION_LIST \
    X("1470", "", "49", "Volkstheater") \
    X("4933", "", "U3", "Simmering") \
    X("157",  "", "51A", "Hietzing")
#elif STATION_PRESET == 2
  #define STATION_LIST \
    X("352",  "", "", "") \
    X("4433", "", "", "")
#elif STATION_PRESET == 3
  #define STATION_LIST \
    X("1379", "", "", "") \
    X("7472", "", "", "") \
    X("1392", "", "", "")
#elif STATION_PRESET == 4
  #define STATION_LIST \
    X("1421", "", "", "") \
    X("2263", "", "", "") \
    X("1420", "", "", "") \
    X("2262", "", "", "")
#else
  #error Invalid STATION_PRESET. Use a value from 0 to 4.
#endif
