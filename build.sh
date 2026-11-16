#!/bin/sh
# Build Lumen for the PlayStation Classic inside the PSC toolchain container:
#   ./toolchain/psc lumen/build.sh      -> lumen/out/lumen (+ psc-check report)
set -e
. /opt/psc/bin/psc-env
cd "$(dirname "$0")"
mkdir -p out
rm -f out/lumen
$CC $PSC_FLAGS -O2 -std=gnu11 -Wall -Wno-unused-function -Wno-format-truncation \
   -Ithird_party/include -Ithird_party -I/opt/psc/sysroot/usr/include/SDL2 -D_REENTRANT \
   src/main.c src/i18n.c src/settings.c src/panel.c src/profiles.c src/launch.c src/attract.c \
   src/music.c src/status.c src/stats.c src/welcome.c src/collections.c src/gamemenu.c src/theme.c src/crt.c src/filepicker.c src/transfer.c src/image.c src/ui.c -o out/lumen \
   -lSDL2 -lSDL2_image -lSDL2_ttf -lGLESv2 -lm -lpthread 2>&1 | grep -v "dynsym local symbol" || true
[ -f out/lumen ] || { echo "build failed"; exit 1; }
$STRIP out/lumen
rm -f out/lumen-buttons
$CC $PSC_FLAGS -O2 -std=gnu11 -Wall -Wno-format-truncation src/buttons.c -o out/lumen-buttons -lm
$STRIP out/lumen-buttons
ls -la out/lumen out/lumen-buttons
psc-check out/lumen out/lumen-buttons
