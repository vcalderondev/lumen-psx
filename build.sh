#!/bin/sh
# Build Lumen for the PlayStation Classic inside the PSC toolchain container:
#   ./toolchain/psc lumen/build.sh      -> lumen/out/lumen (+ psc-check report)
set -e
. /opt/psc/bin/psc-env
cd "$(dirname "$0")"
mkdir -p out
$CC $PSC_FLAGS -O2 -std=gnu11 -Wall -Wno-unused-function -Wno-format-truncation \
   -Ithird_party/include -I/opt/psc/sysroot/usr/include/SDL2 -D_REENTRANT \
   src/main.c -o out/lumen \
   -lSDL2 -lSDL2_image -lSDL2_ttf -lGLESv2 -lm -lpthread 2>&1 | grep -v "dynsym local symbol" || true
[ -f out/lumen ] || { echo "build failed"; exit 1; }
$STRIP out/lumen
ls -la out/lumen
psc-check out/lumen
