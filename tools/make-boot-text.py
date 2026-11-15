#!/usr/bin/env python3
"""Render the boot splash's "Loading" text, one PNG per language.

The boot payload (psc-platform boot/, lumen-splash) shows the splash before Lumen runs,
on the console's own SDL without a font library, so its text comes pre-rendered:

  assets/boot/loading-<code>.png        white on transparent, UI font at 720p (21 px)
  assets/boot/loading-<code>-1080.png   the same at 1080p (32 px)

The text is the `boot.loading` string of every dist/lang/<code>.lang, in the font and
size Lumen's own splash uses for it (src/main.c splash_loading_draw: g_font_sub), on a
canvas as tall as the font's line box, so both splashes put it on the same pixels. The
payload reads the language from <stick>/lumen/settings.ini and falls back to "en".
Run it again after changing a translation of that key or adding a language.

Usage: python3 tools/make-boot-text.py   (from the repo root). Requires Pillow
(e.g. docker run --rm -v "$PWD:/w" -w /w psc-sim:latest python3 tools/make-boot-text.py).
"""
import glob
import math
import os
import re

from PIL import Image, ImageDraw, ImageFont

KEY = 'boot.loading'
FONT = 'assets/fonts/regular.ttf'
OUT = 'assets/boot'
SIZES = (('', 21), ('-1080', int(21 * 1.5 + 0.5)))   # src/main.c open_font: size * ui scale

os.makedirs(OUT, exist_ok=True)
for path in sorted(glob.glob('dist/lang/*.lang')):
    code = os.path.basename(path)[:-5]
    m = re.search(r'(?m)^\s*' + re.escape(KEY) + r'\s*=\s*(.*?)\s*$', open(path, encoding='utf-8-sig').read())
    if not m or not m.group(1):
        print(f'{code}: no {KEY}, skipped')
        continue
    for sfx, px in SIZES:
        font = ImageFont.truetype(FONT, px)
        ascent, descent = font.getmetrics()
        w = math.ceil(font.getlength(m.group(1))) + 1
        im = Image.new('RGBA', (w, ascent + descent), (255, 255, 255, 0))
        ImageDraw.Draw(im).text((0, 0), m.group(1), font=font, fill=(255, 255, 255, 255))
        out = f'{OUT}/loading-{code}{sfx}.png'
        im.save(out, optimize=True)
        print(f'{out}: {im.width}x{im.height} "{m.group(1)}"')
