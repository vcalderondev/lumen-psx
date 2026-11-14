#!/bin/sh
# Assemble a Lumen PSX release: the USB tree the console runs, as a zip.
#
#   scripts/package.sh <version> <platform dir> <output dir>
#
# <platform dir> holds the psc-platform release assets pinned in platform.lock:
#   psc-weston-6.tar.gz psc-weston-16.tar.gz psc-retroarch.tar.gz
#   pcsx_rearmed_libretro.so versions.txt
# out/lumen and out/lumen-buttons must be built (build.sh).
#
# Zip layout (unpack at the root of the USB stick, label SONY):
#   lumen/                      frontend, scripts, data, assets
#   lumen/retroarch/            RetroArch 1.22 + Lumen's config + joypad profiles
#   lumen/cores/                libretro cores
#   lumen/compositor/           the PSC compositor packages (Wayland 1.26 + Weston)
#   boot/project_eris/bin/execute   the boot hand-off (see README-INSTALL.txt)
set -e
VER=$1; PF=$2; OUTD=$3
[ -n "$VER" ] && [ -d "$PF" ] && [ -n "$OUTD" ] || { echo "usage: $0 <version> <platform dir> <output dir>"; exit 2; }
cd "$(dirname "$0")/.."
for f in out/lumen out/lumen-buttons "$PF/psc-weston-6.tar.gz" "$PF/psc-retroarch.tar.gz" "$PF/pcsx_rearmed_libretro.so"; do
  [ -f "$f" ] || { echo "missing $f"; exit 1; }
done
ST="$(mktemp -d)"; L="$ST/lumen"
mkdir -p "$L/retroarch/autoconfig" "$L/cores" "$L/compositor" "$L/systems" "$L/lang" "$L/profiles" \
         "$L/assets" "$ST/boot/project_eris/bin"
cp out/lumen out/lumen-buttons scripts/run.sh scripts/boot.sh psc/lumen.conf "$L/"
chmod 755 "$L/lumen" "$L/lumen-buttons"
for f in dist/*; do [ -f "$f" ] && cp "$f" "$L/"; done
cp dist/systems/* "$L/systems/"
cp dist/lang/*.lang "$L/lang/"
cp assets/splash.png assets/splash-1080.png "$L/assets/"
for d in icons glyphs fonts; do mkdir -p "$L/assets/$d"; cp assets/$d/* "$L/assets/$d/"; done
# platform: compositor packages, RetroArch, cores
cp "$PF"/psc-weston-*.tar.gz "$L/compositor/"
T="$(mktemp -d)"; tar xzf "$PF/psc-retroarch.tar.gz" -C "$T"
cp "$T/bin/retroarch" "$L/retroarch/retroarch"; chmod 755 "$L/retroarch/retroarch"
cp -R "$T/share/retroarch/autoconfig/udev" "$L/retroarch/autoconfig/"
rm -rf "$T"
cp psc/retroarch.cfg "$L/retroarch/retroarch.cfg"
sed -n 's/^retroarch=//p' "$PF/versions.txt" | head -1 > "$L/retroarch/VERSION"
[ -s "$L/retroarch/VERSION" ] || echo "RetroArch 1.22" > "$L/retroarch/VERSION"
cp "$PF"/*_libretro.so "$L/cores/"
cp psc/execute "$ST/boot/project_eris/bin/execute"
{ echo "Lumen PSX $VER"; echo; cat "$PF/versions.txt"; } > "$L/VERSION"
cp LICENSE "$ST/LICENSE"
cat > "$ST/README-INSTALL.txt" <<EOF
Lumen PSX $VER for the PlayStation Classic

1. USB stick formatted exFAT, label SONY, with Project Eris' boot payload on it (until
   Lumen ships its own payload, Eris' signed loader is what starts it from the USB; set
   USB_ONLY="1" in project_eris/etc/project_eris/CFG/project_eris_cfg.INI so it never
   writes to the console's internal memory).
2. Copy the lumen/ folder to the root of the stick.
3. Copy boot/project_eris/bin/execute over project_eris/bin/execute (keep the original
   as execute.eris).
4. Games go in /games/<system>/...; see lumen/systems/*.ini.

Nothing is written to the console's internal memory: the compositor runs from RAM.
Versions of every component: lumen/VERSION.
EOF
mkdir -p "$OUTD"
ZIP="$(cd "$OUTD" && pwd)/lumen-psx-$VER.zip"
rm -f "$ZIP"
(cd "$ST" && zip -qr -X "$ZIP" .)
rm -rf "$ST"
echo "$ZIP"
