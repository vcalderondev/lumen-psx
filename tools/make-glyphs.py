#!/usr/bin/env python3
"""Generate Lumen's dashboard glyphs: flat, clean, vector-style (PS5-like).

White on transparent, drawn at 8x and downsampled (smooth edges), exported at
2x the logical size (logical 32 px -> 64 px PNG). The frontend tints them in
the shader, so one white set serves every colour.

One visual language for the whole set: outlines with round caps and joins, 1.8 px for
the frame of a button (ring, shoulder, pill), 2.4 px for symbols; labels in the UI font.

Usage: python3 tools/make-glyphs.py [--square] [--font FILE] [out_dir]
  out_dir   default: assets/glyphs
  --square  hard corners on shoulders, pills and the d-pad, and only those glyphs are
            written (a theme with `radius = 0` ships them in its glyphs/ folder)
  --font    label font (default: assets/fonts/bold.ttf, the UI font; LUMEN_FONT too)
Requires Pillow.
"""
import math
import os
import sys

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

LOGICAL = 32            # design grid (px)
EXPORT = 2              # export scale -> 64 px
SS = 8                  # supersampling while drawing
N = LOGICAL * SS        # drawing canvas (256)
W = (255, 255, 255, 255)
DIM = (255, 255, 255, 82)   # the unlit part of a level glyph (wifi-0 .. wifi-3)
STROKE = 2.4            # logical stroke width
FRAME = 1.8             # logical stroke width of a button's frame
FONT = os.environ.get("LUMEN_FONT", os.path.join(os.path.dirname(__file__), "..", "assets", "fonts", "bold.ttf"))
SQUARE = False          # --square: hard corners


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


def arc(d, cx, cy, r, a0, a1, col=W, w=STROKE):
    """an arc with round caps (angles in degrees, clockwise from 3 o'clock)"""
    d.arc((u(cx - r), u(cy - r), u(cx + r), u(cy + r)), a0, a1, fill=col, width=int(u(w)))
    rm, cap = r - w / 2, u(w) / 2
    for ang in (a0, a1):
        x, y = u(cx + rm * math.cos(math.radians(ang))), u(cy + rm * math.sin(math.radians(ang)))
        d.ellipse((x - cap, y - cap, x + cap, y + cap), fill=col)


def outline(im, w=STROKE):
    """the filled silhouette in `im` as an outline of w logical px, inside its edge"""
    a = im.getchannel("A")
    inner = a.filter(ImageFilter.MinFilter(2 * int(u(w)) + 1))
    im.putalpha(ImageChops.subtract(a, inner))
    return im


def shoulder(text):
    """L1 / R1 / L2 / R2: the same frame as Select and Start, a little taller"""
    im, d = canvas(40)
    rrect(d, 1.2, 6.5, 38.8, 25.5, 0 if SQUARE else 6, FRAME)
    label(d, text, 20, 16, 12, 40)
    return im


def pill(text, width):
    im, d = canvas(width)
    rrect(d, 1.2, 8, width - 1.2, 24, 0 if SQUARE else 8, FRAME)
    label(d, text, width / 2, 16, 8.5, width)
    return im


def pad_button(kind):
    """the DualSense's Create and Options: no words on that controller, so the pill holds
    the button's own mark, centred and in a fine stroke: three rays fanning up from one
    point (Create), three short lines (Options)"""
    width = 28
    im, d = canvas(width)
    rrect(d, 1.2, 8, width - 1.2, 24, 0 if SQUARE else 8, FRAME)
    cx = width / 2
    if kind == "create":
        for ang in (-90, -155, -25):   # \ | / : apart at the foot, so it never reads as an arrow
            c, s_ = math.cos(math.radians(ang)), math.sin(math.radians(ang))
            line(d, [(cx + 3.4 * c, 20.6 + 3.4 * s_), (cx + 7.4 * c, 20.6 + 7.4 * s_)], 1.4)
    else:
        for y in (13.4, 16, 18.6):
            line(d, [(cx - 3.8, y), (cx + 3.8, y)], 1.4)
    return im


def dpad():
    im, d = canvas()
    a = 4.6
    pts = [(16 - a, 3), (16 + a, 3), (16 + a, 16 - a), (29, 16 - a), (29, 16 + a), (16 + a, 16 + a),
           (16 + a, 29), (16 - a, 29), (16 - a, 16 + a), (3, 16 + a), (3, 16 - a), (16 - a, 16 - a)]
    if SQUARE:
        d.polygon([(u(x), u(y)) for x, y in pts], outline=W, width=int(u(FRAME)))
    else:   # the cross with rounded corners: two rounded bars, outlined as one shape
        d.rounded_rectangle((u(16 - a), u(3), u(16 + a), u(29)), radius=u(1.6), fill=W)
        d.rounded_rectangle((u(3), u(16 - a), u(29), u(16 + a)), radius=u(1.6), fill=W)
        outline(im, FRAME)
        d = ImageDraw.Draw(im)
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
    # controller silhouette: a body and two grips, outlined as one shape
    d.rounded_rectangle((u(2.5), u(8), u(29.5), u(20)), radius=u(6), fill=W)
    d.rounded_rectangle((u(2.5), u(10), u(12), u(26.5)), radius=u(4.7), fill=W)
    d.rounded_rectangle((u(20), u(10), u(29.5), u(26.5)), radius=u(4.7), fill=W)
    outline(im, 2.0)
    d = ImageDraw.Draw(im)
    line(d, [(7.3, 14.4), (12.1, 14.4)], 1.8); line(d, [(9.7, 12), (9.7, 16.8)], 1.8)
    disc(d, 20.6, 13.0, 1.35); disc(d, 23.6, 15.8, 1.35)
    return im


def wifi(level):
    """Wi-Fi signal for the status bar (the wifi module): `level` of the three arcs lit,
    the rest drawn faint, so the shape never changes width as the signal moves"""
    im, d = canvas()
    for i, r in reversed(list(enumerate((7.2, 11.6, 16)))):
        arc(d, 16, 24.6, r, 228, 312, W if i < level else DIM)
    disc(d, 16, 24.6, 2.0)
    return im


def network():
    return wifi(3)


def players():
    im, d = canvas()
    ring(d, 12, 11, 4.6, 2.2)
    arc(d, 12, 31, 10.5, 197, 343, w=2.2)
    arc(d, 21, 11, 4.6, 290, 70, w=2.2)
    arc(d, 23, 31, 9.5, 300, 345, w=2.2)
    return im


def calendar():
    im, d = canvas()
    rrect(d, 4.5, 7, 27.5, 27.5, 4, 2.2)
    line(d, [(5.5, 13.5), (26.5, 13.5)], 2.2)
    line(d, [(11, 4), (11, 9)], 2.2); line(d, [(21, 4), (21, 9)], 2.2)
    return im


def star():
    im, d = canvas()
    pts = []
    for k in range(10):
        a = -math.pi / 2 + k * math.pi / 5
        r = 12.5 if k % 2 == 0 else 5.4
        pts.append((16 + r * math.cos(a), 17 + r * math.sin(a)))
    line(d, pts + [pts[0]], 2.2)
    return im


def play():
    """the main action: a filled triangle with soft corners"""
    im, d = canvas()
    pts = [(11, 7.5), (24.5, 16), (11, 24.5)]
    d.polygon([(u(x), u(y)) for x, y in pts], fill=W)
    line(d, pts + [pts[0]], 3.0)
    return im


def folder():
    im, d = canvas()
    line(d, [(4.5, 24), (4.5, 8.5), (12.5, 8.5), (15.5, 12), (27.5, 12), (27.5, 24), (4.5, 24)], 2.2)
    return im


def save():
    im, d = canvas()
    line(d, [(5.5, 5.5), (21.5, 5.5), (26.5, 10.5), (26.5, 26.5), (5.5, 26.5), (5.5, 5.5)], 2.2)
    line(d, [(10.5, 6.5), (10.5, 12), (19.5, 12), (19.5, 6.5)], 2.0)
    rrect(d, 10, 18, 22, 26.5, 1.5, 2.0)
    return im


def exit_():
    im, d = canvas()
    line(d, [(18, 5.5), (7.5, 5.5), (7.5, 26.5), (18, 26.5)], 2.2)
    line(d, [(14, 16), (27, 16)], 2.2); line(d, [(22.5, 11.5), (27, 16), (22.5, 20.5)], 2.2)
    return im


def user():
    im, d = canvas()
    ring(d, 16, 11, 5.4, 2.2)
    arc(d, 16, 32, 11.5, 200, 340, w=2.2)
    return im


def screen():
    im, d = canvas()
    rrect(d, 3.5, 6, 28.5, 22, 3, 2.2)
    line(d, [(16, 22.5), (16, 26.5)], 2.2); line(d, [(11, 26.5), (21, 26.5)], 2.2)
    return im


def sound():
    im, d = canvas()
    line(d, [(5.5, 13), (10, 13), (16, 8), (16, 24), (10, 19), (5.5, 19), (5.5, 13)], 2.2)
    arc(d, 15, 16, 7.5, 318, 42, w=2.2)
    arc(d, 15, 16, 12.5, 318, 42, w=2.2)
    return im


def library():
    im, d = canvas()
    rrect(d, 5, 6, 11, 26, 1.8, 2.2); rrect(d, 14, 6, 20, 26, 1.8, 2.2)
    line(d, [(23.5, 9), (27, 25.5)], 2.2)
    return im


def modules():
    im, d = canvas()
    rrect(d, 5, 5, 14, 14, 2.4, 2.2); rrect(d, 18, 5, 27, 14, 2.4, 2.2); rrect(d, 5, 18, 14, 27, 2.4, 2.2)
    line(d, [(22.5, 18.5), (22.5, 26.5)], 2.2); line(d, [(18.5, 22.5), (26.5, 22.5)], 2.2)
    return im


def system():
    im, d = canvas()
    rrect(d, 9, 9, 23, 23, 3, 2.2)
    for v in (13, 19):
        line(d, [(v, 4), (v, 8)], 2.0); line(d, [(v, 24), (v, 28)], 2.0)
        line(d, [(4, v), (8, v)], 2.0); line(d, [(24, v), (28, v)], 2.0)
    return im


def chevron(down=False):
    """the mark of a row that opens (right) or has dropped its content down"""
    im, d = canvas()
    line(d, [(10, 12.5), (16, 18.5), (22, 12.5)] if down else [(12.5, 10), (18.5, 16), (12.5, 22)])
    return im


def bluetooth():
    im, d = canvas()
    # the rune: a vertical stroke with the two crossing arrowheads
    line(d, [(10.5, 11), (21.5, 21), (16, 26), (16, 6), (21.5, 11), (10.5, 21)])
    return im


def bluetooth_off():
    """Bluetooth with no adapter or switched off: the rune, crossed out (a gap cut around
    the slash so it reads as one stroke over the rune)"""
    im, d = canvas()
    line(d, [(10.5, 11), (21.5, 21), (16, 26), (16, 6), (21.5, 11), (10.5, 21)])
    gap, gd = canvas()
    line(gd, [(7.5, 6.5), (24.5, 25.5)], w=STROKE * 2)   # the cut: twice a stroke wide
    im = Image.composite(Image.new("RGBA", im.size, (0, 0, 0, 0)), im, gap.split()[3])
    d = ImageDraw.Draw(im)
    line(d, [(7.5, 6.5), (24.5, 25.5)])
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
    outline(im, 2.2)          # an outlined cog, like the rest of the set
    ring(ImageDraw.Draw(im), 16, 16, 4.4, 2.0)
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
    # a cartridge: the shell with its two cut corners, the label and the grip lines
    pts = [(9.5, 4), (22.5, 4), (25, 6.5), (25, 28), (7, 28), (7, 6.5), (9.5, 4)]
    line(d, pts)
    rrect(d, 11, 9, 21, 17.5, 1.5, FRAME)
    for x in (12.4, 16, 19.6):
        line(d, [(x, 22), (x, 24.4)], 1.6)
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
    # Select and Start of the controller in use (main.c, pad_icon): DualShock 4, DualSense
    "ds4-share": lambda: pill("SHARE", 48), "ds4-options": lambda: pill("OPTIONS", 60),
    "ds5-create": lambda: pad_button("create"), "ds5-options": lambda: pad_button("options"),
    "dpad": dpad,
    "pad": pad, "network": network, "clock": clock,
    "info": info, "gallery": gallery, "settings": settings,
    "disc": disc_badge, "cartridge": cartridge, "coin": coin,
    # status bar glyphs of the built-in modules (docs/modules.md, "status")
    "wifi-0": lambda: wifi(0), "wifi-1": lambda: wifi(1), "wifi-2": lambda: wifi(2), "wifi-3": lambda: wifi(3),
    "bluetooth": bluetooth, "bluetooth-off": bluetooth_off, "ethernet": ethernet,
    "chevron-right": chevron, "chevron-down": lambda: chevron(True),
    "players": players, "calendar": calendar, "star": star,
    # actions of the game sheet and the pause; sections of the settings' rail
    "play": play, "folder": folder, "save": save, "exit": exit_,
    "user": user, "screen": screen, "sound": sound, "library": library, "modules": modules, "system": system,
}
# the glyphs with corners: what --square writes
CORNERED = ("l1", "r1", "l2", "r2", "select", "start", "dpad")


def main():
    global FONT, SQUARE
    args = sys.argv[1:]
    if "--square" in args:
        SQUARE = True
        args.remove("--square")
    if "--font" in args:
        i = args.index("--font")
        FONT = args[i + 1]
        del args[i:i + 2]
    out = args[0] if args else os.path.join(os.path.dirname(__file__), "..", "assets", "glyphs")
    os.makedirs(out, exist_ok=True)
    names = CORNERED if SQUARE else list(GLYPHS)
    for name in names:
        im = GLYPHS[name]()
        w = round(im.width / SS * EXPORT)
        im.resize((w, LOGICAL * EXPORT), Image.LANCZOS).save(os.path.join(out, name + ".png"), optimize=True)
    print("wrote", len(names), "glyphs to", os.path.abspath(out))


if __name__ == "__main__":
    main()
