#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 Silicon Laboratories Inc.
# SPDX-License-Identifier: Apache-2.0
"""Convert a monospace TrueType font into an LVGL font for a character grid.

Renders printable ASCII with FreeType (through Pillow) and writes an LVGL
"fmt_txt" font: plain 4 bpp bitmaps, one cmap range and no kerning, the
format lv_font_conv writes with --no-compress. Every glyph advances by the
same cell width, so text lines up on a grid even when the font's own advance
is slightly wider. The font's license text goes into the file header, as its
license may require.

The terminal font of the phone demo was made with:

  gen_font.py --size 14 --cell 8x18 --name lv_font_adwaita_mono_14 \\
      /usr/share/fonts/adwaita-mono-fonts/AdwaitaMono-Regular.ttf \\
      /usr/share/licenses/adwaita-mono-fonts/LICENSE src/font_adwaita_mono_14.c

--preview writes a PNG of the converted glyphs placed as LVGL places them.
"""

import argparse
import os

from PIL import Image, ImageDraw, ImageFont

FIRST = 0x20
LAST = 0x7E

PREVIEW = [
    "n900:~$ kernel threads",
    "*0x80a3c0e8 shell_term",
    "        stack size 3072, unused 1216, usage 1856 / 3072 (60 %)",
    "!\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ",
    "[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~ 0O 1lI",
]


def render_glyph(font, ch, size):
    """Glyph ink as (box_w, box_h, ofs_x, ofs_y, 4 bpp pixel list)."""
    pad = size * 2
    img = Image.new("L", (pad * 3, pad * 3), 0)
    ImageDraw.Draw(img).text((pad, pad * 2), ch, font=font, fill=255, anchor="ls")
    bbox = img.getbbox()
    if bbox is None:
        return 0, 0, 0, 0, []

    left, top, right, bottom = bbox
    pixels = [(v * 15 + 127) // 255 for v in img.crop(bbox).tobytes()]

    # LVGL places the bitmap's bottom edge ofs_y above the baseline
    return right - left, bottom - top, left - pad, pad * 2 - bottom, pixels


def pack4(pixels):
    """Pack 4 bpp pixels continuously, high nibble first, as LVGL reads them."""
    if len(pixels) % 2:
        pixels = pixels + [0]
    return [(pixels[i] << 4) | pixels[i + 1] for i in range(0, len(pixels), 2)]


def license_comment(path):
    with open(path, encoding="utf-8") as f:
        lines = f.read().rstrip().split("\n")
    return "\n".join((" * " + line).rstrip() for line in lines)


def write_c(args, glyphs, metrics):
    copyright_lines = []
    with open(args.license, encoding="utf-8") as f:
        for line in f:
            if line.startswith("Copyright"):
                copyright_lines.append(" * " + line.rstrip())

    out = []
    out.append("/*")
    out.extend(copyright_lines)
    out.append(" * SPDX-FileCopyrightText: Copyright (c) 2026 Silicon Laboratories Inc.")
    out.append(" * SPDX-License-Identifier: OFL-1.1")
    out.append(" *")
    out.append(f" * {os.path.basename(args.ttf)} at {args.size} px in {args.cell} cells, "
               f"4 bpp, 0x{FIRST:02X}-0x{LAST:02X},")
    out.append(" * converted by apps/phone/gen_font.py. The font's license follows.")
    out.append(" *")
    out.append(license_comment(args.license))
    out.append(" */")
    out.append("")
    out.append("#include <lvgl.h>")
    out.append("")
    out.append("static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {")

    dscs = ["\t/* id = 0 reserved */",
            "\t{.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0},"]
    index = 0
    for code, (box_w, box_h, ofs_x, ofs_y, pixels) in glyphs:
        data = pack4(pixels)
        ch = chr(code)
        shown = ch if ch not in "\\*/" else f"0x{code:02X}"
        out.append(f"\t/* U+{code:04X} \"{shown}\" */")
        for i in range(0, len(data), 12):
            out.append("\t" + ", ".join(f"0x{b:02x}" for b in data[i:i + 12]) + ",")
        dscs.append(f"\t{{.bitmap_index = {index}, .adv_w = {metrics['cell_w'] * 16}, "
                    f".box_w = {box_w}, .box_h = {box_h}, .ofs_x = {ofs_x}, "
                    f".ofs_y = {ofs_y}}},")
        index += len(data)

    out.append("};")
    out.append("")
    out.append("static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {")
    out.extend(dscs)
    out.append("};")
    out.append("")
    out.append("static const lv_font_fmt_txt_cmap_t cmaps[] = {")
    out.append(f"\t{{.range_start = {FIRST}, .range_length = {LAST - FIRST + 1}, "
               ".glyph_id_start = 1,")
    out.append("\t .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0,")
    out.append("\t .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY},")
    out.append("};")
    out.append("")
    out.append("static const lv_font_fmt_txt_dsc_t font_dsc = {")
    out.append("\t.glyph_bitmap = glyph_bitmap,")
    out.append("\t.glyph_dsc = glyph_dsc,")
    out.append("\t.cmaps = cmaps,")
    out.append("\t.kern_dsc = NULL,")
    out.append("\t.kern_scale = 0,")
    out.append("\t.cmap_num = 1,")
    out.append("\t.bpp = 4,")
    out.append("\t.kern_classes = 0,")
    out.append("\t.bitmap_format = LV_FONT_FMT_TXT_PLAIN,")
    out.append("};")
    out.append("")
    out.append(f"const lv_font_t {args.name} = {{")
    out.append("\t.get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,")
    out.append("\t.get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,")
    out.append(f"\t.line_height = {metrics['line_h']},")
    out.append(f"\t.base_line = {metrics['base_line']},")
    out.append(f"\t.cap_height = {metrics['cap']},")
    out.append(f"\t.x_height = {metrics['x']},")
    out.append("\t.subpx = LV_FONT_SUBPX_NONE,")
    out.append("\t.underline_position = -1,")
    out.append("\t.underline_thickness = 1,")
    out.append("\t.static_bitmap = 1,")
    out.append("\t.dsc = &font_dsc,")
    out.append("\t.fallback = NULL,")
    out.append("\t.user_data = NULL,")
    out.append("};")
    out.append("")

    with open(args.out, "w", encoding="utf-8") as f:
        f.write("\n".join(out))


def write_preview(path, glyphs, metrics):
    """Place the 4 bpp glyphs on the cell grid as LVGL does."""
    cell_w, line_h, base = metrics["cell_w"], metrics["line_h"], metrics["base_line"]
    table = dict(glyphs)
    cols = max(len(line) for line in PREVIEW)
    img = Image.new("L", (cols * cell_w + 8, len(PREVIEW) * line_h + 8), 0x14)
    for row, line in enumerate(PREVIEW):
        baseline = 4 + (row + 1) * line_h - base
        for col, ch in enumerate(line):
            box_w, box_h, ofs_x, ofs_y, pixels = table[ord(ch)]
            x0 = 4 + col * cell_w + ofs_x
            y0 = baseline - ofs_y - box_h
            for i, v in enumerate(pixels):
                if v:
                    x, y = x0 + i % box_w, y0 + i // box_w
                    img.putpixel((x, y), max(img.getpixel((x, y)), v * 17))
    img.save(path)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--size", type=int, required=True, help="font size in pixels")
    parser.add_argument("--cell", required=True, help="cell size as WIDTHxHEIGHT")
    parser.add_argument("--name", required=True, help="name of the lv_font_t")
    parser.add_argument("--preview", help="also write a PNG preview")
    parser.add_argument("ttf")
    parser.add_argument("license", help="license text of the font")
    parser.add_argument("out", help="C file to write")
    args = parser.parse_args()

    cell_w, line_h = (int(v) for v in args.cell.split("x"))
    font = ImageFont.truetype(args.ttf, args.size)
    glyphs = [(c, render_glyph(font, chr(c), args.size)) for c in range(FIRST, LAST + 1)]

    top = max(g[3] + g[1] for _, g in glyphs if g[1])
    bottom = -min(g[3] for _, g in glyphs if g[1])
    if top + bottom > line_h:
        parser.error(f"glyphs span {top + bottom} px, more than the {line_h} px line")

    by_char = dict(glyphs)
    metrics = {
        "cell_w": cell_w,
        "line_h": line_h,
        # Spare rows go above the tallest glyph
        "base_line": bottom,
        "cap": by_char[ord("H")][1],
        "x": by_char[ord("x")][1],
    }

    write_c(args, glyphs, metrics)
    if args.preview:
        write_preview(args.preview, glyphs, metrics)


main()
