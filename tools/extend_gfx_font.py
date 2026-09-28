#!/usr/bin/env python3
"""Add common Simplified/Traditional Chinese glyphs to a compact GFX header.

Dynamic text (Home Assistant transcripts and replies) can contain any common
Chinese character, not only the ones used by the firmware UI. Existing glyphs
are kept; missing ones from GB2312 level 1 and Big5 level 1 are rendered and
appended, then the header is re-emitted in the compact EncodeRange form.
"""

import argparse
import re
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

from compact_gfx_font import GLYPH, compact


def common_chinese():
    chars = set()
    for hi in range(0xB0, 0xD8):  # GB2312 level 1
        for lo in range(0xA1, 0xFF):
            try: chars.add(bytes([hi, lo]).decode("gb2312"))
            except UnicodeDecodeError: pass
    for hi in range(0xA4, 0xC7):  # Big5 frequently used hanzi
        for lo in list(range(0x40, 0x7F)) + list(range(0xA1, 0xFF)):
            try: chars.add(bytes([hi, lo]).decode("big5"))
            except UnicodeDecodeError: pass
    chars.update("，。、；：？！「」『』（）《》〈〉…—～·％＋－＝／")
    chars.update(chr(c) for c in range(0x30A1, 0x30F7))  # katakana for Matrix rain
    return {ord(c) for c in chars if 0x20 < ord(c) <= 0xFFFF}


def packed(image):
    px, out, value, bits = image.load(), bytearray(), 0, 0
    for y in range(image.height):
        for x in range(image.width):
            value = (value << 1) | (1 if px[x, y] >= 128 else 0); bits += 1
            if bits == 8: out.append(value); value = bits = 0
    if bits: out.append(value << (8 - bits))
    return out


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--header", type=Path, required=True)
    p.add_argument("--font", required=True)
    p.add_argument("--pixels", type=int, required=True)
    p.add_argument("--kana-only", action="store_true", help="only add katakana (keeps large fonts small)")
    a = p.parse_args()
    text = a.header.read_text(encoding="utf-8")
    bm = re.search(r"(const uint8_t (\w+)Bitmaps\[\] PROGMEM = \{)(.*?)(\n\};)", text, re.S)
    name = bm.group(2)
    bitmap = bytearray(int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]{2})", bm.group(3)))
    gstart = text.index(f"const GFXglyph {name}Glyphs[]")
    glyphs = {int(m.group(7), 16): tuple(int(m.group(i)) for i in range(1, 7)) for m in GLYPH.finditer(text[gstart:])}
    y_advance = int(re.search(r"0x[0-9A-Fa-f]+,\s*0x[0-9A-Fa-f]+,\s*(\d+)", text[text.index(f"const GFXfont {name}"):]).group(1))
    font = ImageFont.truetype(a.font, a.pixels)
    added = 0
    wanted = set(range(0x30A1, 0x30F7)) if a.kana_only else common_chinese()
    for code in sorted(wanted - set(glyphs)):
        ch = chr(code)
        l, t, r, b = font.getbbox(ch, anchor="ls")
        w, h = max(1, r - l), max(1, b - t)
        img = Image.new("L", (w, h), 0)
        ImageDraw.Draw(img).text((-l, -t), ch, font=font, fill=255, anchor="ls")
        data = packed(img)
        if not any(data): continue
        glyphs[code] = (len(bitmap), w, h, max(1, round(font.getlength(ch))), l, t)
        bitmap.extend(data); added += 1
    lines = [f"const uint8_t {name}Bitmaps[] PROGMEM = {{"]
    lines += ["  " + ", ".join(f"0x{x:02X}" for x in bitmap[i:i + 12]) + "," for i in range(0, len(bitmap), 12)]
    lines += ["};", "", f"const GFXglyph {name}Glyphs[] PROGMEM = {{"]
    lines += ["  { %5d, %3d, %3d, %3d, %4d, %4d }, // 0x%04X" % (*glyphs[c], c) for c in sorted(glyphs)]
    last = max(glyphs)
    lines += ["};", "", f"const GFXfont {name} PROGMEM = {{",
              f"  (uint8_t*){name}Bitmaps, (GFXglyph*){name}Glyphs,", f"  0x20, 0x{last:04X}, {y_advance} }};", ""]
    a.header.write_text(text[:bm.start()] + "\n".join(lines), encoding="utf-8")
    compact(a.header, 256 if a.kana_only else 64)
    print(f"{a.header.name}: added {added} glyphs, bitmap {len(bitmap)} bytes")


if __name__ == "__main__":
    main()
