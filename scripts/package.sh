#!/bin/sh
# Assemble a Lumen PSX release: the USB tree the console runs, as a zip.
#
#   scripts/package.sh <version> <platform dir> <output dir> [<boot dir>]
#
# <boot dir> holds the SIGNED boot payload as it goes on the stick:
#   d8781dae-c45c-4fc1-b961-e03eedebbffb/{diag,LDIAG.BIN}
# (release.yml: psc-platform's .github/actions/boot-payload signs psc-boot-payload.tar.gz;
# locally: psc-platform/boot/build-payload.sh --sign ..., out/boot/stick). Without it the
# zip has no boot payload, which is refused unless LUMEN_ALLOW_NO_PAYLOAD=1.
#
# <platform dir> holds the psc-platform release assets pinned in platform.lock:
#   psc-weston-16.tar.gz psc-retroarch.tar.gz [psc-sdl3.tar.gz]
#   pcsx_rearmed_libretro.so versions.txt
# out/lumen and out/lumen-buttons must be built (build.sh).
#
# Zip layout (unpack at the root of the USB stick, label SONY):
#   lumen/                      frontend, scripts, data, assets
#   lumen/retroarch/            RetroArch 1.22 + Lumen's config + joypad profiles
#   lumen/cores/                libretro cores
#   lumen/compositor/           the PSC compositor packages (Wayland 1.26 + Weston)
#   lumen/logs/                 empty, for the boot payload and Lumen
#   d8781dae-.../{diag,LDIAG.BIN}   Lumen's own boot payload: Sony's usb_watch runs it
#   games/<system>/             empty, one per system in lumen/systems/*.ini
#   lumen/MANIFEST              SHA-256 + size of every file (also lumen-psx-<ver>.manifest)
# No Project Eris: a stock console boots Lumen from a FAT32 stick labelled SONY.
set -e
VER=$1; PF=$2; OUTD=$3; BOOT=$4
ID=d8781dae-c45c-4fc1-b961-e03eedebbffb
[ -n "$VER" ] && [ -d "$PF" ] && [ -n "$OUTD" ] || { echo "usage: $0 <version> <platform dir> <output dir>"; exit 2; }
cd "$(dirname "$0")/.."
for f in out/lumen out/lumen-buttons "$PF/psc-weston-16.tar.gz" "$PF/psc-retroarch.tar.gz" "$PF/pcsx_rearmed_libretro.so"; do
  [ -f "$f" ] || { echo "missing $f"; exit 1; }
done
if [ -n "$BOOT" ]; then
  [ -f "$BOOT/$ID/LDIAG.BIN" ] || { echo "no $BOOT/$ID/LDIAG.BIN"; exit 1; }
elif [ "${LUMEN_ALLOW_NO_PAYLOAD:-0}" != 1 ]; then
  echo "no boot dir: the zip would not boot (set LUMEN_ALLOW_NO_PAYLOAD=1 for a test zip)"; exit 1
fi
ST="$(mktemp -d)"; L="$ST/lumen"
mkdir -p "$L/retroarch/autoconfig" "$L/cores" "$L/compositor" "$L/systems" "$L/lang" "$L/profiles" \
         "$L/assets" "$L/logs"
cp out/lumen out/lumen-buttons scripts/run.sh scripts/boot.sh scripts/power.sh scripts/net.sh scripts/kernel.sh scripts/update.sh psc/lumen.conf "$L/"
chmod 755 "$L/lumen" "$L/lumen-buttons"
for f in dist/*; do [ -f "$f" ] && cp "$f" "$L/"; done
cp dist/systems/* "$L/systems/"
cp dist/lang/*.lang "$L/lang/"
mkdir -p "$L/shaders"; cp dist/shaders/* "$L/shaders/"   # CRT filter presets for RetroArch (run.sh)
mkdir -p "$L/themes"; cp -R dist/themes/. "$L/themes/"   # example themes (docs/themes.md)
cp assets/splash.png assets/splash-1080.png "$L/assets/"
for d in glyphs fonts boot; do mkdir -p "$L/assets/$d"; cp assets/$d/* "$L/assets/$d/"; done
mkdir -p "$L/assets/brand"; cp assets/brand/*.png "$L/assets/brand/"   # the SVGs stay in the repo
mkdir -p "$L/assets/ui"; cp assets/ui/*.png assets/ui/CREDITS.txt "$L/assets/ui/"   # illustrations (tools/make-illustrations.sh) and their credits
mkdir -p "$L/assets/launch"; cp -R assets/launch/. "$L/assets/launch/"   # flat launch illustrations (lumen-design tools/export-launch.sh)
# platform: compositor packages, RetroArch, cores
cp "$PF/psc-weston-16.tar.gz" "$L/compositor/"
# SDL3 + sdl2-compat, overlaid with the compositor (boot.sh); optional until psc-platform ships it
[ -f "$PF/psc-sdl3.tar.gz" ] && cp "$PF/psc-sdl3.tar.gz" "$L/compositor/"
# Wi-Fi / Bluetooth (scripts/net.sh): unpacked to RAM on demand; optional until psc-platform ships them
for p in psc-net.tar.gz psc-bt.tar.gz psc-netmod-stock.tar.gz; do [ -f "$PF/$p" ] && cp "$PF/$p" "$L/compositor/"; done
T="$(mktemp -d)"; tar xzf "$PF/psc-retroarch.tar.gz" -C "$T"
cp "$T/bin/retroarch" "$L/retroarch/retroarch"; chmod 755 "$L/retroarch/retroarch"
cp -R "$T/share/retroarch/autoconfig/udev" "$L/retroarch/autoconfig/"
# The Home / PS button belongs to Lumen (lumen-buttons: it does what OPEN does), so no
# profile may also open RetroArch's menu with it; Start+Select does that (retroarch.cfg).
grep -l '^input_menu_toggle_btn' "$L/retroarch/autoconfig/udev/"*.cfg 2>/dev/null | while IFS= read -r f; do
  sed '/^input_menu_toggle_btn/d' "$f" > "$f.tmp" && mv -f "$f.tmp" "$f"
done
rm -rf "$T"
cp psc/retroarch.cfg "$L/retroarch/retroarch.cfg"
sed -n 's/^retroarch=//p' "$PF/versions.txt" | head -1 > "$L/retroarch/VERSION"
[ -s "$L/retroarch/VERSION" ] || echo "RetroArch 1.22" > "$L/retroarch/VERSION"
cp "$PF"/*_libretro.so "$L/cores/"
# PPSSPP reads its fonts and data from RetroArch's system folder (psc-platform 2.3.0)
if [ -f "$PF/ppsspp-assets.tar.gz" ]; then mkdir -p "$L/retroarch/system"; tar xzf "$PF/ppsspp-assets.tar.gz" -C "$L/retroarch/system"; fi
# crash reporter: RetroArch crashes leave logs/crash.log (symbols: retroarch.debug in the
# psc-platform release)
[ -f "$PF/psc-crash.so" ] && { mkdir -p "$L/debug"; cp "$PF/psc-crash.so" "$L/debug/"; }
# boot payload (signed) + its trigger file, at the stick root
if [ -n "$BOOT" ]; then
  mkdir -p "$ST/$ID"; cp "$BOOT/$ID/LDIAG.BIN" "$ST/$ID/LDIAG.BIN"; : > "$ST/$ID/diag"
fi
# one empty games folder per system (/media/games/<x> in the declarations)
for g in $(sed -n 's|^paths *= */media/games/\([A-Za-z0-9_-]*\).*|\1|p' dist/systems/*.ini | sort -u); do
  mkdir -p "$ST/games/$g"
done
{ echo "Lumen PSX $VER"; echo; cat "$PF/versions.txt"; } > "$L/VERSION"
cp LICENSE "$ST/LICENSE"
cat > "$ST/README-INSTALL.txt" <<EOF
Lumen PSX $VER for the PlayStation Classic: ready to use, no other mod needed

1. Format a USB stick as FAT32 ("MS-DOS (FAT)" on macOS), with the name SONY.
   exFAT also works, but only on a console that already had Project Eris or BleemSync.
2. Unzip this file at the root of the stick: you get d8781dae-.../, lumen/ and games/.
3. Put your games in games/<system>/ (optional: with none, Lumen lists the console's own
   20 games).
4. Plug the stick into the console and power it on.

Nothing is installed on the console: Lumen runs from the stick and from RAM. Without the
stick the console is exactly as it was.

The original PlayStation Classic menu with the stick plugged in: create an empty file
named disable-boot inside lumen/ (delete it to get Lumen back).
If something does not work: lumen/logs/ (payload.log, boot.log, session.log).
Versions of every component: lumen/VERSION. Macs leave hidden ._* files on FAT32
sticks; they are harmless (dot_clean -m /Volumes/SONY removes them).
EOF
mkdir -p "$ST/games"; cat > "$ST/games/README.txt" <<'EOF'
Put your games in the folder of their system (psx, snes, nes, megadrive, gba, n64, arcade);
lumen/systems/<system>.ini lists the file types each one accepts.
EOF
# lumen/MANIFEST: every folder and file of the package with its SHA-256 and size. The web
# installer compares it with the copy left on the stick by the previous install and only
# downloads (zip ranges) and writes what changed, and deletes what a release dropped.
# Published next to the zip as lumen-psx-<ver>.manifest (covered by SHA256SUMS).
#   d <path>                    a folder
#   f <sha256> <bytes> <path>   a file (the manifest itself is not listed)
if command -v sha256sum >/dev/null 2>&1; then H="sha256sum"; else H="shasum -a 256"; fi
(cd "$ST" && {
  echo "# Lumen PSX $VER manifest 1"
  find . -mindepth 1 -type d | sed 's|^\./||' | LC_ALL=C sort | sed 's|^|d |'
  find . -type f ! -path ./lumen/MANIFEST | sed 's|^\./||' | LC_ALL=C sort | while IFS= read -r f; do
    printf 'f %s %s %s\n' "$($H "$f" | cut -c1-64)" "$(wc -c < "$f" | tr -d ' ')" "$f"
  done
} > "$L/MANIFEST")
mkdir -p "$OUTD"
ZIP="$(cd "$OUTD" && pwd)/lumen-psx-$VER.zip"
rm -f "$ZIP"
cp "$L/MANIFEST" "${ZIP%.zip}.manifest"
(cd "$ST" && zip -qr -X "$ZIP" .)
rm -rf "$ST"
echo "$ZIP"
