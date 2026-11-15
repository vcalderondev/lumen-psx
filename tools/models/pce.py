"""NEC PC Engine (PI-TG001, white).
blender -b -P tools/models/pce.py -- <assets/models dir> <preview dir>

References: tools/models/ref/pce.md (140 x 140 x 38 mm; the top layout measured from a
straight-down photo). No logos or lettering ("PC Engine", "NEC", the legends).
Blender axes: X right, front towards -Y, Z up, mm. Built in the sheet's frame (origin on
the table at the plan centre; Blender y = -sheet z), then moved so the top of the central
plateau is at 0 (Lumen's front-loader frame: the HuCard lies label up in the open tray at
the front of the plateau and slides back into the slot).
"""
import math, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lmdl import *

ARGS = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
OUT, PREV = ARGS[0], ARGS[1]

WHITE = hexc("#e8e7e1")
BASE = hexc("#dbdcd6")
GREEN = hexc("#5db07e")
DARK = hexc("#151515")
TOP, PLAT = 36.5, 38.3
FLOOR = PLAT - 4.0


def tombstone(w, rear_c, front):
    """The plateau's plan: straight sides, a semicircular rear (Blender: rear = +y)."""
    r = w / 2
    return [(r, front)] + arc(0.0, rear_c, r, 0, 180, 24) + [(-r, front)]


def console():
    reset()
    lower = box("base", 139.0, 139.0, 12.5, loc=(0, 0, 0), r=4.0, bevel=1.2, bevel_seg=2)
    boolean(lower, box("exp", 76.0, 12.0, 9.0, loc=(0.0, 70.0, 3.0), r=1.0))
    part(lower, rgba=BASE, gloss=12, spec=0.08)
    o = box("exp_in", 74.0, 1.0, 7.0, loc=(0.0, 64.5, 4.0))
    part(o, rgba=hexc("#c8873c"), gloss=70, spec=0.6)
    top = box("cover", 140.0, 140.0, TOP - 12.5, loc=(0, 0, 12.5), r=4.0, bevel=2.5, bevel_seg=3)
    # vents (five grooves each side, front corners), the controller port, the jacks
    for sx in (-1, 1):
        for i in range(5):
            boolean(top, box("vent", 27.2, 2.0, 4.0, loc=(sx * 52.8, -(31.8 + 2.0 + i * 4.2), TOP - 2.0), r=1.0))
    boolean(top, cyl("port", 13.0, 6.0, loc=(48.0, -74.0, 10.0 + 0.0), axis="Y", n=32))
    boolean(top, box("pow_track", 18.0, 4.0, 6.0, loc=(-50.0, -70.0, 10.0), r=1.0))
    boolean(top, cyl("dc", 8.0, 6.0, loc=(-74.0, -16.0, 20.0), axis="X", n=24))
    boolean(top, cyl("rf", 7.0, 6.0, loc=(70.0, -20.0, 20.0), axis="X", n=24))
    part(top, rgba=WHITE, gloss=12, spec=0.08)
    # label bosses either side (left: AC ADAPTER, right: ANT SWITCH; no text)
    for sx in (-1, 1):
        o = extrude("boss", rrect(17.0, 30.0, 3.0, cx=sx * 57.5, cy=16.0), 1.0, "XY", TOP - 0.2, bevel=0.4, bevel_seg=2)
        part(o, rgba=WHITE, gloss=12, spec=0.08)
    # the central "tombstone" plateau, its HuCard tray and finger scoop
    plat = extrude("plateau", tombstone(78.7, 20.7, -70.0), PLAT - TOP + 0.2, "XY", TOP - 0.2, bevel=0.8, bevel_seg=2)
    boolean(plat, box("tray", 56.8, 61.0, 10.0, loc=(0.0, -(10.6 + 30.5), FLOOR)))
    part(plat, rgba=WHITE, gloss=12, spec=0.08)
    boolean(top, box("tray", 56.8, 61.0, 10.0, loc=(0.0, -(10.6 + 30.5), FLOOR)))
    scoop = extrude("scoop", [(16.0, -80.0)] + arc(0.0, -54.9, 16.0, 0, 180, 16)[0:0] + arc(0.0, -54.9, 16.0, 0, 180, 16) + [(-16.0, -80.0)],
                    30.0, "XY", 8.0)
    boolean(top, scoop)
    # the tray floor: fine fore-aft ribs and an arrow pointing back; the slot mouth behind
    floor = box("tray_floor", 56.6, 59.0, 0.8, loc=(0.0, -(10.6 + 29.5), FLOOR - 0.8))
    boolean(floor, extrude("scoop_f", arc(0.0, -54.9, 16.2, 0, 180, 16) + [(-16.2, -80.0), (16.2, -80.0)], 4.0, "XY", FLOOR - 2.0))
    part(floor, rgba=hexc("#d6d5cf"), gloss=10, spec=0.06)
    for i in range(9):
        x = -24.0 + i * 6.0
        if abs(x) < 17:
            continue
        o = box("rib", 0.8, 40.0, 0.5, loc=(x, -35.0, FLOOR))
        part(o, rgba=hexc("#d6d5cf"), gloss=10, spec=0.06)
    o = extrude("arrow", [(-5.0, -26.0), (5.0, -26.0), (0.0, -18.0)], 0.4, "XY", FLOOR)
    part(o, rgba=hexc("#cfcec8"), gloss=10, spec=0.06)
    o = box("slot", 56.0, 30.0, 3.4, loc=(0.0, -10.6 + 15.0 - 0.01, FLOOR))
    part(o, "dark", DARK)
    # the card guard (moves over the card when the power slides on)
    g = box("guard", 9.0, 5.0, 2.2, loc=(-23.5, -64.5, FLOOR + 1.6), r=0.8)
    union(g, box("guard_l", 3.0, 5.0, 4.0, loc=(-27.0, -64.5, FLOOR)))
    part(g, rgba=DARK, gloss=20, spec=0.1, anim="move", chan="power", axis=(1, 0, 0), amount=7.0)
    # power slider (green, front left; to the right = ON), the controller port
    k = box("power", 8.0, 4.0, 10.0, loc=(-54.0, -71.5, 5.0), r=1.5, bevel=0.6, bevel_seg=2)
    for i in range(3):
        union(k, box("grip", 0.8, 0.8, 8.0, loc=(-56.0 + i * 2.0, -73.6, 6.0)))
    part(k, rgba=GREEN, gloss=16, spec=0.1, anim="move", chan="power", axis=(1, 0, 0), amount=8.0)
    o = cyl("port_in", 9.5, 1.0, loc=(48.0, -69.5, 10.0), axis="Y", n=28)
    part(o, "dark", hexc("#2e2e2c"))
    o = cyl("dc_in", 5.0, 1.0, loc=(-69.0, -16.0, 20.0), axis="X", n=20)
    part(o, "dark", hexc("#2e2e2c"))
    o = cyl("rf_in", 4.0, 1.0, loc=(68.5, -20.0, 20.0), axis="X", n=20)
    part(o, rgba=hexc("#b8b8b0"), gloss=60, spec=0.5)
    translate_all(0.0, 0.0, -PLAT)
    return "console_pce"


if __name__ == "__main__":
    os.makedirs(PREV, exist_ok=True)
    key = console()
    export(key, OUT)
    preview(key, PREV, views=("34", "front", "top", "back"))
    pose({"power": 1.0})
    preview(key + "-pose", PREV, views=("34",))
