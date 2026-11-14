#!/usr/bin/env python3
"""Generate Lumen's controller button icons (original pixel art).

Each icon is drawn on a 36x36 logical pixel grid with hard edges (no
anti-aliasing) and exported scaled x4 with nearest neighbour (144x144), so the
frontend can show it at exactly 36 px with every art pixel on a screen pixel.

Usage: python3 tools/make-icons.py [out_dir]   (default: dist/icons)
Requires Pillow.
"""
import math
import os
import sys

from PIL import Image

G = 36          # logical grid
SCALE = 4       # export scale

# palette
OUT = (14, 14, 20, 255)
BODY = (42, 42, 52, 255)
BODY_LO = (30, 30, 38, 255)
BODY_HI = (66, 66, 82, 255)
SHINE = (104, 104, 126, 255)
GROOVE = (20, 20, 27, 255)
LABEL = (226, 224, 238, 255)
LABEL_SH = (12, 12, 18, 255)
SYMBOLS = {
    "triangle": ((63, 211, 166, 255), (22, 98, 76, 255)),
    "circle":   ((255, 93, 108, 255), (120, 30, 40, 255)),
    "cross":    ((111, 168, 255, 255), (36, 64, 120, 255)),
    "square":   ((244, 143, 216, 255), (116, 50, 98, 255)),
}

# 3x5 pixel font for the labels
FONT = {
    "L": ["X..", "X..", "X..", "X..", "XXX"],
    "R": ["XX.", "X.X", "XX.", "X.X", "X.X"],
    "1": [".X.", "XX.", ".X.", ".X.", "XXX"],
    "2": ["XX.", "..X", ".X.", "X..", "XXX"],
    "S": [".XX", "X..", ".X.", "..X", "XX."],
    "E": ["XXX", "X..", "XX.", "X..", "XXX"],
    "C": [".XX", "X..", "X..", "X..", ".XX"],
    "T": ["XXX", ".X.", ".X.", ".X.", ".X."],
    "A": [".X.", "X.X", "XXX", "X.X", "X.X"],
}


def canvas(w=G):
    return Image.new("RGBA", (w, G), (0, 0, 0, 0))


def put(im, x, y, c):
    if 0 <= x < im.width and 0 <= y < G:
        im.putpixel((x, y), c)


def shade_body(im, inside, edge):
    """Fill a shape with a 3-tone bevel lit from the top-left plus an outline."""
    for y in range(G):
        for x in range(im.width):
            if not inside(x, y):
                continue
            if edge(x, y):
                put(im, x, y, OUT)
                continue
            # distance to the outline towards the light / away from it
            lit = inside(x - 1, y - 1) and not inside(x - 2, y - 2)
            dark = inside(x + 1, y + 1) and not inside(x + 2, y + 2)
            if lit and not edge(x - 1, y - 1):
                put(im, x, y, SHINE)
            elif dark:
                put(im, x, y, BODY_LO)
            else:
                t = (x + y) / (im.width + G)
                put(im, x, y, BODY_HI if t < 0.38 else (BODY if t < 0.66 else BODY_LO))


def text(im, s, cx, cy, k, color, shadow=True):
    w = len(s) * 4 * k - k
    x0, y0 = int(round(cx - w / 2)), int(round(cy - 5 * k / 2))
    for i, ch in enumerate(s):
        for gy, row in enumerate(FONT[ch]):
            for gx, v in enumerate(row):
                if v != "X":
                    continue
                for sy in range(k):
                    for sx in range(k):
                        px, py = x0 + i * 4 * k + gx * k + sx, y0 + gy * k + sy
                        if shadow:
                            put(im, px + 1, py + 1, LABEL_SH)
                        put(im, px, py, color)


def face(name):
    im = canvas()
    c, r = 17.5, 16.6
    inside = lambda x, y: (x + 0.5 - c) ** 2 + (y + 0.5 - c) ** 2 <= r * r
    edge = lambda x, y: inside(x, y) and not all(inside(x + dx, y + dy) for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)))
    shade_body(im, inside, edge)
    col, sh = SYMBOLS[name]

    def seg_dist(px, py, ax, ay, bx, by):
        vx, vy, wx, wy = bx - ax, by - ay, px - ax, py - ay
        t = max(0.0, min(1.0, (wx * vx + wy * vy) / (vx * vx + vy * vy)))
        return math.hypot(px - (ax + t * vx), py - (ay + t * vy))

    def symbol(x, y):
        px, py = x + 0.5, y + 0.5
        if name == "triangle":
            a, b, d = (c, c - 8.5), (c - 8.5, c + 6.0), (c + 8.5, c + 6.0)
            return min(seg_dist(px, py, *a, *b), seg_dist(px, py, *b, *d), seg_dist(px, py, *d, *a)) <= 1.25
        if name == "circle":
            return abs(math.hypot(px - c, py - c) - 7.6) <= 1.35
        if name == "cross":
            return min(seg_dist(px, py, c - 7, c - 7, c + 7, c + 7), seg_dist(px, py, c - 7, c + 7, c + 7, c - 7)) <= 1.35
        if name == "square":
            dx, dy = abs(px - c), abs(py - c)
            return max(dx, dy) <= 7.6 and max(dx, dy) >= 5.0
        return False

    for y in range(G):          # shadow first, then the symbol on top
        for x in range(G):
            if symbol(x - 1, y - 1) and not symbol(x, y):
                put(im, x, y, sh)
    for y in range(G):
        for x in range(G):
            if symbol(x, y):
                put(im, x, y, col)
    return im


def rounded(w, h, rad, cy=17.5, cw=G):
    x0, x1, y0, y1 = (cw - w) / 2, (cw + w) / 2, cy - h / 2, cy + h / 2

    def inside(x, y):
        px, py = x + 0.5, y + 0.5
        if not (x0 <= px <= x1 and y0 <= py <= y1):
            return False
        qx = min(max(px, x0 + rad), x1 - rad)
        qy = min(max(py, y0 + rad), y1 - rad)
        return (px - qx) ** 2 + (py - qy) ** 2 <= rad * rad

    edge = lambda x, y: inside(x, y) and not all(inside(x + dx, y + dy) for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)))
    return inside, edge


def shoulder(label):
    im = canvas()
    inside, edge = rounded(34, 22, 6)
    shade_body(im, inside, edge)
    text(im, label, 18, 17.5, 2, LABEL)
    return im


PILL_W = 56


def pill(label):
    im = canvas(PILL_W)
    inside, edge = rounded(PILL_W - 2, 20, 10, cw=PILL_W)
    shade_body(im, inside, edge)
    text(im, label, PILL_W / 2, 17.5, 2, LABEL)
    return im


def dpad():
    im = canvas()
    arm = 6.0

    def inside(x, y):
        px, py = x + 0.5, y + 0.5
        return (abs(px - 18) <= arm and 2 <= py <= 34) or (abs(py - 18) <= arm and 2 <= px <= 34)

    edge = lambda x, y: inside(x, y) and not all(inside(x + dx, y + dy) for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)))
    shade_body(im, inside, edge)
    # centre dimple and arrow nubs
    for y in range(G):
        for x in range(G):
            if (x + 0.5 - 18) ** 2 + (y + 0.5 - 18) ** 2 <= 9:
                put(im, x, y, BODY_LO)
    # arrows: apex towards each arm tip, 3 rows deep
    for (tx, ty, dx, dy) in ((18, 5, 0, 1), (17, 30, 0, -1), (5, 17, 1, 0), (30, 18, -1, 0)):
        for k in range(3):
            for s in range(-k, k + 1):
                if dy:
                    put(im, tx + s, ty + dy * k, GROOVE)
                else:
                    put(im, tx + dx * k, ty + s, GROOVE)
    return im


ICONS = {
    "cross": lambda: face("cross"),
    "circle": lambda: face("circle"),
    "triangle": lambda: face("triangle"),
    "square": lambda: face("square"),
    "l1": lambda: shoulder("L1"),
    "r1": lambda: shoulder("R1"),
    "l2": lambda: shoulder("L2"),
    "r2": lambda: shoulder("R2"),
    "select": lambda: pill("SELECT"),
    "start": lambda: pill("START"),
    "dpad": dpad,
}


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "..", "dist", "icons")
    os.makedirs(out, exist_ok=True)
    for name, make in ICONS.items():
        im = make()
        im.resize((im.width * SCALE, G * SCALE), Image.NEAREST).save(os.path.join(out, name + ".png"), optimize=True)
    print("wrote", len(ICONS), "icons to", os.path.abspath(out))


if __name__ == "__main__":
    main()
