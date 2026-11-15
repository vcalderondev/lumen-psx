#!/bin/sh
# Rebuild every 3D model into assets/models/ (needs Blender: brew install --cask blender).
# Previews (PNG, .glb for any viewer) go to tools/models/build/ (not in git).
# The Game Boy needs tools/models/downloads/gb_smoggybeard/model.glb (CC BY 4.0, see
# CREDITS.md; download it from Sketchfab with an account). Without it that model is skipped
# and Lumen falls back to its procedural Game Boy.
set -e
cd "$(dirname "$0")/../.."
OUT="$PWD/assets/models"; PREV="$PWD/tools/models/build"
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
