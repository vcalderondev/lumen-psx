"""Nintendo 64 (NUS-001) and its Game Pak.
blender -b -P tools/models/n64.py -- <assets/models dir> [<preview dir>]

References: tools/models/ref/n64.md (US Des. 379,832 console and Des. 376,795 cartridge
drawings scaled to the published 260 x 190 x 73 mm and 116 x 77 x 18.5 mm; Commons
photos for the rear and colours). No logos or lettering.
Blender axes: X right, front towards -Y, Z up, mm. Built in the sheet's frame (origin on
the table at the centre of the plan, so Blender y = -sheet z), then moved so the centre
of the slot opening at the top of the hump is the origin (Lumen's top-loader frame).
"""
import math, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lmdl import *

ARGS = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
OUT = ARGS[0] if ARGS else os.path.join(os.path.dirname(__file__), "..", "..", "assets", "models")
PREV = ARGS[1] if len(ARGS) > 1 else os.path.join(os.path.dirname(__file__), "build")

BODY = hexc("#3a3a3c")
KNOB = hexc("#424143")
LIGHT = hexc("#b8b8ba")
FLAP = hexc("#bdbdbd")
CAV = hexc("#2a2a2c")
FOOT = hexc("#1c1c1e")
GLOSS_BLACK = hexc("#0a0a0a")
LED_OFF, LED_ON = hexc("#3b1a1c"), hexc("#ff2a1a")
CART = hexc("#a0a1a3")

SLOT_Y = 51.0      # slot centre (Blender y; sheet z = -51)
TOP = 73.0         # slot rim height


def body_plan():
    """Deck outline (plan, Blender x / y; +y = rear): rear edge 226 wide with R25 corners,
    sides tapering out, the front corners rounding into the pods, a slightly convex front."""
    half = [(0.0, 95.0), (88.0, 95.0)]
    half += arc(88.0, 70.0, 25.0, 90, 0)[1:]
    half += [(115.0, 30.0), (117.0, -6.0), (114.0, -36.0)]
    half += arc(74.0, -48.0, 40.0, -10, -90, 10)[1:]
    half += [(40.0, -88.0), (20.0, -89.5), (0.0, -90.0)]
    return mirror_x(half)


def hump_plan():
    half = [(0.0, 95.0), (71.5, 95.0), (68.0, 60.0), (59.0, 26.0)]
    half += arc(0.0, 26.0, 1.0, 0, -90, 16)[1:]   # placeholder, replaced below
    half = half[:4]
    for i in range(1, 17):                         # semi-ellipse 59 x 40 to the front point
        a = math.radians(-90.0 * i / 16)
        half.append((59.0 * math.cos(a), 26.0 + 40.0 * math.sin(a)))
    return mirror_x(half)


def console():
    reset()
    # main body: lower skirt and deck in one shell, rounded top edge
    body = extrude("body", body_plan(), 54.0, "XY", 5.0, bevel=9.0, bevel_seg=6)
    # the parting seam, a shallow groove all round at y = 31
    outer = extrude("seam_o", offset(body_plan(), 4.0), 0.8, "XY", 30.6)
    inner = extrude("seam_i", offset(body_plan(), -0.7), 1.2, "XY", 30.4)
    boolean(outer, inner)
    boolean(body, outer)
    # the slider wells and the Jumper Pak lid groove in the deck
    for x in (-64.0, 64.0):
        boolean(body, extrude("well", rrect(19.4, 43.0, 9.6, cx=x, cy=-42.0), 10.0, "XY", 55.0))
    lid_o = extrude("lid_o", rrect(56.6, 45.6, 6.8, cy=-42.0), 4.0, "XY", 57.0)
    boolean(lid_o, extrude("lid_i", rrect(55.0, 44.0, 6.0, cy=-42.0), 6.0, "XY", 56.0))
    boolean(body, lid_o)
    # rear: the power-supply bay and the AV pocket
    boolean(body, box("psu_bay", 125.0, 40.0, 38.0, loc=(-10.0, 95.0, 3.0), r=3.0))
    boolean(body, box("av_pocket", 40.0, 8.0, 20.0, loc=(95.0, 95.0, 8.0), r=3.0))
    # side vents: vertical slits low on the rear half of both sides
    for sx in (-1, 1):
        for i in range(16):
            z = -85.0 + i * 4.8
            boolean(body, box("vent", 8.0, 2.4, 18.0, loc=(sx * 116.0, -z, 11.0), r=1.0))
    part(body, "plastic", BODY, gloss=14, spec=0.12)
    # nose: the centre front rises to the logo plate on a face leaning back ~25 degrees
    # (only the front strip between the inner ports, in front of the Jumper Pak lid)
    nose_side = [(-90.6, 30.0), (-90.2, 36.0), (-80.0, 64.0), (-72.0, 65.0), (-66.0, 60.5), (-66.0, 30.0)]
    nose = extrude("nose", [(y, z) for y, z in nose_side], 60.0, "YZ", -30.0, bevel=4.0, bevel_seg=4)
    part(nose, "plastic", BODY, gloss=14, spec=0.12)
    # logo plate: a glossy black rounded trapezoid on the nose (no logo)
    plate = extrude("logo_plate", [(-24.5, 14.5), (24.5, 14.5), (20.0, -14.5), (-20.0, -14.5)], 1.2, "XZ", -0.6, bevel=0.5, bevel_seg=2)
    rotate(plate, "X", -25.0)
    move(plate, 0.0, -85.6, 50.0)
    part(plate, "plastic", GLOSS_BLACK, gloss=140, spec=0.9)
    # front pods: dome cylinders below the deck, black base discs under them
    for sx in (-1, 1):
        pod = cyl("pod", 53.0, 32.0, loc=(sx * 103.5, -62.0, 11.0), bevel=10.0, n=64)
        part(pod, "plastic", BODY, gloss=14, spec=0.12)
        base = cyl("pod_base", 44.0, 11.0, loc=(sx * 103.5, -66.0, 0.0), bevel=1.5, n=48)
        part(base, "plastic", FOOT, gloss=8, spec=0.05)
        foot = cyl("rear_foot", 27.0, 5.0, loc=(sx * 84.0, 59.0, 0.0), bevel=2.0, n=32)
        part(foot, "plastic", FOOT, gloss=8, spec=0.05)
    # the hump around the slot, its fan grille and the slot surround
    hump = extrude("hump", hump_plan(), 22.0, "XY", TOP - 22.0, bevel=10.0, bevel_seg=6)
    boolean(hump, extrude("surround", rrect(122.0, 25.0, 5.0, cy=SLOT_Y), 6.0, "XY", TOP - 3.0))
    boolean(hump, extrude("opening", rrect(118.0, 20.0, 3.0, cy=SLOT_Y), 40.0, "XY", TOP - 40.0))
    for i in range(21):
        a = math.radians(-155.0 + 130.0 * i / 20)
        slit = box("slit", 2.5, 13.0, 8.0, loc=(0.0, SLOT_Y - 41.0, TOP - 4.0), r=1.2)
        rotate(slit, "Z", math.degrees(a) + 90.0, pivot=(0.0, SLOT_Y, 0.0))
        boolean(hump, slit)
    part(hump, "plastic", BODY, gloss=14, spec=0.12)
    o = extrude("emboss", rrect(40.0, 8.0, 4.0, cy=78.0), 0.6, "XY", TOP - 0.1, bevel=0.25, bevel_seg=2)
    part(o, "plastic", KNOB, gloss=20, spec=0.15)
    # inside the slot: dark depth, the two light-grey spring flaps
    o = box("slot_dark", 117.0, 19.0, 30.0, loc=(0.0, SLOT_Y, TOP - 36.0))
    part(o, "dark", hexc("#050506"))
    for s in (1, -1):   # hinged on the long edges, swinging down and apart
        hy = SLOT_Y + s * 9.8
        f = box(f"flap_{s}", 116.0, 9.6, 1.0, loc=(0.0, hy - s * 4.8, TOP - 5.0), r=0.5)
        part(f, "plastic", FLAP, gloss=12, spec=0.1, anim="rot", chan="open",
             pivot=(0.0, hy, TOP - 4.5), axis=(1, 0, 0), amount=s * 80.0)
    # POWER and RESET slide knobs with grip ribs, the Jumper Pak lid and its tab
    for x, nm in ((-64.0, "power"), (64.0, "reset")):
        knob = box(f"knob_{nm}", 14.0, 22.0, 6.0, loc=(x, -46.0, 55.0), r=6.5, bevel=1.5, bevel_seg=3)
        for i in range(5):
            union(knob, box("rib", 12.0, 1.2, 1.0, loc=(x, -53.0 + i * 3.5, 60.6), r=0.5))
        if nm == "power":
            part(knob, "plastic", KNOB, gloss=14, spec=0.12, anim="move", chan="power", axis=(0, 1, 0), amount=12.0)
        else:
            part(knob, "plastic", KNOB, gloss=14, spec=0.12)
    o = extrude("jumper_lid", rrect(55.0, 44.0, 6.0, cy=-42.0), 1.0, "XY", 58.4, bevel=0.4, bevel_seg=2)
    part(o, "plastic", BODY, gloss=14, spec=0.12)
    o = box("lid_tab", 10.0, 4.0, 1.4, loc=(0.0, -21.5, 58.6), r=1.5)
    part(o, "plastic", KNOB, gloss=14, spec=0.12)
    # controller ports: light-grey round bezels, trapezoid sockets, three pins
    for x in (-66.0, -36.0, 36.0, 66.0):
        fy = -88.0 - 0.012 * x * x / 10.0
        bez = cyl("bezel", 22.0, 6.0, loc=(x, fy - 4.0, 31.0), axis="Y", bevel=1.2, n=48)
        sock = extrude("sock", [(-6.75, 3.75 + 1.5), (6.75, 3.75 + 1.5), (5.0, -3.75 + 1.5), (-5.0, -3.75 + 1.5)], 7.0,
                       "XZ", fy - 4.5, bevel=0.0)
        move(sock, x, 0.0, 31.0)
        boolean(bez, sock)
        part(bez, "plastic", LIGHT, gloss=12, spec=0.1)
        o = extrude("sock_floor", [(-6.5, 5.0), (6.5, 5.0), (4.9, -2.1), (-4.9, -2.1)], 0.4, "XZ", fy + 1.8)
        move(o, x, 0.0, 31.0)
        part(o, "plastic", LIGHT, gloss=12, spec=0.1)
        for px in (-3.8, 0.0, 3.8):
            o = cyl("pin", 2.5, 0.3, loc=(x + px, fy + 1.6, 32.5), axis="Y", n=16)
            part(o, "dark", CAV)
    # the power LED: a horizontal oval lens under the parting seam
    led = extrude("led", rrect(13.0, 3.7, 1.85, seg=6), 1.2, "XZ", -91.2, bevel=0.4, bevel_seg=2)
    move(led, 0.0, 0.0, 24.0)
    part(led, "light", LED_OFF, on=LED_ON, gloss=120, spec=0.8, pivot=(0.0, -91.5, 24.0))
    translate_all(0.0, -SLOT_Y, -TOP)
    return "console_n64"


def cart():
    """Game Pak: 116 x 77 x 18.5, arched top (R146, 12 mm lower at the edges), three
    bands on the face, the label recess, step notches at the bottom corners; on the back
    the sticker recess, two screws and the two notches the dust flaps ride in."""
    reset()
    W, H, D = 116.0, 77.0, 18.5
    cz = H - 146.0
    outline = [(-58.0, 0.0), (58.0, 0.0)]
    outline += [(58.0, 62.0)]
    for i in range(1, 30):
        x = 58.0 - 116.0 * i / 30
        outline.append((x, cz + math.sqrt(146.0 ** 2 - x * x)))
    outline += [(-58.0, 62.0)]
    shell = extrude("shell", outline, D, "XZ", -D / 2, bevel=1.8, bevel_seg=3)
    # band grooves on both faces
    for x in (-34.3, 34.3):
        for y0 in (-D / 2 - 0.5, D / 2 - 0.5):
            g = box("groove", 1.0, 1.0, H + 2, loc=(x, y0 + 0.5, -1.0))
            rotate(g, "Y", 0.0)
            boolean(shell, g)
    # front: label recess (its top follows the arch), corner steps
    lab = [(-27.85, 6.4), (27.85, 6.4)]
    for i in range(0, 21):
        x = 27.85 - 55.7 * i / 20
        lab.append((x, cz + math.sqrt(146.0 ** 2 - x * x) - 6.2))
    boolean(shell, extrude("label_rec", lab, 2.0, "XZ", -D / 2 - 1.3))
    for sx in (-1, 1):
        boolean(shell, box("step", 9.5 + 1.0, 3.5 + 1.0, 28.0 + 1.0, loc=(sx * (58.0 - 4.25), -D / 2 + 1.25, -1.0)))
    # back: sticker recess, the flap notches on the bottom edge
    boolean(shell, extrude("sticker", rrect(80.0, 39.8, 3.0, cy=33.8), 2.0, "XZ", D / 2 - 0.7))
    for sx in (-1, 1):
        boolean(shell, box("notch", 10.3, 4.0, 10.8 + 1.0, loc=(sx * (58.0 - 13.65), D / 2 - 1.0, -1.0)))
    part(shell, "plastic", CART, gloss=12, spec=0.1)
    for sx in (-1, 1):
        o = cyl("screw", 3.8, 0.6, loc=(sx * 32.0, D / 2 - 0.3, 67.0), axis="Y", n=20)
        part(o, "plastic", hexc("#8a8c90"), gloss=60, spec=0.5)
    # the label (the game's art) in its recess
    lbl = extrude("label", [(x * 0.985, y if y < 10 else y - 0.4) for x, y in lab], 0.3, "XZ", -D / 2 + 0.55)
    part(lbl, "label", (1, 1, 1, 1), gloss=14, spec=0.1)
    translate_all(0.0, 0.0, -H / 2)
    return "cart_n64"


if __name__ == "__main__":
    os.makedirs(PREV, exist_ok=True)
    for build in (console, cart):
        key = build()
        export(key, OUT)
        preview(key, PREV, views=("34", "front", "top", "back", "side"))
        pose({"open": 1.0, "power": 1.0})
        preview(key + "-pose", PREV, views=("34",))
