#!/bin/sh
# Build Lumen for the PlayStation Classic inside the PSC toolchain container:
#   ./toolchain/psc lumen/build.sh      -> lumen/out/lumen (+ psc-check report)
set -e
. /opt/psc/bin/psc-env
cd "$(dirname "$0")"
mkdir -p out
rm -f out/lumen
$CC $PSC_FLAGS -O2 -std=gnu11 -Wall -Wno-unused-function -Wno-format-truncation \
   -Ithird_party/include -Ithird_party -I/opt/psc/sysroot/usr/include/SDL2 -D_REENTRANT -DLUMEN_ALSA \
   src/main.c src/i18n.c src/settings.c src/panel.c src/profiles.c src/launch.c src/attract.c \
   src/music.c src/status.c src/stats.c src/welcome.c src/collections.c src/gamemenu.c src/theme.c src/crt.c src/filepicker.c src/transfer.c src/audit.c src/net.c src/conn.c src/kern.c src/image.c src/ui.c src/update.c src/padwait.c src/module.c src/mod_net.c -o out/lumen \
   -lSDL2 -lSDL2_image -lSDL2_ttf -lGLESv2 -lasound -lz -lm -lpthread 2>&1 | grep -v "dynsym local symbol" || true
[ -f out/lumen ] || { echo "build failed"; exit 1; }
$STRIP out/lumen
rm -f out/lumen-buttons
$CC $PSC_FLAGS -O2 -std=gnu11 -Wall -Wno-format-truncation src/buttons.c -o out/lumen-buttons -lm
$STRIP out/lumen-buttons
ls -la out/lumen out/lumen-buttons
psc-check out/lumen out/lumen-buttons
# Keep the README's binary size current: the text between <!-- lumen-size --> and
# <!-- /lumen-size --> becomes the stripped size of out/lumen, rounded to KB.
kb=$(( ($(wc -c < out/lumen) + 512) / 1024 ))
if grep -q '<!-- lumen-size -->' README.md 2>/dev/null; then
   sed "s|<!-- lumen-size -->[^<]*<!-- /lumen-size -->|<!-- lumen-size -->~$kb KB<!-- /lumen-size -->|" \
      README.md > out/README.md.tmp
   # rewrite in place (keeps the file's owner and mode) and only when the size changed
   cmp -s out/README.md.tmp README.md || cat out/README.md.tmp > README.md
   rm -f out/README.md.tmp
   echo "README: lumen binary ~$kb KB"
fi
