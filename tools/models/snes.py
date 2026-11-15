"""Super Famicom (SHVC-001) / PAL Super Nintendo (same shell) and its cartridge.
blender -b -P tools/models/snes.py -- <assets/models dir> <preview dir>

References: tools/models/ref/snes.md (200 x 242 x 72 mm, top views measured from two
near-orthographic photos), checked against the Commons Evan-Amos studio photo and the
orthographic cartridge scan. No logos or lettering (the four-dot logo, the wordmark, the
slot and panel legends, the port numbers).
Blender axes: X right, front towards -Y, Z up, mm. Built in the sheet's frame (origin on
the table at the plan centre; Blender y = -sheet z), then moved so the centre of the slot
opening at the deck is the origin (Lumen's top-loader frame).
"""
import math, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lmdl import *

ARGS = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
OUT, PREV = ARGS[0], ARGS[1]

BODY = hexc("#d2d2cf")
DECK_C = hexc("#a0a1a2")
RECESS = hexc("#868686")
EJECT_C = hexc("#aaaeac")
DARKBTN = hexc("#686c6d")
PINS = hexc("#a8a8a8")
DARK = hexc("#3c3d3e")

SLOT_Y = 25.0        # slot centre (Blender y; sheet z = -25)
DECK = 72.0


def plan(w, d, rr, rf):
    """Rounded rectangle, rear corners rr, front corners rf (Blender plan: +y = rear)."""
    return rrect(w, d, [rr, rr, rf, rf])


def console():
    reset()
    # bottom shell (tray), inset 2 mm, and the feet
    o = extrude("bottom", plan(196.0, 238.0, 8.0, 12.0), 26.0, "XY", 2.0, bevel=2.0, bevel_seg=2)
    boolean(o, box("rear_bay", 95.0, 14.0, 22.0, loc=(-47.5, 119.0 - 6.0, 4.0), r=2.0))
    part(o, rgba=BODY, gloss=14, spec=0.1)
    o = box("bay_dark", 93.0, 1.0, 20.0, loc=(-47.5, 106.5, 5.0))
    part(o, "dark", hexc("#5a5b5b"))
    for x, y, d, c in ((-72, -100, 15, "#2a2a2a"), (72, -100, 15, "#2a2a2a"), (-75, 80, 20, "#bcbcb9"), (75, 80, 20, "#bcbcb9")):
        o = cyl("foot", d, 2.5, loc=(x, y, 0.0), bevel=0.8, n=24)
        part(o, rgba=hexc(c), gloss=8, spec=0.05)
    # top shell: side profile (vertical front to y 40, the nose, the front band rising ~30
    # degrees to the deck, the flat top, the rear strip rolling down), clipped to the plan
    prof = [(-121.0, 26.0), (-121.0, 40.0)]
    prof += arc(-112.0, 40.0, 9.0, 180, 120, 5)[1:]
    prof += [(-73.0, 71.0), (99.0, 71.0)]
    prof += arc(99.0, 58.0, 13.0, 90, 10, 6)[1:]
    prof += [(121.0, 55.0), (121.0, 26.0)]
    top = extrude("top", [(y, z) for y, z in prof], 210.0, "YZ", -105.0)
    boolean(top, extrude("clip", plan(200.0, 242.0, 10.0, 14.0), 60.0, "XY", 20.0), "INTERSECT")
    md = top.modifiers.new("shoulders", "BEVEL")
    md.width = 6.0; md.segments = 5; md.limit_method = "ANGLE"; md.angle_limit = math.radians(50)
    apply_mods(top)
    # slot recess and opening, rear vents, the seams of the front band
    boolean(top, extrude("slot_rec", rrect(140.0, 23.0, 10.0, cy=SLOT_Y), 10.0, "XY", DECK - 4.0))
    boolean(top, extrude("slot_open", rrect(132.0, 21.0, 3.0, cy=SLOT_Y), 50.0, "XY", 20.0))
    for i in range(22):
        boolean(top, box("vent", 3.0, 8.0, 20.0, loc=(-73.5 + i * 7.0, 121.0, 33.0), r=1.2))
    for sx in (-1, 1):
        g = box("seam", 1.0, 50.0, 40.0, loc=(sx * 80.0, -97.0, 38.0))
        boolean(top, g)
    smooth(top, 40.0)
    part(top, rgba=BODY, gloss=14, spec=0.1)
    # the dark deck panel (top cover), flush in its seam, cut by the slot recess
    deck = extrude("deck", rrect(172.0, 172.0, 4.0, cy=13.0), 1.6, "XY", DECK - 1.5, bevel=0.5, bevel_seg=2)
    boolean(deck, extrude("slot_cut", rrect(146.0, 29.0, 13.0, cy=SLOT_Y), 6.0, "XY", DECK - 3.0))
    part(deck, rgba=DECK_C, gloss=12, spec=0.08)
    # slot: light bevelled rim, the darker recess floor with its tick grooves and arrow,
    # dark depth under the two spring flaps
    rim = extrude("rim", rrect(146.0, 29.0, 13.0, cy=SLOT_Y), 1.5, "XY", DECK - 1.6, bevel=0.6, bevel_seg=2)
    boolean(rim, extrude("rim_in", rrect(140.0, 23.0, 10.0, cy=SLOT_Y), 4.0, "XY", DECK - 3.0))
    part(rim, rgba=hexc("#c4c5c4"), gloss=14, spec=0.1)
    floor = extrude("rec_floor", rrect(140.0, 23.0, 10.0, cy=SLOT_Y), 1.0, "XY", DECK - 4.0)
    boolean(floor, extrude("fl_open", rrect(132.0, 21.0, 3.0, cy=SLOT_Y), 4.0, "XY", DECK - 6.0))
    for x in (-55.0, 55.0):
        boolean(floor, box("tick", 1.0, 3.0, 2.0, loc=(x, SLOT_Y - 10.5, DECK - 3.5)))
    boolean(floor, extrude("arrow", [(-2.5, SLOT_Y - 11.6), (2.5, SLOT_Y - 11.6), (0.0, SLOT_Y - 9.0)], 3.0, "XY", DECK - 4.5))
    part(floor, rgba=RECESS, gloss=10, spec=0.05)
    o = box("slot_dark", 131.0, 20.0, 36.0, loc=(0.0, SLOT_Y, DECK - 44.0), r=2.5)
    part(o, "dark", hexc("#0b0b0c"))
    for s in (1, -1):
        hy = SLOT_Y + s * 10.3
        f = box(f"flap_{s}", 130.0, 10.0, 1.0, loc=(0.0, hy - s * 5.0, DECK - 5.0), r=0.5)
        part(f, rgba=RECESS, gloss=10, spec=0.05, anim="rot", chan="open",
             pivot=(0.0, hy, DECK - 4.5), axis=(1, 0, 0), amount=s * 80.0)
    # POWER: light bezel, dark well, the slider plate with its curled grip (ON = rearward)
    for x, y in ((-53.0, -42.0), (53.0, -40.0)):
        b = extrude("bezel", rrect(32.0, 35.0, 4.0, cx=x, cy=y), 2.5, "XY", DECK - 1.0, bevel=0.8, bevel_seg=2)
        boolean(b, extrude("bez_in", rrect(27.6, 30.6, 2.5, cx=x, cy=y), 6.0, "XY", DECK - 3.0))
        part(b, rgba=BODY, gloss=14, spec=0.1)
        o = extrude("well", rrect(27.6, 30.6, 2.5, cx=x, cy=y), 0.5, "XY", DECK - 2.0)
        part(o, "dark", DARK)
    plate = box("power_plate", 25.0, 20.0, 2.0, loc=(-53.0, -46.0, DECK - 1.8), r=1.5, bevel=0.5, bevel_seg=2)
    lip = extrude("power_lip", [(-50.0, 0.0), (-42.0, 0.0), (-38.0, 5.5), (-41.0, 5.5)], 22.0, "YZ", -64.0, bevel=0.6, bevel_seg=2)
    move(lip, 0.0, 0.0, DECK - 0.5)
    union(plate, lip)
    part(plate, rgba=DARKBTN, gloss=22, spec=0.15, anim="move", chan="power", axis=(0, 1, 0), amount=5.5)
    # EJECT lever: wide rounded block, its front a little higher
    ej = extrude("eject", rrect(58.0, 32.0, 4.0, cy=-42.0), 6.0, "XY", DECK - 1.0, bevel=1.6, bevel_seg=3)
    part(ej, rgba=EJECT_C, gloss=18, spec=0.12)
    # RESET cap
    o = extrude("reset", rrect(24.0, 26.0, 2.0, cx=53.0, cy=-40.0), 3.6, "XY", DECK - 1.6, bevel=0.6, bevel_seg=2)
    part(o, rgba=DARKBTN, gloss=22, spec=0.15)
    # power LED: a red pill on the sloping front band, in its oval surround
    ang = math.degrees(math.atan2(71.0 - 49.5, -73.0 + 106.5))
    for nm, w, h, dz, col, kind in (("led_ring", 13.0, 6.0, 0.0, BODY, "plastic"), ("led", 10.5, 3.8, 0.6, hexc("#6e4a48"), "light")):
        o = extrude(nm, rrect(w, h, h / 2, seg=6), 0.8, "XY", 0.0, bevel=0.3, bevel_seg=2)
        rotate(o, "X", ang)
        move(o, 55.0, -100.0, 55.2 + dz * 0.5)
        if kind == "light":
            part(o, "light", col, on=hexc("#ff3020"), pivot=(55.0, -100.5, 56.0), gloss=120, spec=0.8)
        else:
            part(o, rgba=col, gloss=14, spec=0.1)
    # the controller-port pill on the front, its two sockets (rounded end outwards)
    pill = extrude("port_pill", rrect(148.0, 19.0, 9.5, cy=17.0), 1.6, "XZ", -122.3, bevel=0.5, bevel_seg=2)
    for sx in (-1, 1):
        sock = extrude("sock", rrect(37.0, 13.0, [6.5, 0.5, 0.5, 6.5] if sx < 0 else [0.5, 6.5, 6.5, 0.5], cx=sx * 33.0, cy=17.0),
                       9.0, "XZ", -123.0)
        boolean(pill, sock)
    part(pill, rgba=hexc("#cfcfcb"), gloss=14, spec=0.1)
    for sx in (-1, 1):
        o = extrude("sock_in", rrect(36.0, 12.0, 0.5, cx=sx * 33.0, cy=17.0), 0.5, "XZ", -116.0)
        part(o, "dark", hexc("#1c1c1d"))
        for bx, bw, n in ((sx * 33.0 + sx * 11.0, 11.0, 3), (sx * 33.0 - sx * 5.5, 15.0, 4)):
            blk = box("pins", bw, 3.0, 7.0, loc=(bx, -117.5, 13.5), r=1.0)
            for i in range(n):
                boolean(blk, cyl("hole", 2.5, 4.0, loc=(bx - (n - 1) * 1.75 + i * 3.5, -120.0, 17.0), axis="Y", n=12))
            part(blk, rgba=PINS, gloss=10, spec=0.05)
    translate_all(0.0, -SLOT_Y, -DECK)
    return "console_snes"


if __name__ == "__main__":
    os.makedirs(PREV, exist_ok=True)
    key = console()
    export(key, OUT)
    preview(key, PREV, views=("34", "front", "top", "back"))
    pose({"open": 1.0, "power": 1.0})
    preview(key + "-pose", PREV, views=("34",))
