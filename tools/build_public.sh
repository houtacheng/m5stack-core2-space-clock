#!/usr/bin/env bash
# Never override build.extra_flags: it contains the board's PSRAM defines.
set -euo pipefail
project_dir="$(cd "$(dirname "$0")/.." && pwd)"
cd "$project_dir"
arduino_cli="${SPACE_CLOCK_ARDUINO_CLI:-arduino-cli}"
python_cli="${SPACE_CLOCK_PYTHON:-python3}"
"$python_cli" tools/validate_ui_fonts.py
version="$(sed -n 's/^#define SPACE_CLOCK_VERSION "\([^"]*\)"/\1/p' SpaceClockNative/config.h)"
"$arduino_cli" compile --fqbn m5stack:esp32:m5stack_core2:PSRAM=enabled,PartitionScheme=default \
  --build-property compiler.cpp.extra_flags=-DSPACE_CLOCK_PUBLIC_BUILD \
  --build-path "$project_dir/.arduino-build/release-$version" "$@" SpaceClockNative
"$python_cli" tools/validate_public_build.py "$project_dir/.arduino-build/release-$version"
