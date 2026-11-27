#!/usr/bin/env python3
"""Generate Lumen's dashboard glyphs: flat, clean, vector-style (PS5-like).

White on transparent, drawn at 8x and downsampled (smooth edges), exported at
2x the logical size (logical 32 px -> 64 px PNG). The frontend tints them in
the shader, so one white set serves every colour.

Usage: python3 tools/make-glyphs.py [out_dir]   (default: assets/glyphs)
Requires Pillow. Font: Liberation Sans Bold (shipped with the console).
"""
import math
import os
import sys

from PIL import Image, ImageDraw, ImageFont

LOGICAL = 32            # design grid (px)
EXPORT = 2              # export scale -> 64 px
SS = 8                  # supersampling while drawing
N = LOGICAL * SS        # drawing canvas (256)
W = (255, 255, 255, 255)
DIM = (255, 255, 255, 82)   # the unlit part of a level glyph (wifi-0 .. wifi-3)
STROKE = 2.4            # logical stroke width
FONT = os.environ.get("LUMEN_FONT", os.path.join(os.path.dirname(__file__), "..", "..", "device-dump",
                      "sysroot", "usr", "share", "fonts", "ttf", "LiberationSans-Bold.ttf"))


def u(v):
    """logical -> canvas units"""
    return v * SS


def canvas(w=LOGICAL):
    im = Image.new("RGBA", (int(u(w)), N), (0, 0, 0, 0))
    return im, ImageDraw.Draw(im)


def line(d, pts, w=STROKE):
    pts = [(u(x), u(y)) for x, y in pts]
    d.line(pts, fill=W, width=int(u(w)), joint="curve")
    r = u(w) / 2
    for x, y in (pts[0], pts[-1]):            # round caps
        d.ellipse((x - r, y - r, x + r, y + r), fill=W)


def ring(d, cx, cy, r, w=STROKE):
    d.ellipse((u(cx - r), u(cy - r), u(cx + r), u(cy + r)), outline=W, width=int(u(w)))


def disc(d, cx, cy, r):
    d.ellipse((u(cx - r), u(cy - r), u(cx + r), u(cy + r)), fill=W)


def rrect(d, x0, y0, x1, y1, rad, w=STROKE, fill=False):
    box = (u(x0), u(y0), u(x1), u(y1))
    if fill:
        d.rounded_rectangle(box, radius=u(rad), fill=W)
    else:
        d.rounded_rectangle(box, radius=u(rad), outline=W, width=int(u(w)))


def label(d, text, cx, cy, size, w_canvas=LOGICAL, cut=False):
    f = ImageFont.truetype(FONT, int(u(size)))
    b = d.textbbox((0, 0), text, font=f)
    x, y = u(cx) - (b[2] - b[0]) / 2 - b[0], u(cy) - (b[3] - b[1]) / 2 - b[1]
    d.text((x, y), text, font=f, fill=(0, 0, 0, 0) if cut else W)


def knockout_label(im, text, cx, cy, size):
    """text cut out of a filled shape (transparent letters)"""
    mask = Image.new("L", im.size, 0)
    md = ImageDraw.Draw(mask)
    f = ImageFont.truetype(FONT, int(u(size)))
    b = md.textbbox((0, 0), text, font=f)
    md.text((u(cx) - (b[2] - b[0]) / 2 - b[0], u(cy) - (b[3] - b[1]) / 2 - b[1]), text, font=f, fill=255)
    a = im.getchannel("A")
    im.putalpha(Image.composite(Image.new("L", im.size, 0), a, mask))


# ---------------------------------------------------------------- face buttons
def face(symbol):
    im, d = canvas()
    ring(d, 16, 16, 14.2, 1.8)
    if symbol == "cross":
        line(d, [(10.6, 10.6), (21.4, 21.4)]); line(d, [(21.4, 10.6), (10.6, 21.4)])
    elif symbol == "circle":
        ring(d, 16, 16, 6.2)
    elif symbol == "triangle":
        h = 6.6
        pts = [(16, 16 - h), (16 - h * 0.98, 16 + h * 0.62), (16 + h * 0.98, 16 + h * 0.62), (16, 16 - h)]
        line(d, pts)
    elif symbol == "square":
        rrect(d, 10.4, 10.4, 21.6, 21.6, 1.2)
    return im


def shoulder(text):
    im, d = canvas(40)
    rrect(d, 1.5, 6, 38.5, 26, 6, fill=True)
    knockout_label(im, text, 20, 16.2, 12)
    return im


def pill(text, width):
    im, d = canvas(width)
    rrect(d, 1.2, 8, width - 1.2, 24, 8, 1.8)
    label(d, text, width / 2, 16.2, 8.5, width)
    return im


def dpad():
    im, d = canvas()
    a = 4.6
    pts = [(16 - a, 3), (16 + a, 3), (16 + a, 16 - a), (29, 16 - a), (29, 16 + a), (16 + a, 16 + a),
           (16 + a, 29), (16 - a, 29), (16 - a, 16 + a), (3, 16 + a), (3, 16 - a), (16 - a, 16 - a)]
    d.polygon([(u(x), u(y)) for x, y in pts], outline=W, width=int(u(1.8)))
    # small arrows centred in each arm, apex pointing outwards
    for (cx, cy, ox, oy) in ((16, 8.2, 0, -1), (16, 23.8, 0, 1), (8.2, 16, -1, 0), (23.8, 16, 1, 0)):
        h, s_ = 1.5, 1.7                       # half height (along the arm), half base
        px_, py_ = -oy, ox                     # perpendicular
        tri = [(cx + ox * h, cy + oy * h), (cx - ox * h + px_ * s_, cy - oy * h + py_ * s_),
               (cx - ox * h - px_ * s_, cy - oy * h - py_ * s_)]
        d.polygon([(u(px), u(py)) for px, py in tri], fill=W)
    return im


# ---------------------------------------------------------------- status / top icons
def pad():
    im, d = canvas()
    # controller silhouette: body with two grips
    d.rounded_rectangle((u(4), u(9), u(28), u(21)), radius=u(6), outline=W, width=int(u(STROKE)))
    line(d, [(8.5, 19.5), (6.5, 26), (10.5, 26.5), (13, 21)])
    line(d, [(23.5, 19.5), (25.5, 26), (21.5, 26.5), (19, 21)])
    line(d, [(9.5, 15), (13.5, 15)]); line(d, [(11.5, 13), (11.5, 17)])
    disc(d, 20.5, 13.6, 1.3); disc(d, 23.2, 16.2, 1.3)
    return im


def network():
    im, d = canvas()
    for i, r in enumerate((13, 8.6, 4.2)):
        d.arc((u(16 - r), u(25 - r), u(16 + r), u(25 + r)), 225, 315, fill=W, width=int(u(STROKE)))
    disc(d, 16, 24.5, 1.9)
    return im


def wifi(level):
    """Wi-Fi signal for the status bar (the wifi module): `level` of the three arcs lit,
    the rest drawn faint, so the shape never changes width as the signal moves"""
    im, d = canvas()
    for i, r in enumerate((4.2, 8.6, 13)):
        col = W if i < level else DIM
        d.arc((u(16 - r), u(25 - r), u(16 + r), u(25 + r)), 225, 315, fill=col, width=int(u(STROKE)))
    disc(d, 16, 24.5, 1.9)
    return im


def bluetooth():
    im, d = canvas()
    # the rune: a vertical stroke with the two crossing arrowheads
    line(d, [(10.5, 11), (21.5, 21), (16, 26), (16, 6), (21.5, 11), (10.5, 21)])
    return im


def ethernet():
    im, d = canvas()
    # an RJ45 socket: the body, the latch notch and three contacts
    line(d, [(6, 25), (6, 11), (11, 11), (11, 7), (21, 7), (21, 11), (26, 11), (26, 25), (6, 25)])
    for x in (12, 16, 20):
        line(d, [(x, 16), (x, 20.5)])
    return im


def clock():
    im, d = canvas()
    ring(d, 16, 16, 12)
    line(d, [(16, 16), (16, 9.5)]); line(d, [(16, 16), (20.5, 18.5)])
    return im


def info():
    im, d = canvas()
    ring(d, 16, 16, 12)
    disc(d, 16, 10.6, 1.7)
    line(d, [(16, 14.6), (16, 22)])
    return im


def gallery():
    im, d = canvas()
    rrect(d, 4, 6, 28, 26, 3)
    d.polygon([(u(7), u(23)), (u(13), u(15)), (u(18), u(20.5)), (u(21), u(17.5)), (u(25), u(23))], fill=W)
    disc(d, 21, 11.5, 2.2)
    return im


def settings():
    im, d = canvas()
    teeth, ro, ri = 8, 12.5, 9.8
    pts = []
    for k in range(teeth * 2):
        a0 = (k / (teeth * 2)) * 2 * math.pi
        r = ro if k % 2 == 0 else ri
        for t in (-0.11, 0.11):
            a = a0 + t
            pts.append((16 + r * math.cos(a), 16 + r * math.sin(a)))
    d.polygon([(u(x), u(y)) for x, y in pts], fill=W)
    disc_mask = Image.new("L", im.size, 0)
    ImageDraw.Draw(disc_mask).ellipse((u(16 - 4.2), u(16 - 4.2), u(16 + 4.2), u(16 + 4.2)), fill=255)
    im.putalpha(Image.composite(Image.new("L", im.size, 0), im.getchannel("A"), disc_mask))
    return im


# ---------------------------------------------------------------- media badges
def disc_badge():
    im, d = canvas()
    ring(d, 16, 16, 12.5)
    ring(d, 16, 16, 3.6, 1.8)
    d.arc((u(7.5), u(7.5), u(24.5), u(24.5)), 200, 250, fill=W, width=int(u(1.6)))
    return im


def cartridge():
    im, d = canvas()
    pts = [(7, 4), (25, 4), (25, 25), (22.5, 28), (9.5, 28), (7, 25), (7, 4)]
    line(d, pts)
    rrect(d, 10.5, 8, 21.5, 18, 1.5, 1.8)
    for x in (12, 14.7, 17.4, 20):
        line(d, [(x, 24.2), (x, 25.6)], 1.4)
    return im


def coin():
    im, d = canvas()
    ring(d, 16, 16, 12.5)
    ring(d, 16, 16, 8.8, 1.4)
    line(d, [(16, 10.5), (16, 21.5)])
    return im


GLYPHS = {
    "cross": lambda: face("cross"), "circle": lambda: face("circle"),
    "triangle": lambda: face("triangle"), "square": lambda: face("square"),
    "l1": lambda: shoulder("L1"), "r1": lambda: shoulder("R1"),
    "l2": lambda: shoulder("L2"), "r2": lambda: shoulder("R2"),
    "select": lambda: pill("SELECT", 52), "start": lambda: pill("START", 46),
    "dpad": dpad,
    "pad": pad, "network": network, "clock": clock,
    "info": info, "gallery": gallery, "settings": settings,
    "disc": disc_badge, "cartridge": cartridge, "coin": coin,
    # status bar glyphs of the built-in modules (docs/modules.md, "status")
    "wifi-0": lambda: wifi(0), "wifi-1": lambda: wifi(1), "wifi-2": lambda: wifi(2), "wifi-3": lambda: wifi(3),
    "bluetooth": bluetooth, "ethernet": ethernet,
}


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "..", "assets", "glyphs")
    os.makedirs(out, exist_ok=True)
    for name, make in GLYPHS.items():
        im = make()
        w = round(im.width / SS * EXPORT)
        im.resize((w, LOGICAL * EXPORT), Image.LANCZOS).save(os.path.join(out, name + ".png"), optimize=True)
    print("wrote", len(GLYPHS), "glyphs to", os.path.abspath(out))


if __name__ == "__main__":
    main()
