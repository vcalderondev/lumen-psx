"""Import a downloaded .glb and render it (textures on) for review; prints its objects
and bounds. blender -b -P inspect_glb.py -- <file.glb> <out prefix>"""
import sys, os, bpy
from mathutils import Vector
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lmdl import reset, preview
a = sys.argv[sys.argv.index("--") + 1:]
reset()
bpy.ops.import_scene.gltf(filepath=a[0])
lo = Vector((1e9,) * 3); hi = Vector((-1e9,) * 3); tris = 0
for o in bpy.context.scene.objects:
    if o.type == "MESH":
        for c in o.bound_box:
            w = o.matrix_world @ Vector(c); lo = Vector(map(min, lo, w)); hi = Vector(map(max, hi, w))
        tris += sum(len(p.vertices) - 2 for p in o.data.polygons)
        print("[obj]", o.name, len(o.data.polygons), [m.name for m in o.data.materials])
print("[bounds]", tuple(round(x, 4) for x in (hi - lo)), "tris", tris)
sc = bpy.context.scene
preview(os.path.basename(a[1]), os.path.dirname(a[1]), views=("34", "front", "top", "back", "low"))
sc.display.shading.color_type = "TEXTURE"
preview(os.path.basename(a[1]) + "-tex", os.path.dirname(a[1]), views=("34",))
