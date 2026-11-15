"""Game Boy Advance (AGB-001, Indigo) — the handheld.
blender -b -P tools/models/gba.py -- <assets/models dir> <preview dir>

References: tools/models/ref/gba.md (front outline measured as a polygon from a straight-on
photo scaled to the published 144.5 x 82 x 24.5 mm; layout of every control). No logos or
lettering ("GAME BOY ADVANCE", "Nintendo", the legends).
Blender axes: X right, front (screen) towards -Y, Z up, mm; origin at the body's centre
(Lumen's handheld frame). The cartridge sits in the back half under the top edge: its back
face 1 mm inside the back plateau (centre y = +4), its top 8 mm below the top plateau.
"""
import math, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lmdl import *

ARGS = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
OUT, PREV = ARGS[0], ARGS[1]

BODY = hexc("#5e5296")
LIGHT = hexc("#c8c8c6")
LENS = hexc("#2b2d2e")
FRONT, GRIP_BACK, PLATEAU = -12.25, 12.25, 8.75

RIGHT = [(0, 41.0), (30, 41.0), (37.5, 39.4), (45.2, 35.9), (49.0, 34.7), (55, 33.0), (60.4, 32.2), (64.2, 29.3),
         (65.6, 24.8), (66.9, 18.9), (68.2, 13.0), (69.4, 7.1), (70.3, 1.3), (71.1, -4.6), (71.9, -10.5), (72.2, -16.3),
         (71.9, -19.3), (70.5, -22.2), (67.9, -25.1), (61.8, -28.1), (53.0, -31.0), (45.7, -33.9), (38.7, -36.9),
         (29.9, -38.7), (18.5, -40.2), (0, -41.0)]


def outline():
    return mirror_x(RIGHT)


def console():
    reset()
    body = extrude("body", outline(), GRIP_BACK - FRONT, "XZ", FRONT, bevel=2.2, bevel_seg=3)
    # the flatter back between the grips (the grips roll out to 24.5 mm)
    boolean(body, extrude("back_flat", rrect(92.0, 120.0, 14.0, cy=-10.0), 6.0, "XZ", PLATEAU))
    # cartridge slot: through the top edge, and the cut-out in the back showing the pak
    boolean(body, box("slot", 59.0, 8.6, 30.0, loc=(0.0, 4.0, 20.0)))
    boolean(body, extrude("slot_back", rrect(62.0, 40.0, 3.0, cy=30.0), 8.0, "XZ", 4.0 + 3.75))
    # speaker grille: six slots tilted ~13 degrees
    for i in range(6):
        s = box("spk", 15.0, 6.0, 1.2, loc=(51.3, FRONT - 1.0, -12.8 - 7.25 + i * 2.9), r=0.6)
        rotate(s, "Y", -13.0, pivot=(51.3, 0, -12.8))
        boolean(body, s)
    # L / R: the back half of the top corners, following the outline
    lr = []
    for sx in (-1, 1):
        region = box("lr_reg", 26.0, 11.0, 18.0, loc=(sx * 59.0, 5.5, 23.0))
        cut = extrude("lr_cut", outline(), 12.0, "XZ", 0.5)
        boolean(cut, region, "INTERSECT")
        lr.append(cut)
    for c in lr:   # the body gives way to the buttons
        boolean(body, c, keep=True)
    # bottom edge: headphone jack, volume window
    boolean(body, cyl("phones", 3.5, 8.0, loc=(22.5, 0.0, -45.0), axis="Z", n=16))
    boolean(body, box("vol", 8.0, 3.0, 6.0, loc=(36.0, 0.0, -44.0)))
    # front: the speaker and start/select plates sit in shallow recesses
    for y in (-10.5, -19.3):
        boolean(body, extrude("ss_plate", rrect(15.5, 5.8, 2.9, cx=-50.6, cy=y), 1.0, "XZ", FRONT - 0.6))
    part(body, rgba=BODY, gloss=26, spec=0.22)
    for i, c in enumerate(lr):
        c.name = f"shoulder_{i}"
        md = c.modifiers.new("bevel", "BEVEL"); md.width = 1.2; md.segments = 3
        md.limit_method = "ANGLE"; md.angle_limit = math.radians(40)
        apply_mods(c)
        move(c, 0.0, 0.0, 0.4)
        part(c, rgba=LIGHT, gloss=14, spec=0.1)
    # inside the slot (dark), behind the pak
    o = box("slot_dark", 58.0, 1.0, 30.0, loc=(0.0, -0.6, 8.0))
    part(o, "dark", hexc("#151418"))
    # screen: the black lens plate (bottom edge bulging, large lower corners), its window,
    # the LCD behind it
    lens_pts = rrect(80.5, 65.0, [5.0, 5.0, 12.0, 12.0], cx=-0.4, cy=0.5)
    plate = extrude("lens", lens_pts, 0.8, "XZ", FRONT - 0.5, bevel=0.3, bevel_seg=2)
    boolean(plate, extrude("win", rrect(68.0, 44.0, 1.0, cy=5.7), 3.0, "XZ", FRONT - 2.0))
    part(plate, rgba=LENS, gloss=140, spec=0.8)
    o = extrude("lcd", rrect(66.0, 42.0, 0.5, cy=5.7), 0.4, "XZ", FRONT + 0.4)
    part(o, "screen", hexc("#5e625c"), on=(1.0, 1.0, 1.0, 1.0), gloss=120, spec=0.6)
    o = extrude("lcd_glass", rrect(68.0, 44.0, 1.0, cy=5.7), 0.3, "XZ", FRONT - 0.3)
    part(o, "glass", (0.95, 0.97, 1.0, 0.10), gloss=160, spec=0.9)
    # D-pad, A, B, START, SELECT, the power LED
    for w, h in ((19.1, 6.5), (6.5, 19.1)):
        o = extrude("dpad", rrect(w, h, 1.2, cx=-53.9, cy=9.3), 2.8, "XZ", FRONT - 2.5, bevel=0.6, bevel_seg=2)
        part(o, rgba=LIGHT, gloss=16, spec=0.12)
    for x, z in ((46.5, 8.5), (59.3, 13.0)):
        o = cyl("ab", 9.2, 3.0, loc=(x, FRONT - 2.4, z), axis="Y", bevel=1.6, n=32)
        part(o, rgba=LIGHT, gloss=22, spec=0.18)
    for x, z in ((-45.4, -10.5), (-45.9, -19.3)):
        o = cyl("ss", 4.6, 2.0, loc=(x, FRONT - 1.4, z), axis="Y", bevel=0.8, n=20)
        part(o, rgba=LIGHT, gloss=16, spec=0.12)
    led = cyl("led", 2.3, 0.8, loc=(44.8, FRONT - 0.6, 29.3), axis="Y", bevel=0.3, n=16)
    part(led, "light", hexc("#3a3f38"), on=hexc("#38e048"), pivot=(44.8, FRONT - 0.8, 29.3), gloss=120, spec=0.8)
    # power switch on the bottom edge, left grip (slides on)
    o = box("power_sw", 7.0, 5.0, 2.5, loc=(-53.0, -2.0, -36.2), r=1.0, bevel=0.5, bevel_seg=2)
    part(o, rgba=LIGHT, gloss=16, spec=0.12, anim="move", chan="power", axis=(1, 0, 0), amount=4.0)
    # back: battery cover with its latch, screws
    bc = extrude("battery", rrect(60.0, 45.0, 3.0, cx=1.5, cy=-20.5), 0.8, "XZ", PLATEAU - 0.2, bevel=0.3, bevel_seg=2)
    part(bc, rgba=hexc("#574b8c"), gloss=22, spec=0.18)
    o = box("latch", 10.0, 1.0, 3.0, loc=(1.5, PLATEAU + 0.6, 0.5), r=1.0)
    part(o, rgba=hexc("#574b8c"), gloss=22, spec=0.18)
    for x, z in ((-47.0, 27.0), (47.0, 27.0), (-62.0, -22.0), (62.0, -22.0)):
        o = cyl("screw", 3.2, 0.6, loc=(x, GRIP_BACK - 0.3, z), axis="Y", n=12)
        part(o, rgba=hexc("#3c345e"), gloss=40, spec=0.3)
    return "console_gba"


if __name__ == "__main__":
    os.makedirs(PREV, exist_ok=True)
    key = console()
    export(key, OUT)
    preview(key, PREV, views=("34", "front", "back", "top"))
