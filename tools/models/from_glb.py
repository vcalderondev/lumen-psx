"""Convert a downloaded CC-BY model (tools/models/downloads/<name>/model.glb, see
CREDITS.md) into a Lumen model: drop logos and lettering, scale to the real size in mm,
put the origin where Lumen expects it, decimate to what the console can draw, and give
each part its kind, plastic colour and animation.

blender -b -P tools/models/from_glb.py -- <assets/models dir> <preview dir> <recipe>...
"""
import math, os, re, sys
import bpy
from mathutils import Vector, Matrix
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lmdl import *

HERE = os.path.dirname(os.path.abspath(__file__))
ARGS = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
OUT, PREV = ARGS[0], ARGS[1]


def load(name):
    reset()
    bpy.ops.import_scene.gltf(filepath=os.path.join(HERE, "downloads", name, "model.glb"))
    # flatten: bake every object's world transform into its mesh, drop the empties
    for o in list(bpy.context.scene.objects):
        if o.type == "MESH":
            o.data = o.data.copy()
            o.data.transform(o.matrix_world)
            o.parent = None
            o.matrix_world = Matrix.Identity(4)
    for o in list(bpy.context.scene.objects):
        if o.type != "MESH":
            bpy.data.objects.remove(o, do_unlink=True)
    # the imported custom normals do not survive scaling and decimation: rebuild them
    for o in meshes():
        bpy.context.view_layer.objects.active = o
        try:
            bpy.ops.mesh.customdata_custom_splitnormals_clear()
        except Exception:
            pass
        smooth(o, 40.0)


def meshes():
    return [o for o in bpy.context.scene.objects if o.type == "MESH"]


def drop(pattern):
    for o in meshes():
        if re.search(pattern, o.name):
            bpy.data.objects.remove(o, do_unlink=True)


def bounds(objs=None):
    lo = Vector((1e18,) * 3); hi = Vector((-1e18,) * 3)
    for o in objs or meshes():
        for v in o.data.vertices:
            lo = Vector(map(min, lo, v.co)); hi = Vector(map(max, hi, v.co))
    return lo, hi


def transform_all(m):
    for o in meshes():
        o.data.transform(m)
        o.data.update()


def fit(axis, mm):
    """Scale uniformly so the model measures mm along axis (0 x, 1 y, 2 z)."""
    lo, hi = bounds()
    k = mm / (hi[axis] - lo[axis])
    transform_all(Matrix.Scale(k, 4))


def stretch(axis, mm):
    """Scale along one axis only, to mm (models that are thinner or wider than the real
    thing; the reference sheet decides)."""
    lo, hi = bounds()
    k = [1.0, 1.0, 1.0]
    k[axis] = mm / (hi[axis] - lo[axis])
    transform_all(Matrix.Diagonal((k[0], k[1], k[2], 1.0)))


def centre(at=(0.5, 0.5, 0.5), to=(0, 0, 0)):
    """Move the point at fractions `at` of the bounds to `to`."""
    lo, hi = bounds()
    p = Vector([lo[i] + (hi[i] - lo[i]) * at[i] for i in range(3)])
    transform_all(Matrix.Translation(Vector(to) - p))


def ntris(o):
    return sum(len(p.vertices) - 2 for p in o.data.polygons)


def decimate(o, tris):
    """Planar first (merges coplanar triangles: flat faces stay exact), then collapse
    only what is still over budget."""
    if ntris(o) <= tris:
        return
    md = o.modifiers.new("plan", "DECIMATE")
    md.decimate_type = "DISSOLVE"
    md.angle_limit = math.radians(0.5)
    md.delimit = {"NORMAL", "MATERIAL", "SEAM", "SHARP", "UV"}
    apply_mods(o)
    bpy.context.view_layer.objects.active = o
    md = o.modifiers.new("tri", "TRIANGULATE")
    apply_mods(o)
    if ntris(o) > tris:
        md = o.modifiers.new("dec", "DECIMATE")
        md.ratio = tris / ntris(o)
        md.use_collapse_triangulate = True
        md.use_symmetry = False
        apply_mods(o)
    smooth(o, 40.0)


def split_inside(o, lo, hi, name):
    """Move the faces of o whose centre lies inside the X/Z box (front view) lo..hi into a
    new object called name; returns it."""
    import bmesh
    bm = bmesh.new(); bm.from_mesh(o.data)
    inside = [f for f in bm.faces if lo[0] <= f.calc_center_median().x <= hi[0] and lo[2] <= f.calc_center_median().z <= hi[2]]
    bm2 = bmesh.new()
    vmap = {}
    for f in inside:
        vs = []
        for v in f.verts:
            if v not in vmap:
                vmap[v] = bm2.verts.new(v.co)
            vs.append(vmap[v])
        bm2.faces.new(vs)
    bmesh.ops.delete(bm, geom=inside, context="FACES")
    bm.to_mesh(o.data); bm.free()
    me = bpy.data.meshes.new(name); bm2.to_mesh(me); bm2.free()
    n = bpy.data.objects.new(name, me)
    bpy.context.scene.collection.objects.link(n)
    smooth(n, 40.0)
    return n


def take(pattern, **kw):
    """Parts whose name matches: join them into one, then set the part properties."""
    objs = [o for o in meshes() if re.search(pattern, o.name) and "kind" not in o]
    if not objs:
        print("[from_glb] no part matches", pattern)
        return None
    if len(objs) > 1:
        bpy.context.view_layer.objects.active = objs[0]
        for o in bpy.context.selected_objects:
            o.select_set(False)
        for o in objs:
            o.select_set(True)
        bpy.ops.object.join()
    o = bpy.context.view_layer.objects.active if len(objs) > 1 else objs[0]
    o.name = kw.pop("name", o.name)
    tris = kw.pop("tris", None)
    if tris:
        decimate(o, tris)
    part(o, **kw)
    return o


def leftovers():
    rest = [o.name for o in meshes() if "kind" not in o]
    if rest:
        print("[from_glb] unassigned parts (dropped):", rest)
        for o in meshes():
            if "kind" not in o:
                bpy.data.objects.remove(o, do_unlink=True)


# ---- Game Boy (DMG-01), Smoggybeard (CC BY 4.0) ----------------------------------------
# 90 x 148 x 32 mm, front towards -Y; Lumen's handheld frame: origin at the body's centre.
def console_gb():
    load("gb_smoggybeard")
    drop(r"TEXT_WHITE|Text_Blue")             # "Nintendo GAME BOY", "DOT MATRIX ..." lettering
    fit(2, 148.0)
    stretch(1, 32.0)                           # the download is 22 mm deep; the DMG is 32
    centre()
    take(r"White_Plastic", name="body", rgba=hexc("#c9c7c1"), gloss=16, spec=0.12, tris=60000)
    take(r"Screen", name="screen", kind="screen", rgba=hexc("#8a9a3c"), on=hexc("#b8d870"))
    take(r"Glass", name="lens", kind="glass", rgba=(0.95, 0.97, 1.0, 0.10), gloss=160, spec=0.9, tris=4000)
    # the slate bezel printed on the lens (ref/gb.md 2): 77 x 58 at y +32.8, corners r2 and
    # r15 at the bottom right, a 48 x 44 window over the LCD; on the body's front face
    body = [o for o in meshes() if o.name == "body"][0]
    yf = min(v.co.y for v in body.data.vertices if -38 < v.co.x < 38 and 4 < v.co.z < 61)
    plate = extrude("bezel", rrect(77.0, 58.0, [2.0, 2.0, 15.0, 2.0], cy=32.8), 0.6, "XZ", yf - 0.2)
    boolean(plate, extrude("win", rrect(48.0, 44.0, 0.6, cy=32.8), 3.0, "XZ", yf - 1.0))
    part(plate, rgba=hexc("#4a4c5c"), gloss=110, spec=0.6)
    for pat, nm, col in ((r"Red_Line", "line_red", "#7a1f4a"), (r"BLUE_Line", "line_blue", "#2b3a78")):
        o = take(pat, name=nm, rgba=hexc(col), gloss=20, spec=0.1)
        if o:
            o.data.transform(Matrix.Translation((0, (yf - 0.35) - min(v.co.y for v in o.data.vertices), 0)))
    take(r"D-PAD", name="dpad", rgba=hexc("#18181b"), gloss=30, spec=0.2, tris=6000)
    take(r"A_B_Buttons", name="ab", rgba=hexc("#8e1f50"), gloss=40, spec=0.3)
    take(r"START_SELECT", name="start_select", rgba=hexc("#7b7d86"), gloss=10, spec=0.05)
    lo, hi = bounds([o for o in meshes() if "BATTERY" in o.name])
    led = take(r"BATTERY_LIGHT", name="led", kind="light", rgba=hexc("#3a1214"), on=hexc("#ff2a1a"),
               pivot=tuple((lo + hi) / 2), gloss=120, spec=0.8)
    if led:   # in front of the bezel plate, which is printed round it
        dy = (yf - 0.45) - min(v.co.y for v in led.data.vertices)
        led.data.transform(Matrix.Translation((0, dy, 0)))
        led["pivot"] = [led["pivot"][0], led["pivot"][1] + dy, led["pivot"][2]]
    leftovers()
    return "console_gb"


def open_box(name, lo, hi, open_face="-Y"):
    """A box with one face missing (a bay seen through an opening)."""
    import bmesh
    bm = bmesh.new()
    bmesh.ops.create_cube(bm, size=1.0)
    for v in bm.verts:
        v.co = Vector([lo[i] + (v.co[i] + 0.5) * (hi[i] - lo[i]) for i in range(3)])
    ax = "XYZ".index(open_face[1]); sg = -1 if open_face[0] == "-" else 1
    gone = [f for f in bm.faces if f.normal[ax] * sg > 0.9]
    bmesh.ops.delete(bm, geom=gone, context="FACES")
    me = bpy.data.meshes.new(name); bm.to_mesh(me); bm.free()
    o = bpy.data.objects.new(name, me); bpy.context.scene.collection.objects.link(o)
    return o


# ---- NES (NES-001), miaparcela (CC BY 4.0) ----------------------------------------------
# 256 x 203 x 85 mm, front towards -Y; Lumen's front-loader frame: top at 0, centred in x
# and depth. The door (ref/nes.md 2): x -94.6 .. +54.3, from 59.5 mm above the bottom to
# the top, hinged at its top edge, swinging in; the bay behind it is dark.
def console_nes():
    load("nes_miaparcela")
    drop(r"^[A-Za-z](_\d+)?_Mat\.1_0$|^NES_Mat\.1_0$")   # lettering and the red numerals
    fit(0, 256.0)
    stretch(2, 85.0)
    centre(at=(0.5, 0.5, 1.0))                    # top centre -> origin (bottom at z = -85)
    import bmesh
    upper = [o for o in meshes() if o.name == "NES_Mat.2_0"][0]
    ulo, uhi = bounds([o for o in meshes() if o.name == "NES_Mat.2_0"])
    blo, bhi = bounds([o for o in meshes() if o.name == "NES_Mat.4_0"])
    fy = ulo.y                                    # the upper shell's front face
    x0, x1, z0, z1 = -94.6, blo.x, 59.5 - 85.0, 0.0
    # the meshes are open (not solids), so no boolean: drop the upper shell's front face
    # and rebuild it round the door opening
    bm = bmesh.new(); bm.from_mesh(upper.data)
    front = [f for f in bm.faces if f.normal.y < -0.9 and f.calc_center_median().y < fy + 0.5 and f.calc_center_median().x < blo.x]
    bmesh.ops.delete(bm, geom=front, context="FACES")
    bm.to_mesh(upper.data); bm.free()
    for nm, (a, b, c, d) in {"face_left": (ulo.x, x0, ulo.z, z1), "face_low": (x0, x1, ulo.z, z0)}.items():
        o = box(nm, b - a, 0.8, d - c, loc=((a + b) / 2, fy + 0.4, c))
        o.name = "NES_Mat.2_0_" + nm
    take(r"NES_Mat\.2_0", name="upper", rgba=hexc("#c9c7c2"), gloss=14, spec=0.1)
    take(r"NES_Mat\.3_0", name="lower", rgba=hexc("#8c898a"), gloss=16, spec=0.12)
    take(r"NES_Mat\.4_0", name="band", rgba=hexc("#2a2a2a"), gloss=30, spec=0.2)
    take(r"NES__0", name="details", rgba=hexc("#2c2c2c"), gloss=20, spec=0.1)
    take(r"NES_Mat\.5_0", name="contacts", rgba=hexc("#c9a54a"), gloss=80, spec=0.6)
    lo_l, hi_l = bounds([o for o in meshes() if o.name == "NES_Mat_0"])
    take(r"NES_Mat_0", name="led", kind="light", rgba=hexc("#3a1214"), on=hexc("#ff2a1a"),
         pivot=tuple((lo_l + hi_l) / 2), gloss=120, spec=0.8)
    bay = open_box("bay", (x0 + 0.4, fy + 0.6, z0 - 14.0), (x1 - 0.4, fy + 138.0, z1 - 1.5))
    part(bay, "dark", hexc("#070708"))
    door = box("door", x1 - x0 - 0.8, 2.6, (z1 - 1.0) - z0 - 0.4, loc=((x0 + x1) / 2, fy + 1.4, z0 + 0.2), r=0.6, bevel=0.5, bevel_seg=2)
    part(door, rgba=hexc("#b6b5af"), gloss=12, spec=0.08, anim="rot", chan="door",
         pivot=((x0 + x1) / 2, fy + 1.4, z1 - 1.2), axis=(1, 0, 0), amount=85.0)
    tab = box("door_tab", 31.0, 1.6, 3.0, loc=(-17.5, fy, z0 + 0.4), r=0.8, bevel=0.4, bevel_seg=2)
    part(tab, rgba=hexc("#d2d0cb"), gloss=12, spec=0.08, anim="rot", chan="door",
         pivot=((x0 + x1) / 2, fy + 1.4, z1 - 1.2), axis=(1, 0, 0), amount=85.0)
    leftovers()
    return "console_nes"


def join_all(name):
    objs = meshes()
    bpy.context.view_layer.objects.active = objs[0]
    for o in bpy.context.selected_objects:
        o.select_set(False)
    for o in objs:
        o.select_set(True)
    if len(objs) > 1:
        bpy.ops.object.join()
    o = bpy.context.view_layer.objects.active
    o.name = name
    return o


def align_pca(o):
    """Rotate a scan onto its principal axes: largest extent -> Z, middle -> X,
    smallest -> Y (a cartridge: height, width, thickness)."""
    import numpy as np
    v = np.array([p.co[:] for p in o.data.vertices])
    c = v.mean(axis=0)
    w, vec = np.linalg.eigh(np.cov((v - c).T))
    order = np.argsort(w)                     # small, mid, large
    R = np.stack([vec[:, order[1]], vec[:, order[0]], vec[:, order[2]]])   # rows: new X, Y, Z
    if np.linalg.det(R) < 0:
        R[1] = -R[1]
    m = Matrix(((*R[0], 0), (*R[1], 0), (*R[2], 0), (0, 0, 0, 1))) @ Matrix.Translation(Vector(-c))
    o.data.transform(m)


# ---- NES Game Pak, 3D scan by Matter and Form (CC BY 4.0) ---------------------------------
# 120 x 134 x 17 (ref/nes.md 5). Front: the grip strip on the left, the label recess on the
# right (u 48.1 .. 104.5 from the left, v 0 .. 91.7 from the top).
def cart_nes():
    load("nescart_smb3_matterandform")
    o = join_all("shell")
    align_pca(o)
    lo, hi = bounds()
    k = 134.0 / (hi.z - lo.z)
    transform_all(Matrix.Scale(k, 4))
    centre()
    # orientation: the narrow connector end goes down, the grip strip to the left (-X),
    # the label face to the front (-Y). Measured on the scan: flip until it holds.
    def width_at(zf):
        lo, hi = bounds(); z = lo.z + (hi.z - lo.z) * zf
        xs = [p.co.x for p in o.data.vertices if abs(p.co.z - z) < 2.0]
        return max(xs) - min(xs)
    if width_at(0.06) > width_at(0.94):           # the narrow end must be at the bottom
        transform_all(Matrix.Rotation(math.pi, 4, "Y"))
    def grooves_x():                              # the grip grooves: x where the front is most rugged
        lo, hi = bounds(); best = (0, 0)
        for side in (-1, 1):
            xs = [p.co.y for p in o.data.vertices if side * p.co.x > 20 and side * p.co.x < 45 and p.co.y < 0]
            if xs:
                import statistics
                sd = statistics.pstdev(xs)
                if sd > best[0]:
                    best = (sd, side)
        return best[1]
    if grooves_x() > 0:                           # grip strip must be on the left
        transform_all(Matrix.Rotation(math.pi, 4, "Z"))
    lo, hi = bounds()
    print(f"[from_glb] cart_nes scan aligned: {hi.x - lo.x:.1f} x {hi.z - lo.z:.1f} x {hi.y - lo.y:.1f}")
    decimate(o, 40000)
    # the label region on the front face -> its own part (the game's art)
    lab = split_inside_front(o, (lo.x + 48.6, 0, hi.z - 91.2), (lo.x + 104.0, 0, hi.z - 0.5))
    part(lab, "label", (1, 1, 1, 1), gloss=18, spec=0.12)
    part(o, rgba=hexc("#808080"), gloss=14, spec=0.1)
    return "cart_nes"


def split_inside_front(o, lo, hi):
    """split_inside for the faces looking forward (-Y) only."""
    import bmesh
    bm = bmesh.new(); bm.from_mesh(o.data)
    ymin = min(v.co.y for v in bm.verts)
    inside = [f for f in bm.faces if f.normal.y < -0.6 and f.calc_center_median().y < ymin + 2.5
              and lo[0] <= f.calc_center_median().x <= hi[0] and lo[2] <= f.calc_center_median().z <= hi[2]]
    bm2 = bmesh.new(); vmap = {}
    for f in inside:
        vs = []
        for v in f.verts:
            if v not in vmap:
                vmap[v] = bm2.verts.new(v.co)
            vs.append(vmap[v])
        try:
            bm2.faces.new(vs)
        except ValueError:
            pass
    bmesh.ops.delete(bm, geom=inside, context="FACES")
    bm.to_mesh(o.data); bm.free()
    me = bpy.data.meshes.new("label"); bm2.to_mesh(me); bm2.free()
    n = bpy.data.objects.new("label", me); bpy.context.scene.collection.objects.link(n)
    smooth(n, 40.0)
    return n


RECIPES = {"console_gb": console_gb, "console_nes": console_nes, "cart_nes": cart_nes}

if __name__ == "__main__":
    os.makedirs(PREV, exist_ok=True)
    for r in ARGS[2:]:
        key = RECIPES[r]()
        lo, hi = bounds()
        print(f"[from_glb] {key}: {hi.x - lo.x:.1f} x {hi.z - lo.z:.1f} x {hi.y - lo.y:.1f} mm (w x h x d)")
        export(key, OUT)
        preview(key, PREV, views=("34", "front", "back", "top"))
        pose({"door": 1.0, "open": 1.0, "power": 1.0})
        preview(key + "-pose", PREV, views=("34", "low"))
