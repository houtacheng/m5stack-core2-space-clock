#!/usr/bin/env python3
"""Repair missing or empty glyphs in an existing sparse GFX font header.

Preserve valid glyphs and repair empty rasters, including the baseline-anchor
regression in 2.9.6. Also add characters found in the provided source files.
"""

import argparse
import re
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


def pack_bitmap(image):
    pixels = image.load()
    out = bytearray()
    value = bit_count = 0
    for y in range(image.height):
        for x in range(image.width):
            value = (value << 1) | (1 if pixels[x, y] >= 128 else 0)
            bit_count += 1
            if bit_count == 8:
                out.append(value)
                value = bit_count = 0
    if bit_count:
        out.append(value << (8 - bit_count))
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--header", required=True, type=Path)
    parser.add_argument("--font", required=True, type=Path)
    parser.add_argument("--pixels", required=True, type=int)
    parser.add_argument("--source", required=True, type=Path, nargs="+")
    args = parser.parse_args()

    text = "\n".join(path.read_text(encoding="utf-8") for path in args.source)
    needed = {ord(char) for char in text if 0x20 <= ord(char) <= 0x9FBF}
    header = args.header.read_text(encoding="utf-8")

    bitmap_match = re.search(
        r"(const uint8_t (\w+)Bitmaps\[\] PROGMEM = \{)(.*?)(\s*\};)",
        header,
        re.S,
    )
    if not bitmap_match:
        raise SystemExit(f"Could not find bitmap array in {args.header}")
    font_name = bitmap_match.group(2)
    glyph_marker = f"const GFXglyph {font_name}Glyphs[] PROGMEM = {{"
    glyph_start = header.find(glyph_marker)
    if glyph_start < 0:
        raise SystemExit(f"Could not find glyph array {font_name}Glyphs")
    glyph_end = header.find("};", glyph_start)
    if glyph_end < 0:
        raise SystemExit("Could not find end of glyph array")

    glyph_block = header[glyph_start:glyph_end]
    glyph_pattern = re.compile(
        r"^(\s*)\{\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*"
        r"(\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*\},(\s*//\s*0x([0-9A-Fa-f]+).*)$",
        re.M,
    )
    records = {
        int(match.group(9), 16): match
        for match in glyph_pattern.finditer(glyph_block)
    }
    if not records:
        raise SystemExit("Could not parse glyph records")

    bitmap_text = bitmap_match.group(3)
    bitmap = bytes(int(value, 16) for value in re.findall(r"0x[0-9A-Fa-f]{2}", bitmap_text))
    bitmap_bytes = len(bitmap)
    font = ImageFont.truetype(str(args.font), args.pixels)
    appended = bytearray()
    replacements = {}
    missing = []

    # Empty rasters can have valid dimensions. Checking only width/height leaves
    # these invisible glyphs unrepaired. Inspect all previously populated slots.
    populated = {cp for cp, m in records.items() if int(m.group(3)) and int(m.group(4))}
    for codepoint in sorted(needed | populated):
        match = records.get(codepoint)
        if not match:
            continue
        width, height = int(match.group(3)), int(match.group(4))
        offset = int(match.group(2))
        byte_count = (width * height + 7) // 8
        if width and height and any(bitmap[offset:offset + byte_count]):
            continue

        char = chr(codepoint)
        if char.isspace() and codepoint != 0x3000:
            continue
        if codepoint == 0x3000:
            advance = 16 if args.pixels <= 16 else 27
            replacements[codepoint] = (
                match.group(1)
                + "{ %5d, %3d, %3d, %3d, %4d, %4d },%s"
                % (int(match.group(2)), 0, 0, advance, 0, 0, match.group(8))
            )
            continue
        try:
            left, top, right, bottom = font.getbbox(char, anchor="ls")
        except (ValueError, OSError):
            missing.append(codepoint)
            continue
        glyph_width, glyph_height = right - left, bottom - top
        if glyph_width <= 0 or glyph_height <= 0:
            missing.append(codepoint)
            continue

        image = Image.new("L", (glyph_width, glyph_height), 0)
        ImageDraw.Draw(image).text((-left, -top), char, font=font, fill=255, anchor="ls")
        bits = pack_bitmap(image)
        if not any(bits):
            missing.append(codepoint)
            continue
        offset = bitmap_bytes + len(appended)
        appended.extend(bits)

        # Match the slightly generous CJK advance already used by the UI fonts.
        advance = max(round(font.getlength(char)), 16 if args.pixels <= 16 else 27)
        x_offset = 1 if args.pixels <= 16 else max(0, (advance - glyph_width) // 2)
        values = (offset, glyph_width, glyph_height, advance, x_offset, top)
        replacements[codepoint] = (
            match.group(1)
            + "{ %5d, %3d, %3d, %3d, %4d, %4d },%s"
            % (*values, match.group(8))
        )

    for codepoint, match in records.items():
        replacement = replacements.get(codepoint)
        if replacement:
            glyph_block = glyph_block.replace(match.group(0), replacement, 1)

    if appended:
        additions = []
        values = list(appended)
        for start in range(0, len(values), 12):
            additions.append("  " + ", ".join(f"0x{byte:02X}" for byte in values[start:start + 12]) + ",")
        bitmap_contents = bitmap_text.rstrip()
        if not bitmap_contents.endswith(","):
            bitmap_contents += ","
        bitmap_replacement = bitmap_match.group(1) + bitmap_contents + "\n" + "\n".join(additions) + "\n};"
        header = header[:bitmap_match.start()] + bitmap_replacement + header[bitmap_match.end():]
    if replacements:
        glyph_start = header.find(glyph_marker)
        glyph_end = header.find("};", glyph_start)
        header = header[:glyph_start] + glyph_block + header[glyph_end:]
        args.header.write_text(header, encoding="utf-8")

    unresolved = ", ".join(f"U+{codepoint:04X}" for codepoint in missing)
    print(f"{args.header}: repaired/added {len(replacements)} glyphs, {len(appended)} bitmap bytes")
    if unresolved:
        raise SystemExit(f"Unsupported or blank glyphs: {unresolved}")


if __name__ == "__main__":
    main()
