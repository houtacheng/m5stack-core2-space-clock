"""Regression tests for baseline alignment and repairing populated blank glyphs."""
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from validate_ui_fonts import read_font, validate_font

TOOLS = Path(__file__).resolve().parent


class FontTests(unittest.TestCase):
    def test_released_fonts(self):
        subprocess.run([sys.executable, str(TOOLS / "validate_ui_fonts.py")], check=True)

    def test_generated_font_and_blank_repair(self):
        from PIL import ImageFont
        # Pillow includes a scalable font, so the test needs no licensed font.
        font_bytes = ImageFont.load_default(size=16).font_bytes
        with tempfile.TemporaryDirectory() as temp:
            folder = Path(temp)
            font = folder / "font.ttf"
            font.write_bytes(font_bytes)
            source = folder / "source.txt"
            source.write_text("ABC xyz 0123")
            header = folder / "font.h"
            subprocess.run([sys.executable, str(TOOLS / "generate_sparse_gfx_font.py"),
                            "--font", str(font), "--source", str(source), "--output", str(header),
                            "--name", "TestFont", "--pixels", "16", "--ascii-only"], check=True)
            validate_font(header, map(ord, "ABCxyz0123"))
            # A declared rectangle with no lit pixels reproduces the 2.9.6 bug.
            header.write_text("""const uint8_t TestFontBitmaps[] PROGMEM = { 0x00, 0x80 };
const GFXglyph TestFontGlyphs[] PROGMEM = {
  { 0, 2, 2, 16, 0, -2 }, // 0x0041
  { 1, 1, 1, 16, 0, -1 }, // 0x0042
};
""")
            patch = [sys.executable, str(TOOLS / "patch_sparse_gfx_font.py"),
                     "--font", str(font), "--header", str(header), "--pixels", "16", "--source", str(source)]
            with self.assertRaises(ValueError):
                validate_font(header, [65, 66])
            subprocess.run(patch, check=True)
            validate_font(header, [65, 66])
            bitmap, glyphs = read_font(header)
            self.assertEqual(glyphs[66], (1, 1, 1, 16, 0, -1))
            self.assertEqual(bitmap[1], 0x80)
            self.assertGreater(glyphs[65][1] * glyphs[65][2], 4)
            repaired = header.read_bytes()
            subprocess.run(patch, check=True)
            self.assertEqual(header.read_bytes(), repaired)


if __name__ == "__main__":
    unittest.main()
