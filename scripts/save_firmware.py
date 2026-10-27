# After a build: its firmware goes to firmware/<build>_v<version>_<date>.bin,
# e.g. Tramagotchi_C3_v1.0_2026-10-27.bin (Tramagotchi_S3 for the S3): the file
# to pick for "Firmware OTA upload" on the device's page. The version is
# firmware_version.json (counted up by source_id.py from custom_firmware_version). An older file of the same build
# goes, so the folder holds the newest of each.
import datetime
import json
import os
import shutil

Import("env")


def firmware_version(root, env):
    # The counted version (source_id.py), else the one in platformio.ini.
    try:
        with open(os.path.join(root, "firmware_version.json"), encoding="utf-8") as f:
            return json.load(f)["version"]
    except (OSError, ValueError, KeyError):
        return env.GetProjectOption("custom_firmware_version", "0")


def save(target, source, env):
    root = env.subst("$PROJECT_DIR")
    folder = os.path.join(root, "firmware")
    os.makedirs(folder, exist_ok=True)
    build = env.subst("$PIOENV")
    version = firmware_version(root, env)
    name = "%s_v%s_%s.bin" % (build, version, datetime.date.today().isoformat())
    for old in os.listdir(folder):  # this build's earlier files, also under the old name
        if old != name and (old == build + ".bin" or old.startswith(build + "_v")):
            os.remove(os.path.join(folder, old))
    saved = os.path.join(folder, name)
    shutil.copyfile(str(target[0]), saved)
    print("Saved", os.path.relpath(saved, root))


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", save)
