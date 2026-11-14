# Lumen assets

Everything visual that Lumen ships lives here and is installed to
`<home>/assets/` on the console (`/media/lumen/assets/`).

| Path | What |
|---|---|
| `splash.png` | 1280x720 boot screen |
| `icons/<name>.png` | previous pixel-art controller icons (kept as an alternative style), pixel art authored on a 36 px grid and exported x4 (`tools/make-icons.py`); names: cross, circle, triangle, square, dpad, l1, r1, l2, r2, select, start |
| `glyphs/<name>.png` | dashboard glyphs (PS5-style): flat white on transparent at 2x (64 px), tinted in the shader (`tools/make-glyphs.py`); buttons: cross, circle, triangle, square, dpad, l1, r1, l2, r2, select, start; status: pad, network, clock; top: info, gallery, settings; media badges: disc, cartridge, coin |
| `fonts/{regular,medium,semibold,bold}.ttf` | UI font: Outfit (SIL Open Font License, `fonts/OFL.txt`, github.com/Outfitio/Outfit-Fonts); without them the system Liberation Sans is used |
| `systems/<id>/<logo>` | optional tab logo of each declared system (`[system] logo`, usually `logo.png`); a value with a `/` (`psx/logo.png`) shares another system's art |
| `systems/<id>/<sound>` | optional launch sound (`[animation] sound`) |

Lookup order at runtime (the first file that exists wins):

| What | New layout | Old layout (fallback) | Last resort |
|---|---|---|---|
| help-bar icons | `<home>/assets/icons/<name>.png` | `<home>/icons/<name>.png` | text label |
| fonts | `<home>/assets/fonts/{regular,medium,semibold,bold}.ttf` (medium/semibold fall back to bold) | `<home>/fonts/{regular,bold}.ttf` | `/usr/share/fonts/ttf/LiberationSans-{Regular,Bold}.ttf` |
| system logo / sound | `<home>/assets/systems/<id>/<file>` | `<home>/systems/<id>/<file>` | system name as text / none |

System declarations (`<home>/systems/<id>.ini`) and their data files (`*.games`) are not
assets: they stay in `<home>/systems/`.

Third-party art (system logos, game covers, disc images) is not part of the
repository; installers place it under the same tree on the console.
