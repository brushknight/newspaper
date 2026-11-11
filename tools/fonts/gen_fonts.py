#!/usr/bin/env python3
"""Generate core/fonts/np_fonts.c from TrueType fonts.

The generated file is committed, so a normal build does not need this script
or Pillow. Run it only to change sizes or typefaces:

    pip install pillow
    python3 tools/fonts/gen_fonts.py <dir with the .ttf files> core/fonts/np_fonts.c

Expected files in the font directory (all SIL Open Font License 1.1):
    UnifrakturCook-Bold.ttf, IBMPlexSerif-Medium.ttf, IBMPlexSerif-Bold.ttf,
    IBMPlexMono-Bold.ttf
"""
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ASCII = "".join(chr(c) for c in range(32, 127))

# name, file, pixel size, characters
FONTS = [
    ("np_font_nameplate", "UnifrakturCook-Bold.ttf", 118, " TheMorningNwspa"),
    ("np_font_text", "IBMPlexSerif-Medium.ttf", 40, ASCII),
    ("np_font_heading", "IBMPlexSerif-Bold.ttf", 68, ASCII),
    ("np_font_code", "IBMPlexMono-Bold.ttf", 170, "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-"),
]


def render_font(path, size, chars):
    font = ImageFont.truetype(str(path), size)
    ascent, descent = font.getmetrics()
    line_height = ascent + descent
    glyphs = []
    bitmap = bytearray()
    for ch in sorted(set(chars)):
        advance = int(round(font.getlength(ch)))
        pad = size
        img = Image.new("L", (advance + 2 * pad, line_height + 2 * pad), 0)
        ImageDraw.Draw(img).text((pad, pad), ch, font=font, fill=255)
        img = img.point(lambda v: 255 if v >= 128 else 0)
        box = img.getbbox()
        if box is None:
            glyphs.append((ord(ch), 0, 0, 0, 0, advance, len(bitmap)))
            continue
        x0, y0, x1, y1 = box
        w, h = x1 - x0, y1 - y0
        offset = len(bitmap)
        px = img.load()
        for y in range(y0, y1):
            byte = 0
            for i, x in enumerate(range(x0, x1)):
                if px[x, y]:
                    byte |= 0x80 >> (i % 8)
                if i % 8 == 7:
                    bitmap.append(byte)
                    byte = 0
            if w % 8:
                bitmap.append(byte)
        glyphs.append((ord(ch), w, h, x0 - pad, y0 - pad, advance, offset))
    return line_height, ascent, glyphs, bitmap


def main():
    font_dir = Path(sys.argv[1])
    out = Path(sys.argv[2])
    lines = [
        "/*",
        " * GENERATED FILE, do not edit. Made by tools/fonts/gen_fonts.py.",
        " *",
        " * Bitmap renderings of fonts under the SIL Open Font License 1.1:",
        " *   np_font_nameplate: UnifrakturCook Bold, (c) 2010 j. 'mach' wust, (c) 2009 Peter Wiegel",
        " *   np_font_text, np_font_heading: IBM Plex Serif, (c) 2017 IBM Corp.",
        " *   np_font_code: IBM Plex Mono Bold, (c) 2017 IBM Corp.",
        " * The licence texts are in tools/fonts/. These bitmaps stay under the OFL.",
        " */",
        '#include "font.h"',
        "",
    ]
    for name, file, size, chars in FONTS:
        line_height, ascent, glyphs, bitmap = render_font(font_dir / file, size, chars)
        lines.append(f"static const uint8_t {name}_bitmap[] = {{")
        for i in range(0, len(bitmap), 20):
            lines.append("    " + ",".join(str(b) for b in bitmap[i:i + 20]) + ",")
        lines.append("};")
        lines.append(f"static const np_glyph_t {name}_glyphs[] = {{")
        for g in glyphs:
            lines.append("    {%d, %d, %d, %d, %d, %d, %d}," % g)
        lines.append("};")
        lines.append(
            f"const np_font_t {name} = {{ {name}_glyphs, {len(glyphs)}, {name}_bitmap, {line_height}, {ascent} }};"
        )
        lines.append("")
        print(f"{name}: {len(glyphs)} glyphs, {len(bitmap)} bytes, line height {line_height}")
    out.write_text("\n".join(lines))


if __name__ == "__main__":
    main()
