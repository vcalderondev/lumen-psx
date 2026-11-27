#!/bin/sh
# Host unit tests (no console, no GL): tests/run.sh
#   CC (default cc) and SDL2's headers (lumen.h includes them): sdl2-config, pkg-config or
#   SDL2_CFLAGS
set -e
cd "$(dirname "$0")/.."
CC="${CC:-cc}"
SDL="${SDL2_CFLAGS:-$(sdl2-config --cflags 2>/dev/null || pkg-config --cflags sdl2 2>/dev/null || true)}"
OUT="${TMPDIR:-/tmp}/lumen-tests.$$"
mkdir -p "$OUT"
trap 'rm -rf "$OUT"' EXIT
$CC -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Ithird_party/include -Ithird_party $SDL \
   tests/test_module.c src/module.c -o "$OUT/test_module"
"$OUT/test_module"
