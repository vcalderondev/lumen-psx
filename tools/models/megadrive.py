"""Sega Mega Drive (model 1, Japanese trim) and the Mega Drive cartridge (PAL / Genesis
shell, 109 x 70 x 17).
blender -b -P tools/models/megadrive.py -- <assets/models dir> <preview dir>

References: tools/models/ref/megadrive.md (published sizes, photo-measured layout), checked
against the Commons JP model 1 studio photo; the kaveshkoli3 Genesis model (CC BY, see
CREDITS.md) was used only to compare proportions. No logos or lettering ("16-BIT", "MEGA
DRIVE", "SEGA", the ring texts, the panel legends).
Blender axes: X right, front towards -Y, Z up, mm. Built in the sheet's frame (origin on
the table at the plan centre; Blender y = -sheet z), then moved so the centre of the slot
opening at the disc top is the origin (Lumen's top-loader frame).
"""
import math, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lmdl import *

ARGS = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
OUT, PREV = ARGS[0], ARGS[1]

BODY = hexc("#1c1c1e")
GLOSS = hexc("#0e0e0e")
RING = hexc("#1a1a1a")
MAGENTA = hexc("#8e1f4f")
RESET_BLUE = hexc("#0062ae")
RED = hexc("#d0202a")
DARK = hexc("#0a0a0a")

DX, DY = 35.0, 4.0           # disc centre (Blender x, y)
SY = 34.0                    # slot centre (Blender y): rear half of the disc
DECK, RING_TOP, CAP_TOP = 47.0, 50.0, 58.0


def plan_body():
    """280 x 212, rear corners R6, both front corners cut at 45 degrees (18 mm)."""
    hw, hd, c = 140.0, 106.0, 18.0
    pts = [(-hw + c, -hd), (hw - c, -hd), (hw, -hd + c)]
    pts += arc(hw - 6.0, hd - 6.0, 6.0, 0, 90, 4)
    pts += arc(-hw + 6.0, hd - 6.0, 6.0, 90, 180, 4)
    pts += [(-hw, -hd + c)]
    return pts


def console():
    reset()
    # base plinth, set back 3 mm all round (the shadow line)
    o = extrude("plinth", offset(plan_body(), -3.0), 6.0, "XY", 0.0, bevel=1.0, bevel_seg=2)
    part(o, rgba=hexc("#141416"), gloss=10, spec=0.05)
    # lower front band (holds the controller ports): 4 mm behind the upper front
    lower = extrude("lower", offset(plan_body(), -0.0), 16.0, "XY", 6.0)
    boolean(lower, box("band_set", 300.0, 4.0 + 1.0, 18.0, loc=(0, -106.0 + 2.0 - 0.5, 5.0)))
    # upper shell: side profile with the front sloping back 15 degrees and a large round
    # front edge into the deck, which rises from 42 at the front to 47 at the back
    prof = [(-106.0, 22.0), (-101.2, 40.0)]
    prof += arc(-92.0, 33.5, 9.0, 160, 95, 6)[1:]
    prof += [(106.0, DECK), (106.0, 22.0)]
    upper = extrude("upper", [(y, z) for y, z in prof], 290.0, "YZ", -145.0)
    clip = extrude("clip", plan_body(), 60.0, "XY", 20.0)
    boolean(upper, clip, "INTERSECT")
    union(upper, lower)
    # the fine groove across the sloped front, the headphone socket
    boolean(upper, box("groove", 300.0, 2.0, 0.8, loc=(0, -103.0, 29.6)))
    boolean(upper, cyl("phones", 6.0, 8.0, loc=(-100.0, -110.0, 30.5), axis="Y", n=24))
    # control panel recess (front left of the deck)
    boolean(upper, box("panel", 70.0, 48.0, 10.0, loc=(-84.0, -76.0, 39.0), r=2.0))
    # controller ports on the lower band: trapezoid sockets
    for x in (28.0, 63.0):
        s = extrude("port", [(-11.0, 6.0), (11.0, 6.0), (9.0, -6.0), (-9.0, -6.0)], 9.0, "XZ", -103.0)
        move(s, x, 0.0, 14.0)
        boolean(upper, s)
    # Mega-CD expansion cover on the right side, rear panel recesses
    boolean(upper, box("exp", 3.0, 70.0, 20.0, loc=(140.0, 25.0, 6.0), r=1.0))
    boolean(upper, box("rear_io", 120.0, 4.0, 14.0, loc=(20.0, 106.0, 12.0), r=2.0))
    part(upper, rgba=BODY, gloss=12, spec=0.1)
    # inside the sockets and the expansion recess
    for x in (28.0, 63.0):
        o = extrude("port_in", [(-9.5, 5.0), (9.5, 5.0), (7.8, -5.0), (-7.8, -5.0)], 0.5, "XZ", -94.5)
        move(o, x, 0.0, 14.0)
        part(o, "dark", DARK)
        for i in range(5):
            o = cyl("pin", 1.2, 0.6, loc=(x - 6.0 + i * 3.0, -95.2, 15.8), axis="Y", n=10)
            part(o, rgba=hexc("#8a8a8a"), gloss=60, spec=0.5)
        for i in range(4):
            o = cyl("pin", 1.2, 0.6, loc=(x - 4.5 + i * 3.0, -95.2, 12.4), axis="Y", n=10)
            part(o, rgba=hexc("#8a8a8a"), gloss=60, spec=0.5)
    o = cyl("phones_in", 3.5, 0.4, loc=(-100.0, -101.0, 30.5), axis="Y", n=20)
    part(o, "dark", DARK)
    # vent block (rear left), 13 slats across
    vent = box("vent", 40.0, 120.0, 5.0, loc=(-100.0, 20.0, DECK - 1.0), r=2.0, bevel=0.8, bevel_seg=2)
    for i in range(13):
        boolean(vent, box("slot", 32.0, 4.0, 8.0, loc=(-100.0, 20.0 - 48.0 + i * 8.0, DECK - 0.5), r=1.0))
    part(vent, rgba=BODY, gloss=12, spec=0.1)
    o = box("vent_dark", 34.0, 106.0, 1.0, loc=(-100.0, 20.0, DECK - 0.6))
    part(o, "dark", DARK)
    # the disc: plinth ring with its chamfer, the glossy cap, the slot with its flaps
    ring = cyl("ring", 160.0, RING_TOP - DECK + 1.0, loc=(DX, DY, DECK - 1.0), bevel=2.5, n=128)
    part(ring, rgba=RING, gloss=30, spec=0.25)
    cap = cyl("cap", 140.0, CAP_TOP - RING_TOP + 0.5, loc=(DX, DY, RING_TOP - 0.5), bevel=4.0, n=128)
    boolean(cap, extrude("slot", rrect(114.0, 22.0, 4.0, cx=DX, cy=SY), 30.0, "XY", CAP_TOP - 25.0))
    part(cap, rgba=GLOSS, gloss=110, spec=0.7)
    o = box("slot_dark", 113.0, 21.0, 25.0, loc=(DX, SY, CAP_TOP - 31.0), r=3.5)
    part(o, "dark", hexc("#050505"))
    for s in (1, -1):
        hy = SY + s * 10.6
        f = box(f"flap_{s}", 112.0, 10.2, 1.0, loc=(DX, hy - s * 5.1, CAP_TOP - 4.0), r=0.6)
        part(f, rgba=hexc("#101011"), gloss=20, spec=0.1, anim="rot", chan="open",
             pivot=(DX, hy, CAP_TOP - 3.5), axis=(1, 0, 0), amount=s * 80.0)
    # the plate in front of the slot (no "16-BIT"), down to the magenta band on the ring
    front_y = DY - 80.0
    plate = extrude("plate", [(DX - 20.0, SY - 13.0), (DX + 20.0, SY - 13.0), (DX + 27.5, front_y + 12.0), (DX - 27.5, front_y + 12.0)],
                    0.6, "XY", CAP_TOP - 0.3)
    boolean(plate, cyl("capclip", 136.0, 4.0, loc=(DX, DY, CAP_TOP - 2.0), n=128), "INTERSECT")
    part(plate, rgba=hexc("#060606"), gloss=160, spec=0.9)
    band = extrude("band", [(DX - 27.5, front_y + 12.0), (DX + 27.5, front_y + 12.0), (DX + 28.5, front_y), (DX - 28.5, front_y)],
                   0.4, "XY", RING_TOP - 0.1)
    ring_in = cyl("ring_in", 143.0, 4.0, loc=(DX, DY, RING_TOP - 2.0), n=128)
    boolean(band, ring_in)
    boolean(band, cyl("ring_out", 159.0, 4.0, loc=(DX, DY, RING_TOP - 2.0), n=128), "INTERSECT")
    part(band, rgba=MAGENTA, gloss=60, spec=0.4)
    led = cyl("led", 3.0, 0.8, loc=(DX - 3.0, front_y + 5.0, RING_TOP + 0.1), bevel=0.3, n=16)
    part(led, "light", hexc("#3a1214"), on=hexc("#ff2a1a"), pivot=(DX - 3.0, front_y + 5.0, RING_TOP + 0.5), gloss=120, spec=0.8)
    # the fairings at the front right / left of the ring, dying into the deck
    for side, ang in ((1, -35.0), (-1, -145.0)):
        a = math.radians(ang)
        w = extrude("fairing", [(-2.0, -8.0), (38.0, -2.0), (38.0, 2.0), (-2.0, 8.0)], 3.5, "XY", DECK - 1.0, bevel=1.6, bevel_seg=3)
        rotate(w, "Z", ang)
        move(w, DX + 79.0 * math.cos(a), DY + 79.0 * math.sin(a), 0.0)
        part(w, rgba=BODY, gloss=12, spec=0.1)
    # control panel: volume slide (front-back), power slide (left-right, red mark), reset
    o = box("vol_track", 8.0, 32.0, 1.0, loc=(-108.0, -76.0, 39.0))
    part(o, "dark", DARK)
    knob = box("vol_knob", 10.0, 12.0, 8.0, loc=(-108.0, -70.0, 39.0), r=1.5, bevel=0.8, bevel_seg=2)
    for i in range(4):
        union(knob, box("rib", 10.4, 1.0, 0.8, loc=(-108.0, -74.5 + i * 3.0, 46.8)))
    part(knob, rgba=hexc("#141414"), gloss=20, spec=0.15)
    o = box("pow_track", 28.0, 10.0, 1.0, loc=(-72.0, -64.0, 39.0))
    part(o, "dark", DARK)
    knob = box("pow_knob", 10.0, 8.0, 7.0, loc=(-79.0, -64.0, 39.0), r=1.5, bevel=0.8, bevel_seg=2)
    union(knob, box("pow_mark", 2.0, 7.0, 0.5, loc=(-79.0, -64.0, 45.9)))
    part(knob, rgba=hexc("#141414"), gloss=20, spec=0.15, anim="move", chan="power", axis=(1, 0, 0), amount=12.0)
    o = box("pow_red", 2.0, 7.0, 0.3, loc=(-79.0, -64.0, 46.4))
    part(o, rgba=RED, gloss=80, spec=0.5, anim="move", chan="power", axis=(1, 0, 0), amount=12.0)
    o = box("reset", 20.0, 10.0, 3.0, loc=(-72.0, -86.0, 39.0), r=1.0, bevel=0.6, bevel_seg=2)
    part(o, rgba=RESET_BLUE, gloss=40, spec=0.3)
    translate_all(-DX, -SY, -CAP_TOP)
    return "console_md"


def cart():
    """PAL / Genesis shell: 109 x 70 x 17; a plain lower part, large rounded shoulders and
    a gently curved top; four finger slots each side; the label recess."""
    reset()
    W, H, D = 109.0, 70.0, 17.0
    out = [(-54.5, 0.0), (54.5, 0.0)]
    out += arc(39.5, 55.0, 15.0, 0, 90, 8)
    out += arc(-39.5, 55.0, 15.0, 90, 180, 8)
    shell = extrude("shell", out, D, "XZ", -D / 2, bevel=1.5, bevel_seg=3)
    boolean(shell, extrude("label_rec", rrect(80.0, 50.0, 2.0, cy=32.0), 1.5, "XZ", -D / 2 - 1.0))
    for sx in (-1, 1):
        for i in range(4):
            boolean(shell, box("finger", 12.0, 2.5, 2.5, loc=(sx * 47.0, -D / 2 + 1.0, 38.0 + i * 5.0), r=1.2))
    boolean(shell, box("conn", 90.0, 8.0, 4.0, loc=(0.0, 0.0, -1.0)))
    part(shell, rgba=hexc("#1a1a1a"), gloss=18, spec=0.12)
    o = extrude("label", rrect(79.0, 49.0, 1.8, cy=32.0), 0.25, "XZ", -D / 2 + 0.3)
    part(o, "label", (1, 1, 1, 1), gloss=40, spec=0.2)
    translate_all(0.0, 0.0, -H / 2)
    return "cart_md"


if __name__ == "__main__":
    os.makedirs(PREV, exist_ok=True)
    for build in (console, cart):
        key = build()
        export(key, OUT)
        preview(key, PREV, views=("34", "front", "top", "back"))
        pose({"open": 1.0, "power": 1.0})
        preview(key + "-pose", PREV, views=("34",))
