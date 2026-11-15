#!/bin/sh
# Rebuild every 3D model into assets/models/ (needs Blender: brew install --cask blender).
# Blender exports the full-detail models (.lmdl v1) to tools/models/build/v1/, then
# optimize.py (plain Python) turns them into what Lumen loads (.lmdl v2: parts merged by
# material, decimated within a sub-pixel tolerance, indexed) in assets/models/.
# Previews (PNG, .glb for any viewer) go to tools/models/build/ (not in git).
# Without Blender: the v1 exports of Lumen 0.9.1 are in git, so
#   mkdir -p tools/models/build/v1 && git archive 8172fa2 assets/models | tar -x --strip-components=2 -C tools/models/build/v1
#   python3 tools/models/optimize.py tools/models/build/v1 assets/models
# The Game Boy needs tools/models/downloads/gb_smoggybeard/model.glb (CC BY 4.0, see
# CREDITS.md; download it from Sketchfab with an account). Without it that model is skipped
# and Lumen falls back to its procedural Game Boy.
set -e
cd "$(dirname "$0")/../.."
SHIP="$PWD/assets/models"; PREV="$PWD/tools/models/build"; OUT="$PREV/v1"
mkdir -p "$OUT"
B="${BLENDER:-blender}"
run() { "$B" -b -P "tools/models/$1" -- "$OUT" "$PREV" ${2:-} 2>&1 | grep -E "\[lmdl\]|Traceback|Error" || true; }
run media.py
run n64.py
run megadrive.py
run snes.py
run gba.py
run pce.py
run carts.py
if [ -f tools/models/downloads/gb_smoggybeard/model.glb ] && [ -f tools/models/downloads/nes_miaparcela/model.glb ]; then
  run from_glb.py "console_gb console_nes"
else
  echo "skipped console_gb / console_nes: downloads missing (see CREDITS.md)"
fi
python3 tools/models/optimize.py "$OUT" "$SHIP"
