import os
import re

from SCons.Script import COMMAND_LINE_TARGETS

Import("env")


OTA_TARGETS = {"uploadota", "uploadfsota"}


def read_device_hostname(config_path):
    with open(config_path, "r", encoding="utf-8", errors="ignore") as config_file:
        content = config_file.read()

    match = re.search(
        r'^\s*#define\s+DEVICE_LOCAL_HOSTNAME\s+"([^"]+)"',
        content,
        re.MULTILINE,
    )
    if not match:
        raise ValueError(
            "DEVICE_LOCAL_HOSTNAME not found in include/config.h; "
            "cannot derive OTA upload target."
        )
    return match.group(1)


if OTA_TARGETS & set(COMMAND_LINE_TARGETS):
    project_dir = env.subst("$PROJECT_DIR")
    config_path = os.path.join(project_dir, "include", "config.h")
    hostname = read_device_hostname(config_path)

    env.Replace(
        UPLOAD_PROTOCOL="espota",
        UPLOAD_PORT=f"{hostname}.local",
    )
    print(f"Configured OTA target from DEVICE_LOCAL_HOSTNAME: {hostname}.local")
