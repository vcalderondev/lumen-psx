#!/bin/sh
# Install Lumen onto the console's USB stick as /lumen (MD5-verified, no ._ files, ejects).
#   tools/install.sh [--cores DIR]          binaries + run.sh/boot.sh + data + system declarations
#                                           + assets (+ libretro cores from DIR)
#   tools/install.sh --bg <system>/<game dir> <img>  per-game background -> /games/<system>/<dir>/background.<ext>
# USB mount point: $LUMEN_USB (default /Volumes/SONY)
set -e
cd "$(dirname "$0")/.."
VOL="${LUMEN_USB:-/Volumes/SONY}"
[ -d "$VOL" ] || { echo "USB not mounted at $VOL"; exit 1; }

copy_file() {  # src dst
  mkdir -p "$(dirname "$2")"
  COPYFILE_DISABLE=1 cp -X "$1" "$2"
  [ "$(md5 -q "$1")" = "$(md5 -q "$2")" ] || { echo "MD5 mismatch: $2"; exit 1; }
  echo "ok  $2"
}

if [ "$1" = "--bg" ]; then
  copy_file "$3" "$VOL/games/$2/background.${3##*.}"
  find "$VOL/games/$2" -name '._*' -type f -delete
else
  [ -f out/lumen ] || { echo "build first: out/lumen missing"; exit 1; }
  copy_file out/lumen "$VOL/lumen/lumen"
  [ -f out/lumen-buttons ] && copy_file out/lumen-buttons "$VOL/lumen/lumen-buttons"
  copy_file scripts/run.sh "$VOL/lumen/run.sh"
  copy_file scripts/boot.sh "$VOL/lumen/boot.sh"
  for f in dist/*; do [ -f "$f" ] || continue; copy_file "$f" "$VOL/lumen/$(basename "$f")"; done
  for f in dist/lang/*.lang; do copy_file "$f" "$VOL/lumen/lang/$(basename "$f")"; done
  for f in dist/shaders/*; do [ -f "$f" ] || continue; copy_file "$f" "$VOL/lumen/shaders/$(basename "$f")"; done
  for f in dist/systems/*; do [ -f "$f" ] || continue; copy_file "$f" "$VOL/lumen/systems/$(basename "$f")"; done
  for f in dist/themes/*/*; do [ -f "$f" ] || continue; t=${f#dist/themes/}; mkdir -p "$VOL/lumen/themes/${t%/*}"; copy_file "$f" "$VOL/lumen/themes/$t"; done
  copy_file assets/splash.png "$VOL/lumen/assets/splash.png"
  for d in icons glyphs fonts models; do
    for f in assets/$d/*; do [ -f "$f" ] || continue; copy_file "$f" "$VOL/lumen/assets/$d/$(basename "$f")"; done
  done
  for f in assets/brand/*.png; do copy_file "$f" "$VOL/lumen/assets/brand/$(basename "$f")"; done
  mkdir -p "$VOL/lumen/profiles"
  if [ "$1" = "--cores" ] && [ -d "$2" ]; then
    for f in "$2"/*.so; do copy_file "$f" "$VOL/lumen/cores/$(basename "$f")"; done
  fi
  find "$VOL/lumen" -name '._*' -type f -delete
  [ -e "$VOL/._lumen" ] && rm "$VOL/._lumen"
fi
sync
for i in 1 2 3 4 5; do diskutil eject "$VOL" && break; sleep 2; done
