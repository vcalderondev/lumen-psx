"""compare.py REF.ppm TEST.ppm [DIFF.png]: PSNR (dB) and mean / max absolute difference
of TEST against REF; optionally a difference image (x8)."""
import sys, math
from PIL import Image, ImageChops

a = Image.open(sys.argv[1]).convert("RGB")
b = Image.open(sys.argv[2]).convert("RGB")
d = ImageChops.difference(a, b)
px = list(d.getdata())
n = len(px) * 3
se = sum(c * c for p in px for c in p)
mx = max(max(p) for p in px)
mean = sum(c for p in px for c in p) / n
psnr = float("inf") if se == 0 else 10 * math.log10(255 * 255 / (se / n))
print(f"psnr {psnr:.2f} dB, mean |d| {mean:.2f}, max |d| {mx}")
if len(sys.argv) > 3:
    d.point(lambda v: min(255, v * 8)).save(sys.argv[3])
