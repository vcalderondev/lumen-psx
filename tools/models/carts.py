"""Cartridges and cards modelled from the reference sheets (tools/models/ref/*.md).
blender -b -P tools/models/carts.py -- <assets/models dir> <preview dir> [key ...]

Blender axes: X right, front (label) towards -Y, Z up, mm; origin at the cartridge's
centre (Lumen fits the bounds to the declared size). No logos or lettering: embossed
brand text areas are left as plain recesses.
"""
import math, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lmdl import *

ARGS = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
OUT, PREV = ARGS[0], ARGS[1]


def recess(target, outline, depth, face_y, front=True):
    """Cut a recess of depth into the face at y = face_y (front: the -Y face)."""
    y0 = face_y - 1.0 if front else face_y
    boolean(target, extrude("rec", outline, depth + 1.0, "XZ", y0))


# ---- Game Boy (DMG) Game Pak: ref/gb.md 6 -----------------------------------------------
def cart_gb():
    reset()
    W, H, D = 57.0, 65.5, 7.7
    top, fy, by = H / 2, -D / 2, D / 2
    body = extrude("shell", rrect(W, H, 1.0), D, "XZ", -D / 2, bevel=0.5, bevel_seg=2)
    # the cut corner (top right seen from the label): 5.7 wide, 2.6 deep, through
    boolean(body, box("notch", 5.7 + 0.5, D + 2, 2.6 + 0.5, loc=(W / 2 - 5.7 / 2 + 0.25, 0, top - 2.6)))
    # top stadium (the embossed name sits here on the real one: left plain)
    recess(body, rrect(44.7, 13.1, 6.5, cx=0.6, cy=21.5), 0.45, fy)
    # label recess and the arrow under it
    recess(body, rrect(45.1, 40.0, 0.9, cx=0.8, cy=-5.4), 0.45, fy)
    recess(body, [(1.5 - 4.5, -26.6), (1.5 + 4.5, -26.6), (1.5, -31.8)], 0.45, fy)
    # back: the connector opening along the bottom edge
    boolean(body, box("conn", 48.0, 2.6, 6.0, loc=(0, by - 1.3, -top - 1.0)))
    part(body, rgba=hexc("#a9a7ac"), gloss=14, spec=0.1)
    # grip ridges, five each side under the top edge
    for x0 in (-28.5, 22.5):
        for i in range(5):
            o = box("ridge", 6.0, 0.6, 0.8, loc=(x0 + 3.0, fy - 0.2, top - 5.1 - i * 1.85 - 0.4), r=0.3)
            part(o, rgba=hexc("#a9a7ac"), gloss=14, spec=0.1)
    o = cyl("screw", 3.8, 0.5, loc=(0, by - 0.3, -10.0), axis="Y", n=20)
    part(o, rgba=hexc("#8d8f94"), gloss=60, spec=0.5)
    # the label (the game's art)
    o = extrude("label", rrect(43.5, 38.5, 0.6, cx=0.8, cy=-5.4), 0.2, "XZ", fy + 0.3)
    part(o, "label", (1, 1, 1, 1), gloss=14, spec=0.1)
    return "cart_gb"


# ---- NES Game Pak: ref/nes.md 5, checked against the Matter and Form 3D scan -----------
# 120 x 134 x 17. u = mm from the left edge, v = mm down from the top (front view).
def cart_nes():
    reset()
    W, H, D = 120.0, 134.0, 17.0
    top, fy, by = H / 2, -D / 2, D / 2
    X = lambda u: -W / 2 + u
    Z = lambda v: top - v
    body = extrude("shell", rrect(W, H, 1.2), D, "XZ", -D / 2, bevel=0.6, bevel_seg=2)
    # the narrow connector end: 6.9 mm in on each side over the bottom 24.5
    for sx in (-1, 1):
        boolean(body, box("step", 6.9 + 1, D + 2, 24.5 + 1, loc=(sx * (W / 2 - 6.9 / 2 + 0.5), 0, -top - 1)))
        boolean(body, box("topnotch", 4.0 + 1, D + 2, 2.0 + 1, loc=(sx * (W / 2 - 2.0 + 0.5), 0, top - 2.0)))
    # label recess, open at the top edge (the label folds over the top)
    recess(body, rrect(56.4, 92.7, [0.0, 0.0, 0.8, 0.8], cx=X(48.1 + 28.2), cy=Z(91.7 / 2) + 0.5), 0.5, fy)
    # the grip strip: its upper window, then 47 grooves at 2.5 mm
    recess(body, rrect(26.3, 14.5, 0.6, cx=X(18.8 + 13.15), cy=Z(1.5 + 7.25)), 0.35, fy)
    for i in range(47):
        v = 3.7 + 0.6 + i * 2.5
        if v < 16.5:
            continue
        boolean(body, box("groove", 26.3, 1.4, 1.2, loc=(X(18.8 + 13.15), fy - 0.6 + 0.0, Z(v) - 0.6)))
    # the arrow, pointing down to the connector
    recess(body, [(X(60) - 6, Z(99)), (X(60) + 6, Z(99)), (X(60), Z(109))], 0.5, fy)
    # back: the seal panel, five screws
    recess(body, rrect(70.0, 40.0, 2.0, cy=Z(80)), 0.4, by - 0.4, front=False)
    part(body, rgba=hexc("#808080"), gloss=14, spec=0.1)
    for (u, v) in ((12, 12), (108, 12), (12, 100), (108, 100), (60, 120)):
        o = cyl("screw", 4.0, 0.5, loc=(X(u), by - 0.3, Z(v)), axis="Y", n=20)
        part(o, rgba=hexc("#6d6d70"), gloss=60, spec=0.5)
    o = extrude("label", rrect(55.4, 91.2, 0.6, cx=X(48.1 + 28.2), cy=Z(91.2 / 2)), 0.2, "XZ", fy + 0.25)
    part(o, "label", (1, 1, 1, 1), gloss=18, spec=0.12)
    return "cart_nes"


# ---- Super Famicom / PAL Super Nintendo cartridge: ref/snes.md 5 -------------------------
# 128 x 87 x 20, landscape. The front rolls into the top with R6, the back with R2.
def cart_snes():
    reset()
    W, H, D = 128.0, 87.0, 20.0
    fy, by = -D / 2, D / 2
    prof = [(fy, 0.0), (fy, H - 6.0)]
    prof += arc(fy + 6.0, H - 6.0, 6.0, 180, 90, 6)[1:]
    prof += arc(by - 2.0, H - 2.0, 2.0, 90, 0, 3)[1:]
    prof += [(by, 0.0)]
    shell = extrude("shell", [(y, z) for y, z in prof], W, "YZ", -W / 2)
    boolean(shell, extrude("clip", rrect(W, D + 2, [1.5, 1.5, 6.0, 6.0]), H + 2, "XY", -1.0), "INTERSECT")
    md = shell.modifiers.new("edges", "BEVEL")
    md.width = 0.8; md.segments = 2; md.limit_method = "ANGLE"; md.angle_limit = math.radians(50)
    apply_mods(shell)
    # front: seams of the side rails, the label recess, the finger-grip stadium
    for sx in (-1, 1):
        boolean(shell, box("seam_t", 0.8, 1.0, 6.5, loc=(sx * 43.8, fy, H - 6.0)))
        boolean(shell, box("seam_b", 0.8, 1.0, 44.0, loc=(sx * 43.8, fy, -0.5)))
    recess(shell, rrect(107.0, 36.5, 3.0, cy=H - 6.0 - 18.25), 0.5, fy)
    recess(shell, rrect(72.0, 6.0, 3.0, cy=30.0), 1.5, fy)
    # grip slots: five per side near the top, round the front corners and along the sides
    for sx in (-1, 1):
        for i in range(5):
            z = H - 7.0 - i * 4.0 - 0.8
            boolean(shell, box("gs_side", 2.0, D + 2.0, 1.6, loc=(sx * (W / 2), 0.0, z)))
            boolean(shell, box("gs_front", 4.5, 2.0, 1.6, loc=(sx * (W / 2 - 2.25), fy, z)))
    # back: the recessed centre panel between the rails; bottom: the connector opening
    boolean(shell, box("back_panel", 87.0, 2.0, 70.0, loc=(0.0, by + 0.2, 4.0)))
    boolean(shell, box("conn", 98.5, 12.0, 7.0, loc=(0.0, 0.0, -1.0)))
    part(shell, rgba=hexc("#cdd0d1"), gloss=14, spec=0.1)
    for x in (-12.0, 12.0):
        o = box("grip_div", 1.2, 1.2, 5.0, loc=(x, fy + 0.9, 27.5))
        part(o, rgba=hexc("#cdd0d1"), gloss=14, spec=0.1)
    for sx in (-1, 1):
        o = cyl("screw", 4.3, 0.6, loc=(sx * 56.4, fy - 0.1, 6.0), axis="Y", n=20)
        part(o, rgba=hexc("#b6ac7e"), gloss=70, spec=0.6)
    o = extrude("label", rrect(105.5, 35.0, 2.5, cy=H - 6.0 - 18.25), 0.2, "XZ", fy + 0.2)
    part(o, "label", (1, 1, 1, 1), gloss=18, spec=0.12)
    o = extrude("back_label", rrect(72.0, 15.0, 1.5, cy=14.0), 0.2, "XZ", by - 0.9)
    part(o, rgba=hexc("#e8e6df"), gloss=10, spec=0.05)
    translate_all(0.0, 0.0, -H / 2)
    return "cart_snes"


# ---- Game Boy Advance Game Pak: ref/gba.md 6 --------------------------------------------
# 57 x 35 x 7.5, the top band 7.6 tall and 0.7 wider each side; label recess 43.8 x 22.4.
def cart_gba():
    reset()
    W, H, D = 57.0, 35.0, 7.5
    top, fy, by = H / 2, -D / 2, D / 2
    body = extrude("shell", rrect(W, H - 6.0, [0.0, 0.0, 1.0, 1.0], cy=-3.0), D, "XZ", -D / 2, bevel=0.5, bevel_seg=2)
    band = [(-29.25, top - 7.6 - 1.2), (-28.5, top - 7.6 - 0.2)]
    for i in range(1, 12):
        x = -28.5 + 57.0 * i / 12
        band.append((x, top - 7.6 + 0.6 * math.cos(math.pi * x / 57.0)))
    band += [(28.5, top - 7.6 - 0.2), (29.25, top - 7.6 - 1.2)]
    band += arc(29.25 - 2.75, top - 2.75, 2.75, 0, 90, 4)
    band += arc(-29.25 + 2.75, top - 2.75, 2.75, 90, 180, 4)
    tb = extrude("band", band, D, "XZ", -D / 2, bevel=0.5, bevel_seg=2)
    union(body, tb)
    recess(body, rrect(43.8, 22.4, 1.0, cy=top - 8.4 - 11.2), 0.4, fy)
    recess(body, [(-5.2, top - 32.0), (5.2, top - 32.0), (0.0, top - 33.8)], 0.4, fy)
    boolean(body, box("notch", 3.0, 3.2, 3.0, loc=(W / 2 - 1.5, by - 1.2, -top - 0.5)))
    boolean(body, box("conn", 48.0, 2.5, 4.0, loc=(0.0, 0.0, -top - 1.0)))
    part(body, rgba=hexc("#4c5052"), gloss=14, spec=0.1)
    for i in range(4):
        o = box("ridge", 40.0, 0.5, 0.6, loc=(0.0, fy - 0.15, top - 3.2 - i * 1.2), r=0.25)
        part(o, rgba=hexc("#4c5052"), gloss=14, spec=0.1)
    o = cyl("screw", 3.0, 0.4, loc=(0.0, by - 0.25, -6.0), axis="Y", n=16)
    part(o, rgba=hexc("#707478"), gloss=60, spec=0.5)
    o = extrude("label", rrect(43.0, 21.6, 0.8, cy=top - 8.4 - 11.2), 0.2, "XZ", fy + 0.25)
    part(o, "label", (1, 1, 1, 1), gloss=18, spec=0.12)
    return "cart_gba"


# ---- HuCard: ref/pce.md 6 ---------------------------------------------------------------
# 54 x 85 x 2, corners r2.5; the contacts along the top end (it goes in that end first),
# the black chip area under them on the back, grip ridges at the far end.
def hucard():
    reset()
    W, H, D = 54.0, 85.0, 2.0
    top, fy, by = H / 2, -D / 2, D / 2
    card = extrude("card", rrect(W, H, 2.5), D, "XZ", -D / 2, bevel=0.3, bevel_seg=2)
    part(card, rgba=hexc("#f2f1ec"), gloss=30, spec=0.2)
    o = extrude("chip", rrect(51.5, 31.0, [0.5, 0.5, 3.0, 3.0], cy=top - 15.5), 0.2, "XZ", by - 0.05)
    part(o, rgba=hexc("#111111"), gloss=120, spec=0.7)
    for i in range(38):
        o = box("contact", 1.0, 0.25, 4.9, loc=(-25.0 + i * (50.5 / 37), by + 0.1, top - 1.7 - 4.9))
        part(o, rgba=hexc("#c9a24e"), gloss=90, spec=0.8)
    for i in range(5):
        o = box("grip", 16.0 - i * 1.5, 0.3, 0.5, loc=(0.0, by + 0.1, -top + 1.2 + i * 1.0))
        part(o, rgba=hexc("#e6e5df"), gloss=20, spec=0.1)
    o = extrude("label", rrect(50.0, 75.0, 1.5, cy=-2.5), 0.15, "XZ", fy - 0.1)
    part(o, "label", (1, 1, 1, 1), gloss=40, spec=0.2)
    return "hucard"


BUILD = {"cart_gb": cart_gb, "cart_nes": cart_nes, "cart_snes": cart_snes, "cart_gba": cart_gba, "hucard": hucard}

if __name__ == "__main__":
    os.makedirs(PREV, exist_ok=True)
    for k in (ARGS[2:] or list(BUILD)):
        key = BUILD[k]()
        export(key, OUT)
        preview(key, PREV, views=("34", "front", "back"))
