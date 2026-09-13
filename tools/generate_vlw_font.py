#!/usr/bin/env python3
"""Build a compact anti-aliased VLW font array for LovyanGFX."""

import argparse
import struct
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


def be32(value: int) -> bytes:
    return struct.pack(">I", value & 0xFFFFFFFF)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--font", required=True)
    parser.add_argument("--source", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--size", type=int, default=24)
    args = parser.parse_args()

    source = Path(args.source).read_text(encoding="utf-8")
    chars = set(chr(code) for code in range(33, 127))
    chars.update(ch for ch in source if 0x20 < ord(ch) <= 0xFFFF)
    chars = sorted(chars, key=ord)

    font = ImageFont.truetype(args.font, args.size)
    ascent, descent = font.getmetrics()
    records = []
    bitmaps = []
    for ch in chars:
        left, top, right, bottom = font.getbbox(ch, anchor="ls")
        width = max(1, right - left)
        height = max(1, bottom - top)
        advance = max(1, round(font.getlength(ch)))
        image = Image.new("L", (width, height), 0)
        ImageDraw.Draw(image).text((-left, -top), ch, font=font, fill=255)
        records.append((ord(ch), height, width, advance, -top, left, 0))
        bitmaps.append(image.tobytes())

    data = bytearray()
    for value in (len(chars), 11, args.size, 0, ascent, descent):
        data += be32(value)
    for record in records:
        for value in record:
            data += be32(value)
    for bitmap in bitmaps:
        data += bitmap

    name = "SourceHanSansTC_Smooth24"
    lines = ["#pragma once", "#include <stdint.h>", "", f"static const uint8_t {name}[] PROGMEM = {{"]
    for offset in range(0, len(data), 16):
        lines.append("  " + ", ".join(f"0x{b:02X}" for b in data[offset:offset + 16]) + ",")
    lines += ["};", f"static constexpr uint32_t {name}_size = {len(data)};", ""]
    Path(args.output).write_text("\n".join(lines), encoding="utf-8")
    print(f"{len(chars)} glyphs, {len(data)} bytes")


if __name__ == "__main__":
    main()
