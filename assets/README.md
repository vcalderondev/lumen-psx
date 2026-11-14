# Lumen assets

Everything visual that Lumen ships lives here and is installed to
`<home>/assets/` on the console (`/media/lumen/assets/`).

| Path | What |
|---|---|
| `splash.png` | 1280x720 boot screen |
| `icons/<name>.png` | previous pixel-art controller icons (kept as an alternative style), pixel art authored on a 36 px grid and exported x4 (`tools/make-icons.py`); names: cross, circle, triangle, square, dpad, l1, r1, l2, r2, select, start |
| `glyphs/<name>.png` | dashboard glyphs (PS5-style): flat white on transparent at 2x (64 px), tinted in the shader (`tools/make-glyphs.py`); buttons: cross, circle, triangle, square, dpad, l1, r1, l2, r2, select, start; status: pad, network, clock; top: info, gallery, settings; media badges: disc, cartridge, coin |
| `fonts/regular.ttf`, `fonts/bold.ttf` | optional UI fonts (default: the system Liberation Sans) |
| `systems/<id>/logo.png` | optional tab logo for each declared system |

Third-party art (system logos, game covers, disc images) is not part of the
repository; installers place it under the same tree on the console.
