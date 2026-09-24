#!/usr/bin/env python3
"""Check the flags and generated OTA image before publishing a public build."""
import argparse
import hashlib
import json
import re
from pathlib import Path
from validate_ui_fonts import read_font


def validate_build(build, sketch):
    options = json.loads((build / "build.options.json").read_text())
    if not options["fqbn"].startswith("m5stack:esp32:m5stack_core2"):
        raise ValueError("Use the M5Stack Core2 board package for releases")
    commands = json.loads((build / "compile_commands.json").read_text())
    cpp = [c for c in commands if c["file"].endswith(".ino.cpp")]
    if not cpp:
        raise ValueError("Sketch compile command not found")
    flags = cpp[0]["arguments"]
    for flag in ("-DBOARD_HAS_PSRAM", "-DSPACE_CLOCK_PUBLIC_BUILD"):
        if flag not in flags:
            raise ValueError(f"Missing required release flag: {flag}")
    # A C++-only PSRAM flag would leave esp32-hal-psram.c uninitialized.
    core = [c for c in commands if c["file"].endswith("esp32-hal-psram.c")]
    if not core or any("-DBOARD_HAS_PSRAM" not in c["arguments"] for c in core):
        raise ValueError("PSRAM initialization must also be enabled in the C core")
    image = build / "SpaceClockNative.ino.bin"
    data = image.read_bytes()
    if not data or data[0] != 0xE9:
        raise ValueError("Not an ESP32 application image")
    if len(data) > 0x640000:
        raise ValueError("Image exceeds the existing OTA partition")
    version = re.search(r'SPACE_CLOCK_VERSION "([^"]+)"', (sketch / "config.h").read_text())[1]
    if version.encode() not in data:
        raise ValueError("Built image does not contain the current version")
    for size in (8, 14, "28_ascii"):
        header = sketch / "generated" / f"source_han_sans_{size}.h"
        bitmap, _ = read_font(header)
        if bitmap not in data:
            raise ValueError(f"OTA image contains stale or missing bitmaps: {header.name}")
    # Never print local credentials, even when this check fails.
    secrets = sketch / "wifi_secrets.h"
    if secrets.exists():
        for value in re.findall(r'^\s*#define\s+DEFAULT_WIFI_\w+\s+"([^"\n]+)"', secrets.read_text(), re.M):
            if value.encode() + b"\0" in data:
                raise ValueError("Private Wi-Fi configuration detected in public image")
    return {"version": version, "size": len(data), "sha256": hashlib.sha256(data).hexdigest()}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    print(json.dumps(validate_build(args.build, root / "SpaceClockNative"), indent=2))
