"""Game media: PS1 jewel case, DVD keep case, UMD keep case, UMD shell.
blender -b -P tools/models/media.py -- <assets/models dir> [<preview dir>]

References: tools/models/ref/media.md (ECMA-130 disc, ECMA-365 UMD outline, case sizes).
Blender axes: X right, front towards -Y, Z up; millimetres. The discs themselves are
drawn by Lumen (they spin and fly), at the positions noted per model below.
"""
import math, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lmdl import *

ARGS = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
OUT = ARGS[0] if ARGS else os.path.join(os.path.dirname(__file__), "..", "..", "assets", "models")
PREV = ARGS[1] if len(ARGS) > 1 else os.path.join(os.path.dirname(__file__), "build")

CLEAR = (0.90, 0.94, 1.00, 0.16)      # water-clear polystyrene
CLEAR_PP = (0.86, 0.90, 0.95, 0.30)   # clear polypropylene (PSP keep case)
SLEEVE = (0.95, 0.97, 1.00, 0.10)     # polyethylene sleeve
PAPER = hexc("#e9e7e1")


def lid_part(o, kind="glass", rgba=CLEAR, pivot=(0, 0, 0), amount=-112.0, **kw):
    return part(o, kind, rgba, anim="rot", chan="open", pivot=pivot, axis=(0, 0, 1), amount=amount, **kw)


# ---- PS1 jewel case: 142 x 125 x 10.4 ------------------------------------------------
# Disc (drawn by Lumen): centre x = +5.5, z = 0, y = -0.7 (Lumen z = +0.7 mm).
def jewel():
    reset()
    W, H, D = 142.0, 125.0, 10.4
    hw, hh, hd = W / 2, H / 2, D / 2
    tray_c = 5.5
    # base: back plate, top / bottom walls, the hollow clear spine on the left
    o = extrude("base_back", rrect(W, H, 0.8), 1.2, "XZ", hd - 1.2, bevel=0.3, bevel_seg=2)
    part(o, "glass", CLEAR, gloss=90, spec=0.9)
    for s in (1, -1):
        o = box(f"base_wall_{s}", W - 7.0, D - 1.6, 1.1, loc=(3.5, 0.8, s * (hh - 0.55) - 0.55), bevel=0.25, bevel_seg=2)
        part(o, "glass", CLEAR, gloss=90, spec=0.9)
    for x in (-hw + 0.5, -hw + 6.5):
        o = box(f"spine_wall_{x:.0f}", 1.0, D, H, loc=(x, 0, -hh), bevel=0.3, bevel_seg=2)
        part(o, "glass", CLEAR, gloss=90, spec=0.9)
    # paper: back inlay (seen through the back) and its spine flap (seen through the spine)
    o = box("inlay_back", 137.0, 0.25, 118.0, loc=(3.0, hd - 1.35, -59.0))
    part(o, "back", PAPER, gloss=8, spec=0.05)
    o = box("inlay_spine", 0.25, D - 2.6, 118.0, loc=(-hw + 1.2, 0.1, -59.0))
    part(o, "spine", hexc("#111111"), gloss=8, spec=0.05)
    # black tray: disc well, finger notches, support ring, the hub rosette
    tray = box("tray", 134.0, 5.5, 119.0, loc=(3.5, 1.25, -59.5), r=0.8, bevel=0.5, bevel_seg=2)
    boolean(tray, cyl("well", 121.0, 1.6, loc=(tray_c, -1.6, 0), axis="Y", n=96))
    for nx in (tray_c - 60.0, tray_c + 60.0):
        boolean(tray, cyl("notch", 24.0, 3.6, loc=(nx, -1.6, 0), axis="Y", n=48))
    part(tray, "plastic", hexc("#0d0d10"), gloss=48, spec=0.35)
    o = cyl("support_ring", 36.0, 0.6, loc=(tray_c, -0.7, 0), axis="Y", bevel=0.2, n=64)
    boolean(o, cyl("ring_hole", 30.0, 1.0, loc=(tray_c, -0.9, 0), axis="Y", n=48))
    part(o, "plastic", hexc("#141418"), gloss=48, spec=0.35)
    for i in range(10):
        t = box(f"tooth_{i}", 2.6, 3.4, 2.2, loc=(tray_c, -1.8, 5.4), bevel=0.4, bevel_seg=2)
        rotate(t, "Y", i * 36.0, pivot=(tray_c, 0, 0))
        part(t, "plastic", hexc("#18181c"), gloss=48, spec=0.35)
    o = cyl("hub_button", 7.0, 2.3, loc=(tray_c, -2.4, 0), axis="Y", bevel=0.8, n=32)
    part(o, "plastic", hexc("#18181c"), gloss=48, spec=0.35)
    # lid: front plate with lips, hinge knuckles, booklet tabs, the booklet
    piv = (-hw + 1.2, -hd + 1.2, 0)
    o = extrude("lid_front", rrect(W, H, 0.8), 1.2, "XZ", -hd, bevel=0.3, bevel_seg=2)
    lid_part(o, pivot=piv, gloss=90, spec=0.9)
    for s in (1, -1):
        o = box(f"lid_lip_{s}", W - 4.0, 2.4, 1.0, loc=(1.0, -hd + 1.2 + 1.2, s * (hh - 0.5) - 0.5), bevel=0.2, bevel_seg=2)
        lid_part(o, pivot=piv, gloss=90, spec=0.9)
        o = cyl(f"knuckle_{s}", 3.4, 7.0, loc=(-hw + 1.4, -hd + 1.6, s * (hh - 4.0) - 3.5), axis="Z", bevel=0.4, n=24)
        lid_part(o, pivot=piv, gloss=90, spec=0.9)
        o = box(f"tab_{s}", 3.0, 1.0, 9.0, loc=(hw - 3.0, -hd + 1.7, s * 50.0 - 4.5), bevel=0.2, bevel_seg=2)
        lid_part(o, pivot=piv, gloss=90, spec=0.9)
    o = box("booklet", 120.0, 0.5, 120.0, loc=(tray_c, -hd + 1.5, -60.0))
    lid_part(o, "label", (1, 1, 1, 1), pivot=piv, gloss=10, spec=0.08)
    return "case_jewel"


# ---- keep cases (DVD 135 x 190 x 14, UMD 104 x 177 x 14) -------------------------------
def keep_case(key, W, H, D, shell, inner_hub=True, umd_pocket=None):
    """umd_pocket: (cx, cz) centre of the UMD (disc centre) on the back panel."""
    reset()
    hw, hh, hd = W / 2, H / 2, D / 2
    t = 1.4
    glass = shell[3] < 1.0
    kind = "glass" if glass else "plastic"
    gl, sp = (70, 0.7) if glass else (18, 0.2)
    piv = (-hw, -hd, 0)
    # back panel, walls, spine
    o = extrude("back_panel", rrect(W, H, 5.0), t, "XZ", hd - t, bevel=1.0, bevel_seg=3)
    part(o, kind, shell, gloss=gl, spec=sp)
    for s in (1, -1):
        o = box(f"wall_{s}", W - 8.0, D - 2 * t, t, loc=(2.0, 0, s * (hh - t / 2 - 0.6) - t / 2), bevel=0.4, bevel_seg=2)
        part(o, kind, shell, gloss=gl, spec=sp)
    o = box("wall_open", t, D - 2 * t, H - 10.0, loc=(hw - t / 2 - 0.6, 0, -hh + 5.0), bevel=0.4, bevel_seg=2)
    part(o, kind, shell, gloss=gl, spec=sp)
    o = box("spine", t, D, H - 2.0, loc=(-hw + t / 2, 0, -hh + 1.0), bevel=0.6, bevel_seg=3)
    part(o, kind, shell, gloss=gl, spec=sp)
    # inside the back panel: the disc's hub and well ridge, or the UMD pocket
    if inner_hub:
        cx = (-hw + D + hw) / 2   # centre of the back panel beside the spine
        ring = cyl("well_ridge", 124.0, 0.8, loc=(cx, hd - t - 0.8, 0), axis="Y", n=128)
        boolean(ring, cyl("well_in", 121.0, 1.2, loc=(cx, hd - t - 1.0, 0), axis="Y", n=128))
        part(ring, kind, shell, gloss=gl, spec=sp)
        o = cyl("hub_ring", 38.0, 1.0, loc=(cx, hd - t - 1.0, 0), axis="Y", bevel=0.3, n=64)
        part(o, kind, shell, gloss=gl, spec=sp)
        for i in range(12):
            tt = box(f"tooth_{i}", 2.4, 3.6, 2.0, loc=(cx, hd - t - 4.6, 5.6), bevel=0.4, bevel_seg=2)
            rotate(tt, "Y", i * 30.0, pivot=(cx, 0, 0))
            part(tt, kind, shell, gloss=gl, spec=sp)
        o = cyl("hub_button", 9.0, 3.2, loc=(cx, hd - t - 3.2, 0), axis="Y", bevel=1.0, n=32)
        part(o, kind, shell, gloss=gl, spec=sp)
    if umd_pocket:
        cx, cz = umd_pocket
        outer = extrude("pocket", [(x * 1.06, z * 1.06) for x, z in umd_outline()], 1.6, "XZ", hd - t - 1.6)
        move(outer, cx, 0, cz)
        boolean(outer, move(extrude("pocket_in", [(x * 1.015, z * 1.015) for x, z in umd_outline()], 2.4, "XZ", hd - t - 2.0), cx, 0, cz))
        part(outer, kind, shell, gloss=gl, spec=sp)
        o = box("umd_clip", 14.0, 4.0, 2.4, loc=(cx, hd - t - 2.0, cz + 33.5), bevel=0.5, bevel_seg=2)
        part(o, kind, shell, gloss=gl, spec=sp)
    # front cover (opens about the front crease), its booklet clips
    o = extrude("front_panel", rrect(W, H, 5.0), t, "XZ", -hd, bevel=1.0, bevel_seg=3)
    lid_part(o, kind, shell, pivot=piv, amount=-165.0, gloss=gl, spec=sp)
    for s in (1, -1):
        o = box(f"clip_{s}", 8.0, 1.2, 3.0, loc=(-hw + D + 6.0, -hd + t + 0.6, s * (hh - 18.0) - 1.5), bevel=0.3, bevel_seg=2)
        lid_part(o, kind, shell, pivot=piv, amount=-165.0, gloss=gl, spec=sp)
    # printed wrap (front art, back art, spine) under the clear sleeve
    o = extrude("art_front", rrect(W - 5.5, H - 6.0, 2.0, cx=2.0), 0.15, "XZ", -hd - 0.2)
    lid_part(o, "label", (1, 1, 1, 1), pivot=piv, amount=-165.0, gloss=10, spec=0.08)
    o = extrude("art_back", rrect(W - 5.5, H - 6.0, 2.0, cx=2.0), 0.15, "XZ", hd + 0.05)
    part(o, "back", PAPER, gloss=10, spec=0.08)
    o = box("art_spine", 0.15, D - 1.0, H - 6.0, loc=(-hw - 0.15, 0, -hh + 3.0))
    part(o, "spine", hexc("#111111"), gloss=10, spec=0.08)
    o = extrude("sleeve_front", rrect(W + 0.8, H + 0.8, 5.4), 0.25, "XZ", -hd - 0.5)
    lid_part(o, "glass", SLEEVE, pivot=piv, amount=-165.0, gloss=120, spec=0.8)
    o = extrude("sleeve_back", rrect(W + 0.8, H + 0.8, 5.4), 0.25, "XZ", hd + 0.25)
    part(o, "glass", SLEEVE, gloss=120, spec=0.8)
    return key


# ---- UMD shell (ECMA-365): 64 x 65 x 4.2, origin at the disc centre --------------------
def umd_outline(n=40):
    pts = []
    for i in range(n + 1):                      # round end: R32 semicircle, concentric
        a = math.pi * i / n
        pts.append((32.0 * math.cos(a), 32.0 * math.sin(a)))
    pts.append((-32.0, -20.5))                  # left side, into the R5 corner
    for i in range(1, 6):                       # corner blend into the R70 end
        t = i / 6.0
        p0, c, p1 = (-32.0, -20.5), (-32.0, -26.9), (-27.0, 37.0 - math.sqrt(70 ** 2 - 27.0 ** 2))
        pts.append(((1 - t) ** 2 * p0[0] + 2 * (1 - t) * t * c[0] + t * t * p1[0],
                    (1 - t) ** 2 * p0[1] + 2 * (1 - t) * t * c[1] + t * t * p1[1]))
    for i in range(0, 25):                      # flat end: convex R70 arc, centre (0, +37)
        x = -27.0 + 54.0 * i / 24
        pts.append((x, 37.0 - math.sqrt(70 ** 2 - x * x)))
    for i in range(1, 6):
        t = i / 6.0
        p0, c, p1 = (27.0, 37.0 - math.sqrt(70 ** 2 - 27.0 ** 2)), (32.0, -26.9), (32.0, -20.5)
        pts.append(((1 - t) ** 2 * p0[0] + 2 * (1 - t) * t * c[0] + t * t * p1[0],
                    (1 - t) ** 2 * p0[1] + 2 * (1 - t) * t * c[1] + t * t * p1[1]))
    pts.append((32.0, -20.5))
    return pts


def umd():
    reset()
    T = 4.2
    shell = extrude("shell", umd_outline(), T, "XZ", -T / 2, bevel=0.3, bevel_seg=2)
    # the front window over the disc label and the back's spindle window, laser slot, holes
    boolean(shell, cyl("win", 56.0, 1.0, loc=(0, -T / 2 - 0.1, 0), axis="Y", n=96))
    boolean(shell, cyl("spindle", 18.0, 1.2, loc=(0, T / 2 - 1.1, 0), axis="Y", n=48))
    boolean(shell, box("laser", 21.0, 1.2, 18.4, loc=(-21.0, T / 2 - 0.55, -9.2)))
    for x in (20.5, -20.5):
        boolean(shell, cyl("hole", 3.6, 3.0, loc=(x, T / 2 - 2.9, -26.3), axis="Y", n=24))
    part(shell, "glass", (0.94, 0.94, 0.96, 0.55), gloss=60, spec=0.6)
    o = cyl("window", 56.0, 0.6, loc=(0, -T / 2 + 0.3, 0), axis="Y", n=96)
    part(o, "glass", (0.95, 0.97, 1.0, 0.12), gloss=120, spec=0.9)
    # the opaque printed band at the flat end
    band = extrude("band", [(x, z) for x, z in umd_outline() if z < -18.0] + [(26.0, -18.0), (-26.0, -18.0)][::1], 0.2, "XZ", -T / 2 - 0.15)
    part(band, "plastic", hexc("#ececee"), gloss=24, spec=0.2)
    # the magnetic clamping plate seen through the back
    o = cyl("clamp", 10.5, 0.3, loc=(0, 0.45, 0), axis="Y", bevel=0.1, n=40)
    part(o, "plastic", hexc("#b8bcc2"), gloss=80, spec=0.8)
    return "umd"


BUILD = {
    "case_jewel": jewel,
    "case_dvd": lambda: keep_case("case_dvd", 135.0, 190.0, 14.0, hexc("#111114")),
    "case_umd": lambda: keep_case("case_umd", 104.0, 177.0, 14.0, CLEAR_PP, inner_hub=False, umd_pocket=(7.0, 20.0)),
    "umd": umd,
}

if __name__ == "__main__":
    only = ARGS[2:] if len(ARGS) > 2 else list(BUILD)
    os.makedirs(PREV, exist_ok=True)
    for k in only:
        key = BUILD[k]()
        export(key, OUT)
        preview(key, PREV, views=("34", "front", "back"))
        pose({"open": 1.0})
        preview(key + "-open", PREV, views=("34", "front"))
