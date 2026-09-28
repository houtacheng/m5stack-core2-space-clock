#!/usr/bin/env python3
"""Drop unused glyph slots from a dense GFX font header.

The UI fonts span U+0020..U+FF1B but contain < 1k real glyphs. A dense
GFXglyph table for that span costs ~780 KB each and pushed the firmware's
DROM segment to ~3.9 MB, leaving no free flash-MMU pages for
esp_image_verify() during OTA. LovyanGFX's GFXfont supports EncodeRange
tables, so keep only real glyphs (plus all ASCII) and emit ranges.
"""

import re
import sys
from pathlib import Path

GLYPH = re.compile(r"^\s*\{\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(-?\d+),\s*(-?\d+)\s*\},?\s*//\s*0x([0-9A-Fa-f]+)", re.M)


def compact(path: Path, fill_gap: int = 64) -> None:
    text = path.read_text(encoding="utf-8")
    name = re.search(r"const uint8_t (\w+)Bitmaps\[\]", text).group(1)
    start = text.index(f"const GFXglyph {name}Glyphs[]")
    head = text[:start]
    glyphs = [(int(m.group(7), 16), tuple(int(m.group(i)) for i in range(1, 7))) for m in GLYPH.finditer(text[start:])]
    keep = [(c, g) for c, g in glyphs if c < 0x7F or g[1] or g[2]]
    # LovyanGFX scans EncodeRange linearly for every character drawn. Thousands
    # of tiny ranges made every text draw (Matrix rain, clock, Assist) slow, so
    # pad short gaps with empty glyphs and keep only a handful of ranges.
    y_adv_guess = max((g[3] for _, g in keep), default=8)
    filled = []
    for code, g in keep:
        if filled and 0 < code - filled[-1][0] - 1 < fill_gap:
            for gap in range(filled[-1][0] + 1, code):
                filled.append((gap, (0, 0, 0, max(1, y_adv_guess // 2), 0, 0)))
        filled.append((code, g))
    keep = filled
    ranges = []
    for index, (code, _) in enumerate(keep):
        if ranges and ranges[-1][1] + 1 == code:
            ranges[-1][1] = code
        else:
            ranges.append([code, code, index])
    y_advance = int(re.search(r"0x[0-9A-Fa-f]+,\s*0x[0-9A-Fa-f]+,\s*(\d+)\s*\}", text[start:]).group(1))
    lines = [f"const GFXglyph {name}Glyphs[] PROGMEM = {{"]
    lines += ["  { %5d, %3d, %3d, %3d, %4d, %4d }, // 0x%04X" % (*g, c) for c, g in keep]
    lines += ["};", "", f"const lgfx::EncodeRange {name}Ranges[] PROGMEM = {{"]
    lines += [f"  {{ 0x{a:04X}, 0x{b:04X}, {base} }}," for a, b, base in ranges]
    lines += ["};", "", f"const GFXfont {name} PROGMEM = {{",
              f"  (uint8_t*){name}Bitmaps, (GFXglyph*){name}Glyphs,",
              f"  0x{keep[0][0]:02X}, 0x{keep[-1][0]:04X}, {y_advance}, {len(ranges)}, (lgfx::EncodeRange*){name}Ranges }};", ""]
    path.write_text(head + "\n".join(lines), encoding="utf-8")
    print(f"{path.name}: {len(glyphs)} -> {len(keep)} glyphs, {len(ranges)} ranges")


if __name__ == "__main__":
    gap = 64
    for arg in sys.argv[1:]:
        if arg.startswith("--gap="):
            gap = int(arg.split("=", 1)[1])
            continue
        compact(Path(arg), gap)
