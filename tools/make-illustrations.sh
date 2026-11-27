#!/bin/sh
# Render the interface illustrations to the PNGs the console draws, with headless Chrome
# like lumen-design's tools/export-launch.sh. <name>.png at 720p (half the SVG's size) and
# <name>-1080.png (0.75x), both into assets/ui. Sources: assets/ui/*.svg (drawn for LPSX)
# and tools/pad-lines/src/*.svg (the controllers of the "pair your controller" card, which
# are PNG only in assets/ui: one of them, pad.png, is not made from an SVG at all, see
# tools/pad-lines). The SVGs stay in the repo; only the PNGs are packaged.
#   tools/make-illustrations.sh
set -e
cd "$(dirname "$0")/.."
OUT="$PWD/assets/ui"
CHROME="${CHROME:-/Applications/Google Chrome.app/Contents/MacOS/Google Chrome}"
[ -x "$CHROME" ] || CHROME="$(command -v chromium || command -v google-chrome || true)"
[ -n "$CHROME" ] || { echo "no Chrome / Chromium found (set CHROME)"; exit 1; }
for svg in assets/ui/*.svg tools/pad-lines/src/*.svg; do
  [ -f "$svg" ] || continue
  n=$(basename "${svg%.svg}"); abs="$PWD/$svg"
  w=$(sed -n 's/.* width="\([0-9]*\)" height.*/\1/p' "$svg" | head -1); h=$(sed -n 's/.* height="\([0-9]*\)".*/\1/p' "$svg" | head -1)
  for v in ":2" "-1080:3"; do   # quarters of the SVG's size: 2/4 at 720p, 3/4 at 1080p
    suf=${v%%:*}; q=${v#*:}
    ow=$((w * q / 4)); oh=$((h * q / 4))
    printf '<html><body style="margin:0;background:transparent"><img src="file://%s" width="%s" height="%s"></body></html>' "$abs" "$ow" "$oh" > "$OUT/.render.html"
    "$CHROME" --headless=new --disable-gpu --hide-scrollbars --allow-file-access-from-files --window-size="$ow,$oh" \
      --default-background-color=00000000 --screenshot="$OUT/$n$suf.png" "file://$OUT/.render.html" >/dev/null 2>&1
    echo "$n$suf.png ${ow}x${oh}"
  done
done
rm -f "$OUT/.render.html"
