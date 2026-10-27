#!/usr/bin/env python3
"""Writes firmware/stations_tree.json from data/: the Line -> Stop -> Direction
tree the flasher's picker shows (bundle_extras.json, option "stations").

Each node is [label, children] and each leaf [label, value]; the value is the
station row the firmware stores (RBL,Line,Towards,StationName), made exactly as
the config page's Add button makes it (src/main.cpp: render, buildDirections,
addPreviewLines, clean, stripDir), so a row picked in the flasher and one added
on the page are the same.

  python scripts/make_stations_tree.py
"""
import json
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HIDDEN = ("abstellplatz", "hastus", "umleitung")  # hiddenStopPatterns
ASCII = {"ä": "ae", "ö": "oe", "ü": "ue", "Ä": "Ae", "Ö": "Oe", "Ü": "Ue", "ß": "ss"}


def ascii_text(v):
    for k, r in ASCII.items():
        v = v.replace(k, r)
    return v


def clean(v):
    return re.sub(r"\s+", " ", ascii_text(v).replace(",", " ")).strip()


def strip_dir(v):
    return re.sub(r",+$", "", " ".join(w for w in v.split() if w not in ("U", "S")))


def read_rows(path):
    with open(path, encoding="utf-8") as f:
        return [line.strip() for line in f if line.strip()]


def stops_of(line_id, line_text):
    path = os.path.join(ROOT, "data", "stops", line_id + ".csv")
    if not os.path.isfile(path):
        return []
    stops = []  # (rbl, name, dirs) as loadStops parses them
    for row in read_rows(path):
        p = row.find("|")
        p2 = row.find("|", p + 1)
        p3 = row.find("|", p2 + 1)
        if p > 0 and p2 > p:
            stops.append((row[:p], row[p + 1:p2], row[p2 + 1:p3] if p3 > p2 else row[p2 + 1:]))
    names = []
    for _, name, _ in stops:
        if name not in names and not any(h in name.lower() for h in HIDDEN):
            names.append(name)
    out = []
    for name in names:
        dirs = []  # [towards, rbl, exact]
        for rbl, n, d in stops:
            if n != name:
                continue
            exact = "~" not in d
            for t in (x.strip() for x in d.split("~")):
                if not t:
                    continue
                e = next((x for x in dirs if x[0] == t), None)
                if e is None:
                    dirs.append([t, rbl, exact])
                elif exact and not e[2]:
                    e[1], e[2] = rbl, True
        leaves = [[t, ",".join((clean(rbl), clean(line_text), clean(strip_dir(t)), clean(name)))]
                  for t, rbl, _ in dirs if rbl.isdigit()]
        if leaves:
            out.append([name, leaves])
    return out


def main():
    tree = []
    for row in read_rows(os.path.join(ROOT, "data", "line_options.csv")):
        line_id, _, line_text = row.partition("|")
        stops = stops_of(line_id, line_text)
        if stops:
            tree.append([line_text, stops])
    out = os.path.join(ROOT, "firmware", "stations_tree.json")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        json.dump(tree, f, ensure_ascii=False, separators=(",", ":"))
    leaves = sum(len(s[1]) for l in tree for s in l[1])
    print("Wrote %s: %d lines, %d leaves, %d KB" % (os.path.relpath(out, ROOT), len(tree), leaves,
                                                   os.path.getsize(out) // 1024))


main()
