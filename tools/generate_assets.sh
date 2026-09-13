#!/bin/sh
set -eu

SOURCE_DIR="${1:-SpaceClockNative/art}"
OUTPUT="${2:-SpaceClockNative/assets.h}"
TMP="$(mktemp)"

{
  echo '#pragma once'
  echo '#include <Arduino.h>'
  echo 'struct Asset { const uint8_t* data; size_t size; };'
  for file in "$SOURCE_DIR"/background.png "$SOURCE_DIR"/cosmonaut_0.png "$SOURCE_DIR"/cosmonaut_1.png "$SOURCE_DIR"/satellite_0.png "$SOURCE_DIR"/satellite_1.png "$SOURCE_DIR"/nav_companion.png "$SOURCE_DIR"/nav_meditation.png; do
    name="$(basename "$file" .png)_png"
    xxd -i -n "$name" "$file" | \
      sed 's/^unsigned char /const unsigned char /; s/\[\] =/[] PROGMEM =/; s/^unsigned int /const unsigned int /'
  done
  echo 'static const Asset satelliteAssets[] = {'
  echo '  {satellite_0_png, satellite_0_png_len},'
  echo '  {satellite_1_png, satellite_1_png_len},'
  echo '};'
} > "$TMP"

mv "$TMP" "$OUTPUT"
