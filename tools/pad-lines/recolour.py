"""Restyle a controller drawing for LPSX's dark card WITHOUT touching its line work: every
shape keeps its geometry, its own stroke width and its place in the stack. Only colours
change: outlines become white, filled surfaces the dark ink, and small solid details, dark or light (text,
symbols drawn as filled shapes), white. Text elements, gradients, filters, metadata go."""
import re, sys, colorsys
import xml.etree.ElementTree as ET
src, dst = sys.argv[1], sys.argv[2]
SVG = "http://www.w3.org/2000/svg"; ET.register_namespace("", SVG)
INK, LINE = "#0d1118", "#f4f4f7"
tree = ET.parse(src); root = tree.getroot()
# CSS classes (CorelDRAW exports): ".name {prop:value;...}" in a <style> block
CSS = {}
for st in root.iter("{%s}style" % SVG):
    for name, body in re.findall(r"\.([\w-]+)\s*\{([^}]*)\}", st.text or ""):
        CSS[name] = dict((k.strip(), v.strip()) for k, v in (d.split(":", 1) for d in body.split(";") if ":" in d))
# optional: the shapes' boxes (index cx cy w h, 0..1 of the drawing, document order): a solid dark
# shape counts as a detail (white) only when it is small; a large one is a dark surface
BOX = {}
if len(sys.argv) > 3:
    for line in open(sys.argv[3]):
        f = line.split()
        if len(f) == 5 and f[0].isdigit(): BOX[int(f[0])] = (float(f[3]), float(f[4]))
count = [0]
def local(t): return t.split('}')[-1]
if not root.get("viewBox"):
    w = float(re.sub(r"[^0-9.]", "", root.get("width", "100")) or 100); h = float(re.sub(r"[^0-9.]", "", root.get("height", "100")) or 100)
    root.set("viewBox", f"0 0 {w} {h}")
NAMED = {"black": (0, 0, 0), "white": (1, 1, 1), "none": None, "red": (1, 0, 0), "gray": (.5, .5, .5), "grey": (.5, .5, .5)}
def rgb(v):
    v = (v or "").strip().lower()
    if v in NAMED: return NAMED[v]
    m = re.fullmatch(r"#([0-9a-f]{3})", v)
    if m: return tuple(int(c * 2, 16) / 255 for c in m.group(1))
    m = re.fullmatch(r"#([0-9a-f]{6})", v)
    if m: return tuple(int(m.group(1)[i:i + 2], 16) / 255 for i in (0, 2, 4))
    m = re.fullmatch(r"rgb\(([^)]+)\)", v)
    if m: return tuple(float(x) / 255 for x in m.group(1).split(","))
    return (0.5, 0.5, 0.5) if v.startswith("url(") else None     # a gradient: some surface
def props(el, inherited):
    p = dict(inherited)
    for cls in (el.get("class") or "").split(): p.update(CSS.get(cls, {}))
    for k in ("fill", "stroke", "stroke-width", "opacity", "fill-opacity", "stroke-opacity", "display"):
        if el.get(k) is not None: p[k] = el.get(k)
    for part in (el.get("style") or "").split(";"):
        if ":" in part:
            k, v = part.split(":", 1); p[k.strip()] = v.strip()
    return p
def symbol_colour(c):
    """a saturated colour of the drawing (a face symbol, a light) in LPSX's own palette, else None"""
    h, l, s = colorsys.rgb_to_hls(*c)
    if s < 0.35: return None
    h *= 360
    if 80 <= h < 200: return "#3ddba6"                      # triangle: green / teal
    if 200 <= h < 270: return "#7fa8f5"                     # cross, lights: blue
    if h >= 300 or h < 20: return "#ee8fd0" if l > 0.68 else "#f2606b"   # square: pink; circle: red
    return "#f5c518"
stats = {"lines": 0, "surfaces": 0, "details": 0, "dropped": 0}
def walk(el, inherited):
    for ch in list(el):
        n = local(ch.tag)
        if n in ("text", "defs", "metadata", "namedview", "script", "image", "style", "title", "desc", "filter", "linearGradient", "radialGradient", "flowRoot") or "sodipodi" in ch.tag or "inkscape" in ch.tag:
            el.remove(ch); continue
        p = props(ch, inherited)
        for a in list(ch.attrib):
            if "}" in a and not a.startswith("{" + SVG): del ch.attrib[a]
        if p.get("display") == "none": el.remove(ch); stats["dropped"] += 1; continue
        if n in ("path", "rect", "circle", "ellipse", "polygon", "polyline", "line"):
            idx = count[0]; count[0] += 1 if n in ("path", "rect", "circle", "ellipse") else 0
            small = BOX.get(idx, (0, 0)); small = small[0] < 0.085 and small[1] < 0.125
            for a in ("style", "fill", "stroke", "opacity", "filter", "class", "clip-path", "mask", "fill-opacity", "stroke-opacity"): ch.attrib.pop(a, None)
            f, s = rgb(p.get("fill", "black")), rgb(p.get("stroke", "none"))
            fo = float(p.get("fill-opacity", 1)) * float(p.get("opacity", 1))
            if s is not None:
                ch.set("stroke", symbol_colour(s) or LINE); ch.set("stroke-width", p.get("stroke-width", "1")); ch.set("stroke-linejoin", "round"); ch.set("stroke-linecap", "round")
                ch.set("fill", "none" if f is None or fo < 0.05 else INK); stats["lines"] += 1
            elif f is None or fo < 0.05: el.remove(ch); stats["dropped"] += 1; continue
            elif symbol_colour(f): ch.set("fill", symbol_colour(f)); stats["details"] += 1
            elif small and fo > 0.6 and (sum(f) / 3 < 0.30 or sum(f) / 3 > 0.70) and max(f) - min(f) < 0.25: ch.set("fill", LINE); stats["details"] += 1      # solid dark detail: ink on the original
            else: ch.set("fill", INK); stats["surfaces"] += 1
        elif n == "g":
            for a in ("style", "fill", "stroke", "opacity", "filter", "clip-path", "mask"): ch.attrib.pop(a, None)
        walk(ch, p)
walk(root, {})
for a in list(root.attrib):
    if "}" in a: del root.attrib[a]
tree.write(dst, encoding="unicode")
print(dst, stats)
