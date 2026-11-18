#!/bin/sh
# crt-bench.sh [OUT_DIR]: time Lumen's RetroArch CRT shaders the way RetroArch draws them
# (crt-bench.c) on psc-sim's software GPU (Mesa llvmpipe, one thread so the numbers track
# the per-pixel work), and save each frame to compare the looks. Needs psc-sim's Docker
# image (repos/core/psc-sim: bin/psc-sim image). Writes OUT_DIR/results.txt and PNGs.
#   before: crt-lumen-quality.glsl through the viewport FBO + copy pass (the 0.20 presets)
#   after:  crt-lumen.glsl straight to the screen (the current presets)
# and the dashboard's pass (src/crt.c) before (CRT_BENCH_BASE, default 0.20.0) and now.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
SHADERS="$(cd "$HERE/../../dist/shaders" && pwd)"
OUT="$(mkdir -p "${1:-$HERE/out}" && cd "${1:-$HERE/out}" && pwd)"
FRAMES="${CRT_BENCH_FRAMES:-30}"
BASE="${CRT_BENCH_BASE:-6418cd8}"
mkdir -p "$OUT/dash"
git -C "$HERE" show "$BASE:src/crt.c" > "$OUT/dash/crt-before.c"
cp "$HERE/../../src/crt.c" "$OUT/dash/crt-after.c"
docker run --rm -e LP_NUM_THREADS=1 -e FRAMES="$FRAMES" -v "$HERE":/bench:ro -v "$SHADERS":/shaders:ro \
  -v "$OUT":/out psc-sim sh -c '
set -e
cc -O2 -o /tmp/crt-bench /bench/crt-bench.c -lEGL -lGLESv2 -lm
mkdir -p /tmp/f && python3 /bench/make-frames.py /tmp/f
SOFT="0.30 0.16 0.022 0.14 0.22"; STRONG="0.55 0.32 0.045 0.24 0.34"
{
printf "%-10s %-8s %-28s %10s\n" frame level variant ms/frame
for src in 320x240 640x480; do
  for lv in soft strong; do
    [ $lv = soft ] && P="$SOFT" || P="$STRONG"
    run() {  # name mode shader
      ms=$(/tmp/crt-bench $2 $3 /tmp/f/ps1-$src.ppm /tmp/o.ppm $FRAMES "$P")
      python3 -c "from PIL import Image; Image.open(\"/tmp/o.ppm\").save(\"/out/$src-$lv-$1.png\")"
      printf "%-10s %-8s %-28s %10s\n" $src $lv $1 $ms
    }
    run off none -
    run before-quality-fbo fbo /shaders/crt-lumen-quality.glsl
    run quality-direct direct /shaders/crt-lumen-quality.glsl
    run after-fast-direct direct /shaders/crt-lumen.glsl
  done
done
for lv in soft strong; do
  [ $lv = soft ] && P="$SOFT" || P="$STRONG"
  cp /bench/dash-off.vs /tmp/dash-off.vs; cp /bench/dash-off.fs /tmp/dash-off.fs   # no filter: a copy
  for v in off before after; do
    [ $v = off ] || python3 /bench/extract-dash.py /out/dash/crt-$v.c /tmp/dash-$v
    ms=$(/tmp/crt-bench dash /tmp/dash-$v /tmp/f/dash-1280x720.ppm /tmp/o.ppm $FRAMES "$P")
    python3 -c "from PIL import Image; Image.open(\"/tmp/o.ppm\").save(\"/out/dash-$lv-$v.png\")"
    printf "%-10s %-8s %-28s %10s\n" dashboard $lv $v $ms
  done
done
echo
echo "look: after (fast) against the quality maths drawn the same way (direct: the same mask phase), same level"
for src in 320x240 640x480; do for lv in soft strong; do
  printf "%-10s %-8s " $src $lv
  python3 /bench/compare.py /out/$src-$lv-quality-direct.png /out/$src-$lv-after-fast-direct.png /out/$src-$lv-diff-x8.png
done; done
for lv in soft strong; do
  printf "%-10s %-8s " dashboard $lv
  python3 /bench/compare.py /out/dash-$lv-before.png /out/dash-$lv-after.png /out/dash-$lv-diff-x8.png
done
} | tee /out/results.txt
'
