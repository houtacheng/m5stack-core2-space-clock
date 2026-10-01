#!/usr/bin/env python3
"""Create a copy of the Arduino ESP32 SDK that has room for Bluetooth Classic.

With PSRAM enabled the stock M5Stack SDK leaves no IRAM for the Bluetooth
controller (the link fails by ~9 KB). Some libc functions that are only ever
called from normal tasks (time formatting, stdio files) are placed in IRAM by
the stock linker script; this copy puts them back into flash. Nothing else
changes. Output: .arduino-build/sdk-esp32 (use it with
--build-property compiler.sdk.path=<that folder>).
"""
import re
import shutil
import subprocess
import sys
from pathlib import Path

OBJECTS = "strftime strptime mktime tzset_r lcltime_r gmtime_r tzcalc_limits fvwrite findfp fflush ungetc refill makebuf fclose stdio wsetup".split()


def find_sdk():
    base = Path.home() / "Library/Arduino15/packages/m5stack/tools/esp32-arduino-libs"
    if not base.exists():
        base = Path.home() / ".arduino15/packages/m5stack/tools/esp32-arduino-libs"
    candidates = sorted(base.glob("*/esp32"))
    if not candidates:
        sys.exit("M5Stack esp32-arduino-libs not found; install the m5stack:esp32 core first")
    return candidates[-1]


def main():
    root = Path(__file__).resolve().parents[1]
    target = root / ".arduino-build" / "sdk-esp32"
    sdk = find_sdk()
    stamp = target / ".source"
    if stamp.exists() and stamp.read_text() == str(sdk) and (target / "ld/sections.ld").exists():
        print(target)
        return
    if target.exists():
        shutil.rmtree(target)
    target.parent.mkdir(parents=True, exist_ok=True)
    # APFS clone where possible (instant), plain copy otherwise.
    if sys.platform == "darwin":
        subprocess.run(["cp", "-Rc", str(sdk), str(target)], check=True)
    else:
        shutil.copytree(sdk, target)
    ld = target / "ld/sections.ld"
    rules = {"*libc.a:libc_a-%s.*(.literal .literal.* .text .text.*)" % o for o in OBJECTS}
    out, removed = [], 0
    for line in ld.read_text().split("\n"):
        if line.strip() in rules:
            removed += 1
            continue
        if "EXCLUDE_FILE(" in line:
            for o in OBJECTS:
                line = line.replace(" *libc.a:libc_a-%s.*" % o, "")
        out.append(line)
    if removed != len(OBJECTS):
        sys.exit("unexpected linker script layout (%d of %d rules found)" % (removed, len(OBJECTS)))
    ld.write_text("\n".join(out))
    stamp.write_text(str(sdk))
    print(target)


if __name__ == "__main__":
    main()
