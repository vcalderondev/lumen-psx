"""Synthetic PS1-like test frames for crt-bench (no game content): a sky gradient, flat
and gouraud polygons, a bright HUD with 1-pixel text strokes, colour bars and a dark area
with a light. Writes ps1-320x240.ppm, ps1-640x480.ppm (the same scene at the enhanced
resolution) and dash-1280x720.ppm (the scene at 4x, cropped: a stand-in for the
dashboard's own frame) into the given folder."""
import sys
from PIL import Image, ImageDraw

def scene(s):
    w, h = 320 * s, 240 * s
    im = Image.new("RGB", (w, h))
    d = ImageDraw.Draw(im)
    for y in range(h * 3 // 5):                       # sky
        t = y / (h * 3 / 5)
        d.line([(0, y), (w, y)], fill=(int(40 + 200 * t), int(70 + 90 * t), int(190 - 120 * t)))
    d.rectangle([0, h * 3 // 5, w, h], fill=(30, 40, 28))   # ground
    for i in range(12):                               # floor tiles in perspective
        y0 = h * 3 // 5 + (i * i) * s // 2
        d.line([(0, y0), (w, y0)], fill=(70, 90, 60), width=s)
    for i in range(-8, 9):
        d.line([(w // 2 + i * 6 * s, h * 3 // 5), (w // 2 + i * 60 * s, h)], fill=(70, 90, 60), width=s)
    d.polygon([(60 * s, 150 * s), (120 * s, 60 * s), (170 * s, 150 * s)], fill=(150, 40, 40))   # buildings
    d.polygon([(120 * s, 60 * s), (170 * s, 150 * s), (200 * s, 140 * s), (150 * s, 55 * s)], fill=(100, 25, 25))
    d.rectangle([210 * s, 80 * s, 280 * s, 150 * s], fill=(60, 120, 170))
    for yy in range(85, 145, 10):
        for xx in range(215, 275, 12):
            d.rectangle([xx * s, yy * s, (xx + 5) * s - 1, (yy + 5) * s - 1], fill=(250, 240, 160))
    d.ellipse([20 * s, 170 * s, 70 * s, 220 * s], fill=(255, 250, 230))   # a light
    for k, c in enumerate([(255, 255, 255), (255, 255, 0), (0, 255, 255), (0, 255, 0),
                           (255, 0, 255), (255, 0, 0), (0, 0, 255), (0, 0, 0)]):
        d.rectangle([(100 + k * 25) * s, 200 * s, (125 + k * 25) * s - 1, 230 * s], fill=c)
    d.rectangle([8 * s, 8 * s, 140 * s, 28 * s], fill=(10, 10, 40))   # HUD
    for k in range(14):                               # text-like 1-px strokes
        x = (12 + k * 9) * s
        d.line([(x, 12 * s), (x, 24 * s)], fill=(255, 255, 255), width=s)
        d.line([(x, 12 * s), (x + 5 * s, 12 * s)], fill=(255, 255, 255), width=s)
        d.line([(x, 18 * s), (x + 4 * s, 18 * s)], fill=(255, 220, 120), width=s)
    d.rectangle([200 * s, 12 * s, 310 * s, 20 * s], fill=(200, 30, 30))
    d.rectangle([200 * s, 12 * s, 270 * s, 20 * s], fill=(60, 230, 60))
    return im

out = sys.argv[1] if len(sys.argv) > 1 else "frames"
scene(1).save(f"{out}/ps1-320x240.ppm")
scene(2).save(f"{out}/ps1-640x480.ppm")
scene(4).crop((0, 120, 1280, 840)).save(f"{out}/dash-1280x720.ppm")
