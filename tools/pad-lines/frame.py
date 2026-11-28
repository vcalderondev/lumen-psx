"""Frame a restyled controller drawing in the 480 x 350 box of the illustrations, colour
or drop chosen shapes (by index in document order) and add the credit comment. Line
weights are the drawing's own: nothing is thickened."""
import sys, json
import xml.etree.ElementTree as ET
src, dst, spec = sys.argv[1], sys.argv[2], json.loads(sys.argv[3])
SVG = "http://www.w3.org/2000/svg"; ET.register_namespace("", SVG)
tree = ET.parse(src); root = tree.getroot()
shapes = [e for e in root.iter() if e.tag.split('}')[-1] in ("path", "rect", "circle", "ellipse")]
parent = {c: p for p in root.iter() for c in p}
for k, col in spec.get("line", {}).items(): shapes[int(k)].set("stroke", col)
for k, col in spec.get("fill", {}).items(): shapes[int(k)].set("fill", col); shapes[int(k)].set("stroke", "none")
for k in spec.get("drop", []): parent[shapes[int(k)]].remove(shapes[int(k)])
if "uniform" in spec:   # one line weight for every outline, in pixels of the rendered picture (the reference: the PS4 drawing)
    for e in shapes:
        if e.get("stroke") not in (None, "none") and parent.get(e) is not None:
            e.set("stroke-width", str(spec["uniform"])); e.set("vector-effect", "non-scaling-stroke")
x, y, w, h = [float(v) for v in root.get("viewBox").split()]
if "crop" in spec: x, y, w, h = spec["crop"]   # the drawing inside a larger page, in viewBox units
pad = spec.get("pad", 0.03); W = w * (1 + 2 * pad); H = W * 350.0 / 480.0
if H < h * (1 + 2 * pad): H = h * (1 + 2 * pad); W = H * 480.0 / 350.0
root.set("viewBox", f"{x - (W - w) / 2:.2f} {y - (H - h) / 2:.2f} {W:.2f} {H:.2f}")
root.set("width", "480"); root.set("height", "350")
out = ET.tostring(root, encoding="unicode")
open(dst, "w").write(out.replace(">", ">\n  <!-- " + spec["credit"] + " -->", 1))
print("wrote", dst)
