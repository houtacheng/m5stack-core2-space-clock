#!/usr/bin/env python3
"""Validate the actual embedded GFX bitmaps, not just glyph dimensions."""
import argparse
import re
from pathlib import Path


def read_font(path):
    text = Path(path).read_text(encoding="utf-8")
    bitmap = bytes(int(n, 16) for n in re.findall(r"0x([0-9A-Fa-f]{2})", text.split("};")[0]))
    records = re.findall(
        r"\{\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(-?\d+),\s*(-?\d+)\s*\},?(?:\s*\};)?\s*//\s*0x([0-9A-Fa-f]+)", text)
    glyphs = {int(r[6], 16): tuple(map(int, r[:6])) for r in records}
    if not glyphs:
        raise ValueError(f"No glyph table found: {path}")
    return bitmap, glyphs


def validate_font(path, required=()):
    bitmap, glyphs = read_font(path)
    errors = []
    visible = 0
    for cp in sorted(set(required) | {cp for cp, g in glyphs.items() if g[1] and g[2]}):
        if chr(cp).isspace():
            continue
        g = glyphs.get(cp)
        if not g or not g[1] or not g[2]:
            errors.append(f"U+{cp:04X}: missing glyph")
            continue
        offset, w, h = g[:3]
        length = (w * h + 7) // 8
        if offset + length > len(bitmap):
            errors.append(f"U+{cp:04X}: bitmap out of bounds")
        elif not any(bitmap[offset:offset + length]):
            errors.append(f"U+{cp:04X}: empty bitmap")
        else:
            visible += 1
    if errors:
        raise ValueError(f"{path}:\n" + "\n".join(errors))
    return visible


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--preview", type=Path)
    args = p.parse_args()
    root = Path(__file__).resolve().parents[1]
    sketch = root / "SpaceClockNative"
    text = "\n".join(p.read_text() for p in (sketch / "SpaceClockNative.ino", sketch / "mqtt_guide.h"))
    required = {ord(c) for c in text if 0x20 <= ord(c) <= 0x9FBF}
    required.update(range(33, 127))
    headers = [sketch / "generated" / f"source_han_sans_{size}.h" for size in (8, 14)]
    for header in headers:
        print(f"{header.name}: {validate_font(header, required)} visible glyphs checked")
    header = sketch / "generated/source_han_sans_28_ascii.h"
    print(f"{header.name}: {validate_font(header, range(33, 127))} visible glyphs checked")
    if args.preview:
        # Decode the shipped 1-bit arrays for inspection, not the source font.
        from PIL import Image, ImageDraw
        image = Image.new("RGB", (660, 260), "#101822")
        samples = ["情緒觀察 身體反應 開始 暫停 重新開始", "星期三 日期 設定 儲存 小夜燈", "Matrix 0123456789 ABC xyz !@#$%"]
        for index, header in enumerate(headers):
            bitmap, glyphs = read_font(header)
            for row, text in enumerate(samples):
                x, baseline = 12, 25 + index * 120 + row * 38
                for ch in text:
                    offset, w, h, advance, dx, dy = glyphs[ord(ch)]
                    draw = ImageDraw.Draw(image)
                    for bit in range(w * h):
                        if bitmap[offset + bit // 8] & (0x80 >> (bit % 8)):
                            draw.point((x + dx + bit % w, baseline + dy + bit // w), fill="white")
                    x += advance
        image.save(args.preview)


if __name__ == "__main__":
    main()
