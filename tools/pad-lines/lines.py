"""Draw a controller as line art from three flat renders of its 3D model (normals, base
colours, depth; tools: index.html?passes=1): a line wherever the surface turns sharply,
steps in depth, changes colour, or ends (the silhouette). White lines on the dark ink.
   lines.py <passes dir> <out prefix> [line px at 1920 wide]"""
import sys
import numpy as np
from PIL import Image, ImageFilter
src, dst = sys.argv[1], sys.argv[2]
LW = float(sys.argv[3]) if len(sys.argv) > 3 else 3.6
nrm = np.asarray(Image.open(f"{src}/000.png").convert("RGBA"), dtype=np.float32) / 255
alb = np.asarray(Image.open(f"{src}/001.png").convert("RGBA"), dtype=np.float32) / 255
dep = np.asarray(Image.open(f"{src}/002.png").convert("RGBA"), dtype=np.float32) / 255
alpha = nrm[..., 3]
def grad(a):          # largest step to a neighbour, over the channels
    if a.ndim == 2: a = a[..., None]
    g = np.zeros(a.shape[:2], np.float32)
    for dy, dx in ((0, 1), (1, 0), (1, 1), (1, -1)):
        d = np.abs(a - np.roll(a, (dy, dx), (0, 1))).max(-1)
        g = np.maximum(g, d)
    return g
inside = alpha > 0.5
lum = alb[..., :3] @ np.array([0.3, 0.59, 0.11], np.float32)
edge = (grad(nrm[..., :3]) > float(sys.argv[4]) if len(sys.argv) > 4 else grad(nrm[..., :3]) > 0.16)
edge |= grad(dep[..., 0]) > 0.012
edge |= grad(lum) > 0.10
edge |= grad(alpha) > 0.5
edge &= (alpha > 0.02) | (grad(alpha) > 0.5)
m = Image.fromarray((edge * 255).astype(np.uint8))
# the raw edge pixels (1 to 3 px at this size) grown to one line of about LW px
r = max(1, int(round(LW)) - 1); r += (r % 2 == 0)
m = m.filter(ImageFilter.MaxFilter(r)).filter(ImageFilter.GaussianBlur(0.8))
body = Image.fromarray((inside * 255).astype(np.uint8)).filter(ImageFilter.MaxFilter(3))
W, H = m.size
# three layers, put together at each size: the body, the lines, the coloured symbols
sym = Image.new("RGBA", (W, H), (0, 0, 0, 0))
if len(sys.argv) > 5:
    from PIL import ImageDraw
    c = [float(v) for v in sys.argv[5].split(",")]; d = ImageDraw.Draw(sym); s = float(sys.argv[6]) if len(sys.argv) > 6 else 20; w_ = max(2, int(round(LW)))
    (tx, ty), (ox, oy), (xx, xy), (sx, sy) = zip(c[0::2], c[1::2])
    d.line([(tx, ty - s), (tx + s * 0.95, ty + s * 0.68), (tx - s * 0.95, ty + s * 0.68), (tx, ty - s)], fill=(61, 219, 166, 255), width=w_, joint="curve")
    d.ellipse([ox - s * 0.86, oy - s * 0.86, ox + s * 0.86, oy + s * 0.86], outline=(242, 96, 107, 255), width=w_)
    d.line([(xx - s * 0.75, xy - s * 0.75), (xx + s * 0.75, xy + s * 0.75)], fill=(127, 168, 245, 255), width=w_); d.line([(xx + s * 0.75, xy - s * 0.75), (xx - s * 0.75, xy + s * 0.75)], fill=(127, 168, 245, 255), width=w_)
    d.rectangle([sx - s * 0.74, sy - s * 0.74, sx + s * 0.74, sy + s * 0.74], outline=(238, 143, 208, 255), width=w_)
# frame it like the other drawings: the controller 88 % of the width (or of the height)
bb = body.getbbox(); bw, bh = bb[2] - bb[0], bb[3] - bb[1]
FW = max(bw / 0.88, bh / 0.88 * 480 / 350); FH = FW * 350 / 480
cx, cy = (bb[0] + bb[2]) / 2, (bb[1] + bb[3]) / 2
off = (int(round(FW / 2 - cx)), int(round(FH / 2 - cy))); fsize = (int(round(FW)), int(round(FH)))
def framed(layer, mode):
    c_ = Image.new(mode, fsize, 0); c_.paste(layer, off); return c_
body_f, lines_f, sym_f = framed(body, "L"), framed(m, "L"), framed(sym, "RGBA")
# GAIN: how strong the lines are at each size, set so their brightness matches the vector
# drawings' outlines (tools: the lines' median and 90th percentile, measured on the PNGs)
GAIN = {"": float(sys.argv[7]) if len(sys.argv) > 7 else 0.93, "-1080": float(sys.argv[8]) if len(sys.argv) > 8 else 0.76}
for suf, size in (("", (240, 175)), ("-1080", (360, 262))):
    b = body_f.resize(size, Image.LANCZOS); l = lines_f.resize(size, Image.LANCZOS).point(lambda v, g=GAIN[suf]: int(min(255, v * g)))
    out = Image.new("RGBA", size, (0, 0, 0, 0))
    out.paste(Image.new("RGBA", size, (13, 17, 24, 255)), (0, 0), b)
    out.alpha_composite(Image.merge("RGBA", (*Image.new("RGB", size, (244, 244, 247)).split(), l)))
    out.alpha_composite(sym_f.resize(size, Image.LANCZOS))
    out.save(dst + suf + ".png", optimize=True); print(dst + suf + ".png")
