Import("env")


env.AddCustomTarget(
    "uploadota",
    "$BUILD_DIR/${PROGNAME}.bin",
    [env.VerboseAction("$UPLOADCMD", "Uploading $SOURCE")],
    title="Upload Firmware OTA",
    description="Upload firmware over the network using DEVICE_LOCAL_HOSTNAME",
)
