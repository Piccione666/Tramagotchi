# SOURCE_ID: a short hash of src/, include/ (not secrets.h) and platformio.ini.
# Every build made from the same source gets the same one, whatever the board.
# Line endings don't count, so a checkout with CRLF matches. From the Tami Remote
# project (scripts/source_id.py), with include/ added: Tramagotchi's settings live there.
import datetime
import hashlib
import json
import os

from SCons.Script import COMMAND_LINE_TARGETS

Import("env")
try:
    Import("projenv")
except Exception:  # an upload without a build ("nobuild") compiles nothing
    projenv = None

root = env.subst("$PROJECT_DIR")
files = [os.path.join(root, "platformio.ini")]
for top in (env.subst("$PROJECT_SRC_DIR"), env.subst("$PROJECT_INCLUDE_DIR")):
    for folder, _, names in os.walk(top):
        # idf_component.yml(.orig): written into src/ by the S3's platform
        # (pioarduino) during its build, so not source.
        files += [os.path.join(folder, name) for name in names
                  if name != "secrets.h" and not name.startswith("idf_component.yml")]

digest = hashlib.sha256()
for path in sorted(os.path.relpath(f, root).replace(os.sep, "/") for f in files):
    digest.update(path.encode() + b"\0")
    with open(os.path.join(root, path), "rb") as f:
        digest.update(f.read().replace(b"\r\n", b"\n") + b"\0")

source_id = digest.hexdigest()[:8]

# The version counts up with the source: custom_firmware_version (platformio.ini,
# "1.0") is where it starts, and every build from changed source adds one after
# the dot (1.1, 1.2, ...). Environments built from the same source share one
# version, so a bundle of them is one release. The count lives in
# firmware_version.json (keep it in git); save_firmware.py and save_parts.py read
# the version from there. A new custom_firmware_version (2.0) restarts it.
base = env.GetProjectOption("custom_firmware_version", "1.0")
state_path = os.path.join(root, "firmware_version.json")
try:
    with open(state_path, encoding="utf-8") as f:
        state = json.load(f)
except (OSError, ValueError):
    state = {}
major, _, start = base.partition(".")
# Only a build counts: an editor reading the project (PlatformIO's IntelliSense
# runs these scripts with the target "idedata" or "__idedata"), or a clean, makes
# no firmware and gets the version as it stands.
building = not set(COMMAND_LINE_TARGETS) & {"idedata", "__idedata", "clean", "cleanall", "compiledb", "envdump", "menuconfig"}
if state.get("base") != base:
    state = {"base": base, "minor": int(start or 0), "source": source_id}
elif state.get("source") != source_id and building:
    state["minor"] = int(state.get("minor", 0)) + 1
    state["source"] = source_id
state["version"] = "%s.%d" % (major, state["minor"])
if building:
    with open(state_path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(state, f, indent=1)
        f.write("\n")
version = state["version"]
# FIRMWARE_NAME: the name its file gets (save_firmware.py), e.g.
# Tramagotchi_C3_v1.0_2026-10-27.
name = "%s_v%s_%s" % (env.subst("$PIOENV"), version, datetime.date.today().isoformat())
print("Source ID:", source_id, name)
if projenv is not None:
    projenv.Append(CPPDEFINES=[("SOURCE_ID", env.StringifyMacro(source_id)),
                               ("FIRMWARE_NAME", env.StringifyMacro(name))])
