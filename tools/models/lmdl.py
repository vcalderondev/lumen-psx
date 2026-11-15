"""Lumen model toolkit for Blender (run inside Blender: blender -b -P <model>.py).

Models are built in millimetres, Blender's axes: X right, Y towards the BACK (the
front of a model faces -Y, as in Blender's front view), Z up. The exporter converts to
Lumen's frame (x right, y up, z towards the viewer) and writes <out>/<key>.lmdl (v1,
full detail: optimize.py turns it into the v2 that Lumen loads, see src/meshes.inc) plus
<key>.glb to look at in any viewer, and renders
previews (<key>-front.png, -34.png, -top.png, -back.png) with tools/models/preview.

Every mesh object is one part; its custom properties say how Lumen draws it:
  kind  plastic | label | screen | light | glass | dark | back (the case's back card,
        seen from behind) | spine (the printed spine, seen from the left)
  rgba  base colour (sRGB 0..1, alpha), on  colour when lit (screens, lights)
  gloss specular exponent, spec  specular strength
  anim  none | rot | move, chan  open | door | power | eject, pivot (mm, Blender
  axes), axis (Blender axes), amount (degrees or mm at channel = 1)
"""
import math, os, struct, sys
import bpy, bmesh
from mathutils import Vector, Matrix

KINDS = {"plastic": 0, "label": 1, "screen": 2, "light": 3, "glass": 4, "dark": 5, "back": 6, "spine": 7}
ANIMS = {"none": 0, "rot": 1, "move": 2}
CHANS = {"none": 0, "open": 1, "door": 2, "power": 3, "eject": 4}
_MATS = {}


def hexc(h, a=1.0):
    h = h.lstrip("#")
    return (int(h[0:2], 16) / 255, int(h[2:4], 16) / 255, int(h[4:6], 16) / 255, a)


# ---- scene ----------------------------------------------------------------------------

def reset():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    _MATS.clear()
    sc = bpy.context.scene
    sc.unit_settings.system = "METRIC"
    sc.unit_settings.scale_length = 0.001
    sc.unit_settings.length_unit = "MILLIMETERS"


def _link(obj):
    bpy.context.scene.collection.objects.link(obj)
    return obj


def _mesh_obj(name, bm):
    me = bpy.data.meshes.new(name)
    bm.to_mesh(me)
    bm.free()
    return _link(bpy.data.objects.new(name, me))


def apply_mods(obj):
    bpy.context.view_layer.objects.active = obj
    for o in bpy.context.selected_objects:
        o.select_set(False)
    obj.select_set(True)
    for m in list(obj.modifiers):
        bpy.ops.object.modifier_apply(modifier=m.name)
    return obj


# ---- profiles -------------------------------------------------------------------------

def rrect(w, h, r=0.0, seg=None, cx=0.0, cy=0.0):
    """Rounded rectangle outline (counter-clockwise) in the XY plane. r: one radius or
    four (top-left, top-right, bottom-right, bottom-left), top = +Y."""
    rs = list(r) if isinstance(r, (list, tuple)) else [r] * 4
    hw, hh = w / 2, h / 2
    rs = [min(max(x, 0.0), hw, hh) for x in rs]
    corners = [  # centre, start angle (arc of 90 degrees, counter-clockwise)
        ((hw - rs[1], hh - rs[1]), 0.0, rs[1]),            # top-right
        ((-hw + rs[0], hh - rs[0]), 90.0, rs[0]),          # top-left
        ((-hw + rs[3], -hh + rs[3]), 180.0, rs[3]),        # bottom-left
        ((hw - rs[2], -hh + rs[2]), 270.0, rs[2]),         # bottom-right
    ]
    pts = []
    for (ccx, ccy), a0, rr in corners:
        n = seg if seg else max(2, min(24, int(rr * 0.9) + 3))
        if rr <= 1e-6:
            pts.append((ccx + cx, ccy + cy))
            continue
        for i in range(n + 1):
            a = math.radians(a0 + 90.0 * i / n)
            pts.append((ccx + rr * math.cos(a) + cx, ccy + rr * math.sin(a) + cy))
    out = []
    for p in pts:  # drop duplicates
        if not out or (abs(p[0] - out[-1][0]) > 1e-5 or abs(p[1] - out[-1][1]) > 1e-5):
            out.append(p)
    if abs(out[0][0] - out[-1][0]) < 1e-5 and abs(out[0][1] - out[-1][1]) < 1e-5:
        out.pop()
    return out


def circle(d, n=48, cx=0.0, cy=0.0):
    return [(cx + d / 2 * math.cos(2 * math.pi * i / n), cy + d / 2 * math.sin(2 * math.pi * i / n)) for i in range(n)]


def arc(cx, cy, r, a0, a1, n=None):
    """Points along a circular arc from angle a0 to a1 (degrees), both ends included."""
    n = n or max(3, int(abs(a1 - a0) / 6) + 1)
    return [(cx + r * math.cos(math.radians(a0 + (a1 - a0) * i / n)), cy + r * math.sin(math.radians(a0 + (a1 - a0) * i / n)))
            for i in range(n + 1)]


def mirror_x(half):
    """Closed outline from its right half, listed from the top centre (x = 0) clockwise
    to the bottom centre: the left half is the mirror image, in reverse."""
    left = [(-x, y) for x, y in reversed(half) if abs(x) > 1e-6]
    return half + left


def offset(pts, d):
    """Offset a closed outline by d (> 0 outwards for a counter-clockwise outline)."""
    n = len(pts)
    area = sum(pts[i][0] * pts[(i + 1) % n][1] - pts[(i + 1) % n][0] * pts[i][1] for i in range(n))
    sgn = 1.0 if area > 0 else -1.0
    out = []
    for i in range(n):
        p0, p1, p2 = pts[i - 1], pts[i], pts[(i + 1) % n]
        e1 = (p1[0] - p0[0], p1[1] - p0[1]); e2 = (p2[0] - p1[0], p2[1] - p1[1])
        def nrm(e):
            l = math.hypot(*e) or 1.0
            return (e[1] / l * sgn, -e[0] / l * sgn)
        n1, n2 = nrm(e1), nrm(e2)
        m = (n1[0] + n2[0], n1[1] + n2[1]); ml = math.hypot(*m) or 1.0
        m = (m[0] / ml, m[1] / ml)
        cosh = max(0.3, m[0] * n2[0] + m[1] * n2[1])
        out.append((p1[0] + m[0] * d / cosh, p1[1] + m[1] * d / cosh))
    return out


def translate_all(dx, dy, dz):
    """Move every part (mesh data and pivots) so a chosen point becomes the origin."""
    for o in bpy.context.scene.objects:
        if o.type == "MESH":
            o.data.transform(Matrix.Translation((dx, dy, dz)))
            if "pivot" in o:
                p = o["pivot"]; o["pivot"] = [p[0] + dx, p[1] + dy, p[2] + dz]


def poly_offset(pts, dx=0.0, dy=0.0):
    return [(x + dx, y + dy) for x, y in pts]


# ---- solids ---------------------------------------------------------------------------

def extrude(name, pts, depth, plane="XY", z0=0.0, bevel=0.0, bevel_seg=4, bevel_top=True, bevel_bottom=True):
    """Prism from an outline. plane XY: outline in X/Y, extruded along +Z from z0 (a
    plan view: x right, y towards the back). XZ: outline in X/Z (a front view: x right,
    z up), extruded towards +Y (the back) from y = z0. YZ: outline in Y/Z (a side view),
    extruded along +X from x = z0. bevel: rounded edges of that radius on the caps."""
    bm = bmesh.new()
    vb = [bm.verts.new((x, y, z0)) for x, y in pts]
    vt = [bm.verts.new((x, y, z0 + depth)) for x, y in pts]
    n = len(pts)
    bm.faces.new(list(reversed(vb)))
    bm.faces.new(vt)
    for i in range(n):
        j = (i + 1) % n
        bm.faces.new((vb[i], vb[j], vt[j], vt[i]))
    if plane == "XZ":
        bmesh.ops.transform(bm, matrix=Matrix(((1, 0, 0, 0), (0, 0, 1, 0), (0, 1, 0, 0), (0, 0, 0, 1))), verts=bm.verts)
        bmesh.ops.reverse_faces(bm, faces=bm.faces)
    elif plane == "YZ":
        bmesh.ops.transform(bm, matrix=Matrix(((0, 0, 1, 0), (1, 0, 0, 0), (0, 1, 0, 0), (0, 0, 0, 1))), verts=bm.verts)
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    obj = _mesh_obj(name, bm)
    if bevel > 0:
        md = obj.modifiers.new("bevel", "BEVEL")
        md.width = bevel
        md.segments = bevel_seg
        md.limit_method = "ANGLE"
        md.angle_limit = math.radians(40)
        md.harden_normals = False
        md.profile = 0.5
        apply_mods(obj)
    smooth(obj)
    return obj


def box(name, w, d, h, loc=(0, 0, 0), r=0.0, bevel=0.0, bevel_seg=4):
    """Box w (X) x d (Y) x h (Z), centred at loc in X/Y, from loc.z up; r: plan corner
    radii (see rrect, top = back)."""
    o = extrude(name, rrect(w, d, r, cx=loc[0], cy=loc[1]), h, "XY", loc[2], bevel, bevel_seg)
    return o


def cyl(name, dia, h, loc=(0, 0, 0), axis="Z", bevel=0.0, n=48):
    pts = circle(dia, n)
    if axis == "Z":
        o = extrude(name, poly_offset(pts, loc[0], loc[1]), h, "XY", loc[2], bevel)
    elif axis == "Y":   # along +Y from loc.y, centred at loc.x / loc.z
        o = extrude(name, poly_offset(pts, loc[0], loc[2]), h, "XZ", loc[1], bevel)
    else:               # along +X from loc.x
        o = extrude(name, poly_offset(pts, loc[1], loc[2]), h, "YZ", loc[0], bevel)
    return o


def boolean(target, cutter, op="DIFFERENCE", keep=False):
    md = target.modifiers.new("bool", "BOOLEAN")
    md.object = cutter
    md.operation = op
    md.solver = "EXACT"
    apply_mods(target)
    if not keep:
        bpy.data.objects.remove(cutter, do_unlink=True)
    smooth(target)
    return target


def union(target, *others):
    for o in others:
        boolean(target, o, "UNION")
    return target


def smooth(obj, angle=35.0):
    """Shade smooth with sharp edges kept above angle (flat faces stay flat)."""
    me = obj.data
    for p in me.polygons:
        p.use_smooth = True
    try:
        bpy.context.view_layer.objects.active = obj
        for o in bpy.context.selected_objects:
            o.select_set(False)
        obj.select_set(True)
        bpy.ops.object.shade_smooth_by_angle(angle=math.radians(angle))
    except Exception:
        pass
    return obj


def move(obj, dx=0, dy=0, dz=0):
    obj.location = (obj.location.x + dx, obj.location.y + dy, obj.location.z + dz)
    return obj


def rotate(obj, axis, deg, pivot=(0, 0, 0)):
    """Rotate the mesh data about pivot (applied: the object stays at its origin)."""
    m = Matrix.Translation(Vector(pivot)) @ Matrix.Rotation(math.radians(deg), 4, axis) @ Matrix.Translation(-Vector(pivot))
    obj.data.transform(m)
    obj.data.update()
    return obj


# ---- parts ----------------------------------------------------------------------------

def part(obj, kind="plastic", rgba=(0.5, 0.5, 0.5, 1), on=None, gloss=24.0, spec=0.25,
         anim="none", chan="none", pivot=(0, 0, 0), axis=(1, 0, 0), amount=0.0):
    obj["kind"] = kind
    obj["rgba"] = list(rgba)
    obj["on"] = list(on if on else rgba)
    obj["gloss"] = gloss
    obj["spec"] = spec
    obj["anim"] = anim
    obj["chan"] = chan
    obj["pivot"] = list(pivot)
    obj["axis"] = list(axis)
    obj["amount"] = amount
    key = (tuple(round(c, 3) for c in rgba), kind)
    if key not in _MATS:
        m = bpy.data.materials.new(f"m{len(_MATS)}")
        m.diffuse_color = rgba
        m.use_nodes = True
        bsdf = m.node_tree.nodes.get("Principled BSDF")
        if bsdf:
            lin = [c ** 2.2 for c in rgba[:3]]
            bsdf.inputs["Base Color"].default_value = (*lin, 1)
            bsdf.inputs["Roughness"].default_value = max(0.08, min(0.9, 1.0 - math.log(max(gloss, 2), 2) / 8.0))
            if kind in ("light", "screen", "dark"):
                bsdf.inputs["Emission Color"].default_value = (*lin, 1)
                bsdf.inputs["Emission Strength"].default_value = 0.6 if kind != "dark" else 0.0
            if rgba[3] < 1:
                bsdf.inputs["Alpha"].default_value = rgba[3]
                m.blend_method = "BLEND" if hasattr(m, "blend_method") else None
        _MATS[key] = m
    obj.data.materials.clear()
    obj.data.materials.append(_MATS[key])
    return obj


# ---- export ---------------------------------------------------------------------------

def _to_lumen(v):
    return (v[0], v[2], -v[1])


def export(key, out_dir):
    os.makedirs(out_dir, exist_ok=True)
    dg = bpy.context.evaluated_depsgraph_get()
    parts = []
    for obj in bpy.context.scene.objects:
        if obj.type != "MESH" or "kind" not in obj:
            continue
        ev = obj.evaluated_get(dg)
        me = ev.to_mesh()
        me.calc_loop_triangles()
        mw = obj.matrix_world
        nm = mw.to_3x3().inverted().transposed()
        cn = me.corner_normals
        uvl = me.uv_layers.active.data if me.uv_layers.active else None
        kind = obj["kind"]
        verts = []
        paper = []   # printed parts: the faces that do not look towards their own side
        facing = {"label": Vector((0, -1, 0)), "screen": Vector((0, -1, 0)), "back": Vector((0, 1, 0)),
                  "spine": Vector((-1, 0, 0))}.get(kind)
        co = [mw @ v.co for v in me.vertices]
        if kind in ("label", "screen", "back", "spine"):  # projection over the part's bounds
            xs = [c.x for c in co]; ys = [c.y for c in co]; zs = [c.z for c in co]
            x0, x1, y0, y1, z0, z1 = min(xs), max(xs), min(ys), max(ys), min(zs), max(zs)
        for tri in me.loop_triangles:
            dst = verts
            if facing is not None and (nm @ tri.normal).normalized().dot(facing) < 0.5:
                dst = paper
            for li in tri.loops:
                vi = me.loops[li].vertex_index
                p = co[vi]
                n = (nm @ cn[li].vector).normalized()
                if kind in ("label", "screen"):     # seen from the front (-Y)
                    uv = ((p.x - x0) / max(x1 - x0, 1e-6), 1.0 - (p.z - z0) / max(z1 - z0, 1e-6))
                elif kind == "back":                # seen from behind (+Y): mirrored
                    uv = (1.0 - (p.x - x0) / max(x1 - x0, 1e-6), 1.0 - (p.z - z0) / max(z1 - z0, 1e-6))
                elif kind == "spine":               # seen from the left (-X)
                    uv = ((p.y - y0) / max(y1 - y0, 1e-6), 1.0 - (p.z - z0) / max(z1 - z0, 1e-6))
                elif uvl:
                    uv = (uvl[li].uv[0], 1.0 - uvl[li].uv[1])
                else:
                    uv = (0.0, 0.0)
                dst.append((_to_lumen(p), uv, _to_lumen(n)))
        ev.to_mesh_clear()
        parts.append((obj, verts, None))
        if paper:
            parts.append((obj, paper, "paper"))
    # glass last is handled by Lumen; keep scene order otherwise
    path = os.path.join(out_dir, key + ".lmdl")
    with open(path, "wb") as f:
        f.write(b"LMDL" + struct.pack("<II", 1, len(parts)))
        for obj, verts, sub in parts:
            name = (obj.name + ("." + sub if sub else "")).encode()[:47].ljust(48, b"\0")
            pv = _to_lumen(obj["pivot"]); ax = _to_lumen(obj["axis"])
            f.write(name)
            if sub == "paper":   # plain paper (or the part's own "paper" colour)
                pc = list(obj.get("paper", (0.93, 0.92, 0.89, 1.0)))
                f.write(struct.pack("<4f4f2f", *pc, *pc, 8.0, 0.05))
                f.write(struct.pack("<3I", KINDS["plastic"], ANIMS[obj["anim"]], CHANS[obj["chan"]]))
            else:
                f.write(struct.pack("<4f4f2f", *obj["rgba"], *obj["on"], obj["gloss"], obj["spec"]))
                f.write(struct.pack("<3I", KINDS[obj["kind"]], ANIMS[obj["anim"]], CHANS[obj["chan"]]))
            f.write(struct.pack("<3f3ff", *pv, *ax, obj["amount"]))
            f.write(struct.pack("<I", len(verts)))
            for p, uv, n in verts:
                f.write(struct.pack("<8f", *p, *uv, *n))
    nv = sum(len(v) for _, v, _ in parts)
    print(f"[lmdl] {path}: {len(parts)} parts, {nv // 3} triangles")
    try:
        gdir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "build")   # for viewers, not shipped
        os.makedirs(gdir, exist_ok=True)
        bpy.ops.export_scene.gltf(filepath=os.path.join(gdir, key + ".glb"), export_format="GLB",
                                  export_apply=True, export_yup=True)
    except Exception as e:
        print("[lmdl] glb export failed:", e)
    return path


# ---- previews -------------------------------------------------------------------------

def preview(key, out_dir, views=("34", "front", "top", "back"), size=900):
    """Workbench renders with the part colours, studio light, cavity and outline."""
    sc = bpy.context.scene
    sc.render.engine = "BLENDER_WORKBENCH"
    sh = sc.display.shading
    sh.light = "STUDIO"
    sh.color_type = "MATERIAL"
    sh.show_cavity = True
    sh.cavity_type = "BOTH"
    sh.show_specular_highlight = True
    sh.show_object_outline = False
    sc.render.resolution_x = size
    sc.render.resolution_y = int(size * 0.75)
    sc.render.film_transparent = False
    sc.world = sc.world or bpy.data.worlds.new("w")
    sc.world.color = (0.22, 0.23, 0.26)
    sh.background_type = "WORLD"
    # bounds
    lo = Vector((1e9, 1e9, 1e9)); hi = Vector((-1e9, -1e9, -1e9))
    for o in sc.objects:
        if o.type == "MESH":
            for c in o.bound_box:
                w = o.matrix_world @ Vector(c)
                lo = Vector(map(min, lo, w)); hi = Vector(map(max, hi, w))
    ctr = (lo + hi) / 2
    rad = (hi - lo).length / 2
    cam_d = bpy.data.cameras.new("cam")
    cam_d.type = "ORTHO"
    cam_d.ortho_scale = rad * 2.25
    cam_d.clip_end = rad * 20
    cam_d.clip_start = rad * 0.01
    cam = _link(bpy.data.objects.new("cam", cam_d))
    sc.camera = cam
    dirs = {"34": Vector((0.75, -1.0, 0.85)), "front": Vector((0, -1, 0.0001)), "top": Vector((0, -0.0001, 1)),
            "back": Vector((0.0001, 1, 0.25)), "side": Vector((1, 0, 0.05)), "low": Vector((-0.6, -1.0, 0.25))}
    outs = []
    for v in views:
        d = dirs[v].normalized()
        cam.location = ctr + d * rad * 4
        cam.rotation_euler = (-d).to_track_quat("-Z", "Y").to_euler()
        p = os.path.join(out_dir, f"{key}-{v}.png")
        sc.render.filepath = p
        bpy.ops.render.render(write_still=True)
        outs.append(p)
    bpy.data.objects.remove(cam, do_unlink=True)
    return outs


def pose(values):
    """Apply channel values {chan: 0..1} to the parts (object transforms, for previews
    only: call after export). Same motion as Lumen: rotate about pivot / axis by amount
    degrees x value, or move along axis by amount mm x value."""
    for o in bpy.context.scene.objects:
        if o.type != "MESH" or "anim" not in o or o["anim"] == "none":
            continue
        v = values.get(o["chan"], 0.0)
        if o["anim"] == "rot":
            m = (Matrix.Translation(Vector(o["pivot"])) @ Matrix.Rotation(math.radians(o["amount"] * v), 4, Vector(o["axis"]))
                 @ Matrix.Translation(-Vector(o["pivot"])))
        else:
            m = Matrix.Translation(Vector(o["axis"]) * o["amount"] * v)
        o.matrix_world = m


def out_dir_from_argv(default):
    a = sys.argv
    if "--" in a and len(a) > a.index("--") + 1:
        return a[a.index("--") + 1]
    return default
