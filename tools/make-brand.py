#!/usr/bin/env python3
"""Render the Lumen PSX brand SVGs (assets/brand/*.svg) to the PNGs the console uses.

The SVGs come from the website (lumen-web/scripts/brand/logo.py, public/brand/) and
are plain <rect>/<polygon> geometry, so they are rasterized here directly with Pillow
(8x supersampling, no SVG library). Colours are kept as they are: never white-on-black.

  logo-front.png       frontal LPSX for the dashboard top bar at 720p: 3 px per voxel
  logo-front-1080.png  the same at 1080p (1.5x: 4.5 px per voxel)
  logo-front-splash.png       the startup splash's logo at 720p: 10 px per voxel
  logo-front-splash-1080.png  the same at 1080p (15 px per voxel)
  logo.png             isometric LPSX (primary logo), 640 px wide: boot / splash screens
  mark.png             the L of four cubes, 256 px tall: small sizes

Usage: python3 tools/make-brand.py [dir]   (default: assets/brand). Requires Pillow
(e.g. docker run --rm -v "$PWD:/w" -w /w psc-sim:latest python3 tools/make-brand.py).
"""
import re
import sys

from PIL import Image, ImageDraw

D = sys.argv[1] if len(sys.argv) > 1 else 'assets/brand'
SS = 8
VOXEL = 16.0   # SVG units per voxel in logo-front.svg


def render(name, scale):
    """scale: output px per SVG unit; cropped to the drawn geometry (+1 px clear border)."""
    svg = open(f'{D}/{name}.svg').read()
    vx, vy, vw, vh = map(float, re.search(r'viewBox="([^"]+)"', svg).group(1).split())
    k = scale * SS
    im = Image.new('RGBA', (int(vw * k + 0.999), int(vh * k + 0.999)), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    # draw in document order: later faces cover earlier ones, as in the SVG
    for m in re.finditer(r'<(rect|polygon)\s([^>]*)/>', svg):
        a = dict(re.findall(r'([\w-]+)="([^"]*)"', m.group(2)))
        if m.group(1) == 'rect':
            x, y, w, h = (float(a[t]) for t in ('x', 'y', 'width', 'height'))
            d.rectangle([(x - vx) * k, (y - vy) * k, (x + w - vx) * k - 1, (y + h - vy) * k - 1], fill=a['fill'])
        else:
            pts = [((float(p) - vx) * k, (float(q) - vy) * k) for p, q in (s.split(',') for s in a['points'].split())]
            d.polygon(pts, fill=a['fill'], outline=a['fill'])
    l, t, r, b = im.getbbox()
    l, t = (l // SS) * SS, (t // SS) * SS          # crop on whole output pixels
    r, b = -(-r // SS) * SS, -(-b // SS) * SS
    im = im.crop((l, t, r, b)).resize(((r - l) // SS, (b - t) // SS), Image.LANCZOS)
    out = Image.new('RGBA', (im.width + 2, im.height + 2), (0, 0, 0, 0))
    out.paste(im, (1, 1))
    return out


def save(im, path):
    im.save(f'{D}/{path}', optimize=True)
    print(f'{path}: {im.width}x{im.height}')


save(render('logo-front', 3.0 / VOXEL), 'logo-front.png')
save(render('logo-front', 4.5 / VOXEL), 'logo-front-1080.png')
big, big1080 = render('logo-front', 10.0 / VOXEL), render('logo-front', 15.0 / VOXEL)
save(big, 'logo-front-splash.png')
save(big1080, 'logo-front-splash-1080.png')
svg = open(f'{D}/logo.svg').read()
save(render('logo', 640 / float(re.search(r'viewBox="[^ ]+ [^ ]+ ([^ ]+)', svg).group(1))), 'logo.png')
svg = open(f'{D}/mark.svg').read()
save(render('mark', 256 / float(re.search(r'viewBox="[^ ]+ [^ ]+ [^ ]+ ([^"]+)"', svg).group(1))), 'mark.png')
