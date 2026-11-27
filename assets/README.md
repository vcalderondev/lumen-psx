# Lumen assets

Everything visual that Lumen ships lives here and is installed to
`<home>/assets/` on the console (`/media/lumen/assets/`).

| Path | What |
|---|---|
| `splash.png` | 1280x720 boot screen, shown by the boot payload (psc-platform `boot/`, `lumen-splash`) before Lumen starts: black with the splash logo dark (unfilled), exactly the first frame of Lumen's own splash, so the two read as one (`tools/make-brand.py`). boot.sh closes it only while the compositor is swapped |
| `splash-1080.png` | the same boot screen at 1920x1080, for 1080p output |
| `boot/loading-<lang>[-1080].png` | the boot screen's "Loading" text, one per language (`boot.loading` of each `dist/lang/<lang>.lang`, UI font, white on transparent; `tools/make-boot-text.py`): the payload has no font library. It picks the language of `settings.ini`, English if that one has no picture |
| `launch/<key>/` | the launch's illustrations, one folder per machine: `launch.ini` and up to seven PNG layers of one 480x360 canvas (`body`, `under`, `medium`, `over`, `lit`, `tick`, `art`). Exported from the lumen-design repository (`tools/export-launch.sh`), never edited by hand; see `docs/systems.md`, "The launch" |
| `glyphs/<name>.png` | dashboard glyphs (PS5-style): white outlines on transparent at 2x (64 px), tinted in the shader, labels in the UI font (`tools/make-glyphs.py`; `--square` draws a theme's hard-cornered buttons); buttons: cross, circle, triangle, square, dpad, l1, r1, l2, r2, select, start; status: pad, network, clock, wifi-0 to wifi-3, bluetooth, ethernet; top: info, gallery, settings; media badges: disc, cartridge, coin; rows: chevron-right, chevron-down |
| `brand/` | Lumen PSX brand (from lumen-web `public/brand/`, palette and rules in `brand/README.md`, a copy of the site's): `logo-front.svg`, `logo.svg`, `mark.svg` are the sources; `tools/make-brand.py` renders the PNGs the console uses, in their own colours: `logo-front.png` (dashboard top bar, 720p, 3 px per voxel), `logo-front-1080.png` (the same at 1080p), `logo-front-splash[-1080].png` (startup splash, 10 px per voxel at 720p). Only the PNGs are installed |
| `fonts/{regular,medium,semibold,bold}.ttf` | UI font: Outfit (SIL Open Font License, `fonts/OFL.txt`, github.com/Outfitio/Outfit-Fonts); without them the system Liberation Sans is used |
| `systems/<id>/<logo>` | optional tab logo of each declared system (`[system] logo`, usually `logo.png`); a value with a `/` (`psx/logo.png`) shares another system's art |
| `systems/<id>/<sound>` | optional launch sound (`[animation] sound`) |

Lookup order at runtime (the first file that exists wins):

| What | New layout | Old layout (fallback) | Last resort |
|---|---|---|---|
| help-bar icons | `<home>/assets/glyphs/<name>.png` | `<home>/assets/icons/<name>.png`, `<home>/icons/<name>.png` (earlier installs) | text label |
| fonts | `<home>/assets/fonts/{regular,medium,semibold,bold}.ttf` (medium/semibold fall back to bold) | `<home>/fonts/{regular,bold}.ttf` | `/usr/share/fonts/ttf/LiberationSans-{Regular,Bold}.ttf` |
| dashboard logo | `<home>/assets/brand/logo-front-1080.png` at 1080p, else `logo-front.png` | `<home>/brand/…` | no logo |
| system logo / sound | `<home>/assets/systems/<id>/<file>` | `<home>/systems/<id>/<file>` | system name as text / none |

System declarations (`<home>/systems/<id>.ini`) and their data files (`*.games`) are not
assets: they stay in `<home>/systems/`.

Third-party art (system logos, game covers, disc images) is not part of the
repository; installers place it under the same tree on the console.
