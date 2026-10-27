# After a build: everything a blank board needs goes to .pio/parts/<build>/
# (bootloader.bin, partitions.bin, boot_app0.bin, firmware.bin and parts.json
# with each file's flash offset), for make_bundle.py to zip. The offsets come
# from the platform itself (FLASH_EXTRA_IMAGES, ESP32_APP_OFFSET), so any
# ESP32 chip and partition table works. Optional, per environment in
# platformio.ini:
#   custom_bundle_project  the project's name in the flasher (default: its folder's)
#   custom_bundle_name     this build's name in the flasher's list (default: the environment's)
#   custom_bundle_info     a line under it: which boards it is for
#   custom_firmware_version  where the version starts; firmware_version.json (if there is one) has the counted one
# After `pio run -t buildfs`, the LittleFS image goes there too (littlefs.bin);
# make_bundle.py gives it its partition's offset, and a flasher writes it to a
# new board only. A firmware build keeps it.
import json
import os
import shutil

Import("env")


def firmware_version(root, env):
    # The counted version (firmware_version.json, see source_id.py), else the option.
    try:
        with open(os.path.join(root, "firmware_version.json"), encoding="utf-8") as f:
            return json.load(f)["version"]
    except (OSError, ValueError, KeyError):
        return env.GetProjectOption("custom_firmware_version", "0")


def save(target, source, env):
    root = env.subst("$PROJECT_DIR")
    build = env.subst("$PIOENV")
    folder = os.path.join(root, ".pio", "parts", build)
    os.makedirs(folder, exist_ok=True)
    for old in os.listdir(folder):  # the last build's parts, but not the data image
        if old != FS_IMAGE:
            os.remove(os.path.join(folder, old))

    images = []
    for offset, path in env.get("FLASH_EXTRA_IMAGES", []):
        path = env.subst(path)
        name = os.path.basename(path)
        shutil.copyfile(path, os.path.join(folder, name))
        images.append({"file": name, "offset": int(str(offset), 0)})
    shutil.copyfile(str(target[0]), os.path.join(folder, "firmware.bin"))

    option = env.GetProjectOption
    meta = {
        "build": build,
        "order": list(env.GetProjectConfig().envs()).index(build),
        "project": option("custom_bundle_project", os.path.basename(root)),
        "name": option("custom_bundle_name", build),
        "info": option("custom_bundle_info", ""),
        "version": firmware_version(root, env),
        "images": images,
        "app": {"file": "firmware.bin", "offset": int(env.subst("$ESP32_APP_OFFSET") or "0x10000", 0)},
    }
    with open(os.path.join(folder, "parts.json"), "w", encoding="utf-8") as f:
        json.dump(meta, f, indent=1)
    print("Saved", os.path.relpath(folder, root))


def save_fs(target, source, env):
    folder = os.path.join(env.subst("$PROJECT_DIR"), ".pio", "parts", env.subst("$PIOENV"))
    os.makedirs(folder, exist_ok=True)
    shutil.copyfile(str(target[0]), os.path.join(folder, FS_IMAGE))
    print("Saved", os.path.join(".pio", "parts", env.subst("$PIOENV"), FS_IMAGE))


FS_IMAGE = "littlefs.bin"
env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", save)
env.AddPostAction("$BUILD_DIR/${ESP32_FS_IMAGE_NAME}.bin", save_fs)
