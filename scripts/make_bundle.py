#!/usr/bin/env python3
"""Zips the builds in firmware/parts/ (save_parts.py) into one bundle for a
flasher page: firmware/<Project>_bundle_v<version>_<date>.zip.

  python scripts/make_bundle.py            every build in firmware/parts/
  python scripts/make_bundle.py -e A -e B  only these environments

The zip holds manifest.json and a folder per build:

  {"format": "esp-firmware-bundle", "formatVersion": 1, "project": "..",
   "version": "1.0", "date": "2026-10-15",
   "builds": [{"id": "Tami_Remote", "name": "..", "info": "..", "chip": "ESP32-C3",
               "files": [{"path": "Tami_Remote/bootloader.bin", "offset": 0,
                          "role": "bootloader", "size": 12345, "sha256": ".."},
                         .. "partitions", "boot_app0", "app" ..]}]}

The chip is read from the app image's header. A flasher writes every file for
a new board ("install"), or only the app and boot_app0 to update one that
already runs the project (the settings in nvs / LittleFS stay).

A build's LittleFS image (littlefs.bin, saved by save_parts.py after buildfs)
goes in as role "other" at its partition's offset, so a new board gets it.

bundle_extras.json in the project folder (optional) adds what the flasher asks
for, applied to every build:
  {"options": [..], "nvs": {"entries": [..]}, "data": [{"path", "from"}]}
"nvs" gets each build's offset and size from its partition table; "data" files
(e.g. a tree option's "source") go into the zip's root, listed with checksums.
See the flasher's README (Website_Matrix) for the option and entry fields.
"""
import argparse
import datetime
import hashlib
import json
import os
import struct
import sys
import zipfile

CHIPS = {0: "ESP32", 2: "ESP32-S2", 5: "ESP32-C3", 9: "ESP32-S3", 12: "ESP32-C2", 13: "ESP32-C6", 16: "ESP32-H2",
         18: "ESP32-P4", 20: "ESP32-C61", 23: "ESP32-C5"}


def role(name, app):
    if name == app:
        return "app"
    for r in ("bootloader", "partitions", "boot_app0"):
        if name.lower().startswith(r):
            return r
    return "other"


def partitions(folder):
    """{name: (type, subtype, offset, size)} from a build's partitions.bin."""
    out = {}
    with open(os.path.join(folder, "partitions.bin"), "rb") as f:
        table = f.read()
    for i in range(0, len(table) - 31, 32):
        e = table[i:i + 32]
        if e[:2] != b"\xaa\x50":
            break
        out[e[12:28].split(b"\0")[0].decode()] = (e[2], e[3], *struct.unpack("<II", e[4:12]))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-e", "--env", action="append", help="environment to include (default: all in firmware/parts)")
    ap.add_argument("--root", default=os.getcwd(), help="the project folder (default: here)")
    args = ap.parse_args()

    parts = os.path.join(args.root, "firmware", "parts")
    builds = []
    for name in sorted(os.listdir(parts)) if os.path.isdir(parts) else []:
        meta_path = os.path.join(parts, name, "parts.json")
        if os.path.isfile(meta_path) and (not args.env or name in args.env):
            with open(meta_path, encoding="utf-8") as f:
                builds.append((json.load(f), os.path.join(parts, name)))
    if not builds:
        sys.exit("Nothing in firmware/parts/: build the environments first (save_parts.py is an extra_script).")
    if args.env and len(builds) != len(set(args.env)):
        sys.exit("Not built: " + ", ".join(sorted(set(args.env) - {m["build"] for m, _ in builds})))
    builds.sort(key=lambda b: b[0].get("order", 0))

    extras = {}
    extras_path = os.path.join(args.root, "bundle_extras.json")
    if os.path.isfile(extras_path):
        with open(extras_path, encoding="utf-8") as f:
            extras = json.load(f)

    today = datetime.date.today().isoformat()
    project = builds[0][0]["project"]
    version = builds[0][0]["version"]
    manifest = {"format": "esp-firmware-bundle", "formatVersion": 1, "project": project,
                "version": version, "date": today, "builds": []}
    files = []  # (path in the zip, source)
    for meta, folder in builds:
        app = meta["app"]["file"]
        with open(os.path.join(folder, app), "rb") as f:
            header = f.read(24)
        if header[:1] != b"\xe9":
            sys.exit(meta["build"] + ": firmware.bin is not an ESP image")
        chip_id = header[12] | (header[13] << 8)
        entries = [(i["file"], i["offset"]) for i in meta["images"]] + [(app, meta["app"]["offset"])]
        table = partitions(folder)
        fs_image = os.path.join(folder, "littlefs.bin")
        if os.path.isfile(fs_image):
            fs = [p for p in table.values() if p[0] == 0x01 and p[1] in (0x82, 0x83)]  # data: spiffs, littlefs
            if not fs:
                sys.exit(meta["build"] + ": littlefs.bin, but no LittleFS partition in its table")
            data_dir = os.path.join(args.root, "data")
            newest = max((os.path.getmtime(os.path.join(d, n)) for d, _, ns in os.walk(data_dir) for n in ns), default=0)
            if newest > os.path.getmtime(fs_image):
                print("Warning: %s: data/ changed after its littlefs.bin (run buildfs again)" % meta["build"])
            entries.append(("littlefs.bin", fs[0][2]))
        listed = []
        for file, offset in entries:
            source = os.path.join(folder, file)
            with open(source, "rb") as f:
                data = f.read()
            path = meta["build"] + "/" + file
            files.append((path, source))
            listed.append({"path": path, "offset": offset, "role": role(file, app),
                           "size": len(data), "sha256": hashlib.sha256(data).hexdigest()})
        build = {"id": meta["build"], "name": meta["name"], "info": meta["info"],
                 "version": meta["version"], "chip": CHIPS.get(chip_id, "chip %d" % chip_id),
                 "chipId": chip_id, "files": listed}
        if extras.get("options"):
            build["options"] = extras["options"]
        if extras.get("nvs"):
            nvs = [p for p in table.values() if p[0] == 0x01 and p[1] == 0x02]
            if not nvs:
                sys.exit(meta["build"] + ": bundle_extras.json has nvs, but its table has no nvs partition")
            build["nvs"] = dict(extras["nvs"], offset=nvs[0][2], size=nvs[0][3])
        manifest["builds"].append(build)
    for d in extras.get("data", []):
        with open(os.path.join(args.root, d["from"]), "rb") as f:
            data = f.read()
        files.append((d["path"], os.path.join(args.root, d["from"])))
        manifest.setdefault("data", []).append({"path": d["path"], "size": len(data),
                                                "sha256": hashlib.sha256(data).hexdigest()})

    out = os.path.join(args.root, "firmware", "%s_bundle_v%s_%s.zip" % (project.replace(" ", "_"), version, today))
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        z.writestr("manifest.json", json.dumps(manifest, indent=1))
        for path, source in files:
            z.write(source, path)
    print("Wrote", os.path.relpath(out, args.root))
    for b in manifest["builds"]:
        print("  %-16s %-9s %s" % (b["id"], b["chip"], b["name"]))


main()
