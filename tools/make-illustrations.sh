#!/bin/sh
# Render the interface illustrations (assets/ui/*.svg) to the PNGs the console draws, with
# headless Chrome like lumen-design's tools/export-launch.sh (same manner of illustration:
# flat planes in the console's greys). <name>.png at 720p (half the SVG's size) and
# <name>-1080.png (0.75x). The SVGs stay in the repo; only the PNGs are packaged.
#   tools/make-illustrations.sh
set -e
cd "$(dirname "$0")/../assets/ui"
CHROME="${CHROME:-/Applications/Google Chrome.app/Contents/MacOS/Google Chrome}"
[ -x "$CHROME" ] || CHROME="$(command -v chromium || command -v google-chrome || true)"
[ -n "$CHROME" ] || { echo "no Chrome / Chromium found (set CHROME)"; exit 1; }
for svg in *.svg; do
  n=${svg%.svg}
  w=$(sed -n 's/.* width="\([0-9]*\)" height.*/\1/p' "$svg" | head -1); h=$(sed -n 's/.* height="\([0-9]*\)".*/\1/p' "$svg" | head -1)
  for v in ":2" "-1080:3"; do   # quarters of the SVG's size: 2/4 at 720p, 3/4 at 1080p
    suf=${v%%:*}; q=${v#*:}
    ow=$((w * q / 4)); oh=$((h * q / 4))
    printf '<html><body style="margin:0;background:transparent"><img src="%s" width="%s" height="%s"></body></html>' "$svg" "$ow" "$oh" > .render.html
    "$CHROME" --headless=new --disable-gpu --hide-scrollbars --allow-file-access-from-files --window-size="$ow,$oh" \
      --default-background-color=00000000 --screenshot="$PWD/$n$suf.png" "file://$PWD/.render.html" >/dev/null 2>&1
    echo "$n$suf.png ${ow}x${oh}"
  done
done
rm -f .render.html
