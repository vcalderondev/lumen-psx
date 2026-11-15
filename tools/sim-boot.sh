#!/bin/sh
# Simulate the console's boot sequence on the desktop and film it: the boot payload's
# splash, the compositor swap, Lumen's own splash, the dashboard. One screenshot every
# 0.2 s, a contact sheet of them, and the list of stretches where the screen was black.
#
#   tools/sim-boot.sh [-l LANG] [-r WxH] [-o OUTDIR] [-b BASE_SRC]
#
#   -l  language written to the simulated stick's settings.ini (default es)
#   -r  output size (default 1920x1080: Lumen at 1080p, as boot.sh sets it up)
#   -o  where the frames go (default <workspace>/sim-boot/out-<lang>)
#   -b  "before" run: build Lumen from this source tree instead and show no boot splash
#       (what a console did before the splash existed), to compare timings
#
# It needs the psc-sim image (<workspace>/psc-sim, `bin/psc-sim image`) and, for the
# splash, a psc-platform checkout (PSC_PLATFORM, default <workspace>/psc-platform): the
# real boot/payload/splash.sh and boot/splash/lumen-splash.c run here, on X instead of
# Wayland. What is NOT simulated is the compositor itself: Sony's Weston stopping and
# psc-weston starting are a pause of SIM_GAP seconds with nothing on screen (default 2.0,
# from console logs: ~0.7 s to stop Weston 1.11, ~1.3 s until Weston 16 has set its
# mode), and the other waits of start / boot.sh are sleeps of their measured length. The
# real length of that gap, and how the TV takes the 720p -> 1080p mode change, can only
# be seen on the console.
set -e
HERE="$(cd "$(dirname "$0")/.." && pwd)"
WS="$(dirname "$HERE")"
PP="${PSC_PLATFORM:-$WS/psc-platform}"
SIMREPO="${PSC_SIM:-$WS/psc-sim}"
LANG_CODE=es; RES=1920x1080; OUT=""; BASE=""
while getopts l:r:o:b: o; do case $o in l) LANG_CODE=$OPTARG ;; r) RES=$OPTARG ;; o) OUT=$OPTARG ;; b) BASE=$OPTARG ;; *) exit 2 ;; esac; done
[ -n "$OUT" ] || OUT="$WS/sim-boot/out-$LANG_CODE${BASE:+-before}"
MEDIA="${SIM_BOOT_MEDIA:-$WS/sim-boot/media}"
mkdir -p "$OUT" "$MEDIA"; rm -f "$OUT"/*.png "$OUT"/*.log "$OUT"/*.txt
SRC="${BASE:-$HERE}"
docker run --rm --network none \
  -v "$SRC":/src:ro -v "$HERE":/new:ro -v "$PP/boot":/boot:ro -v "$SIMREPO/apps/lumen":/app:ro \
  -v "$MEDIA":/media -v "$OUT":/out \
  -e SIM_LANG="$LANG_CODE" -e SIM_RES="$RES" -e SIM_BEFORE="${BASE:+1}" -e SIM_GAP="${SIM_GAP:-2.0}" \
  psc-sim sh -ec '
mkdir -p /tmp/app /tmp/diag/sim/payload/bin
cd /src
gcc -O2 -std=gnu11 -w -Ithird_party -I/usr/include/SDL2 -D_REENTRANT \
  src/main.c src/i18n.c src/settings.c src/panel.c src/profiles.c src/launch.c src/attract.c \
  src/music.c src/status.c src/stats.c src/welcome.c src/collections.c src/gamemenu.c src/theme.c src/crt.c src/filepicker.c src/image.c src/ui.c \
  -o /tmp/app/lumen -lSDL2 -lSDL2_image -lSDL2_ttf -lGLESv2 -lm -lpthread
gcc -O2 -std=gnu99 -Wall -Wextra $(pkg-config --cflags sdl2) -o /tmp/diag/sim/payload/bin/lumen-splash \
  /boot/splash/lumen-splash.c -lSDL2 -ldl
cp /boot/payload/splash.sh /tmp/diag/sim/payload/
# the stick: demo library once (psc-sim seed), then this tree as scripts/package.sh lays it out
[ -d /media/games ] || sh /app/seed.sh
H=/media/lumen
mkdir -p $H/assets/boot $H/assets/models $H/assets/brand $H/shaders $H/themes $H/lang
cp /new/dist/lang/*.lang $H/lang/; cp /new/dist/shaders/* $H/shaders/; cp -R /new/dist/themes/. $H/themes/
cp /new/assets/splash.png /new/assets/splash-1080.png $H/assets/
for d in boot models; do cp /new/assets/$d/* $H/assets/$d/; done
mkdir -p $H/assets/launch; cp -R /new/assets/launch/. $H/assets/launch/
cp /new/assets/brand/*.png $H/assets/brand/
grep -v "^language=" $H/settings.ini > /tmp/s.ini; echo "language=$SIM_LANG" >> /tmp/s.ini; cp /tmp/s.ini $H/settings.ini
rm -f $H/state.txt
python3 /new/tools/sim-boot.py'
echo "frames, sheet.png, timeline.txt: $OUT"
