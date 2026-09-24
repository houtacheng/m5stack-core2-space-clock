#!/usr/bin/env python3
"""Generate a compact, sparse Unicode GFXfont from the characters used by the firmware."""

import argparse
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont


def packed_bitmap(image):
    pixels = image.load()
    bits, out = 0, bytearray()
    value = 0
    for y in range(image.height):
        for x in range(image.width):
            value = (value << 1) | (1 if pixels[x, y] >= 128 else 0)
            bits += 1
            if bits == 8:
                out.append(value); value = 0; bits = 0
    if bits:
        out.append(value << (8 - bits))
    return out


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--font", required=True)
    p.add_argument("--source", required=True)
    p.add_argument("--output", required=True)
    p.add_argument("--name", required=True)
    p.add_argument("--pixels", type=int, required=True)
    p.add_argument("--ascii-only", action="store_true")
    a = p.parse_args()

    text = Path(a.source).read_text(encoding="utf-8")
    used = set(range(0x20, 0x7F))
    if not a.ascii_only:
        used.update(ord(ch) for ch in text if 0x20 <= ord(ch) <= 0xFFFF)
    first, last = 0x20, (0x7E if a.ascii_only else max(used))
    font = ImageFont.truetype(a.font, a.pixels)
    ascent, descent = font.getmetrics()
    y_advance = ascent + descent
    bitmap = bytearray()
    glyphs = []

    for code in range(first, last + 1):
        offset = len(bitmap)
        if code not in used or code == 0x20:
            glyphs.append((offset, 0, 0, max(1, round(font.getlength(chr(code)))), 0, 0))
            continue
        ch = chr(code)
        left, top, right, bottom = font.getbbox(ch, anchor="ls")
        width, height = max(1, right-left), max(1, bottom-top)
        image = Image.new("L", (width, height), 0)
        ImageDraw.Draw(image).text((-left, -top), ch, font=font, fill=255, anchor="ls")
        bitmap.extend(packed_bitmap(image))
        glyphs.append((offset, width, height, max(1, round(font.getlength(ch))), left, top))

    lines = [f"const uint8_t {a.name}Bitmaps[] PROGMEM = {{"]
    for i in range(0, len(bitmap), 12):
        lines.append("  " + ", ".join(f"0x{x:02X}" for x in bitmap[i:i+12]) + ",")
    lines.append("};\n")
    lines.append(f"const GFXglyph {a.name}Glyphs[] PROGMEM = {{")
    for code, g in zip(range(first, last + 1), glyphs):
        lines.append("  { %5d, %3d, %3d, %3d, %4d, %4d }, // 0x%04X" % (*g, code))
    lines += ["};\n", f"const GFXfont {a.name} PROGMEM = {{",
              f"  (uint8_t*){a.name}Bitmaps, (GFXglyph*){a.name}Glyphs,",
              f"  0x{first:02X}, 0x{last:04X}, {y_advance} }};", ""]
    Path(a.output).write_text("\n".join(lines), encoding="utf-8")
    print(f"{a.name}: {len(bitmap)} bitmap bytes, U+{first:04X}-U+{last:04X}")


if __name__ == "__main__":
    main()
