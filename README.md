# Lumen

A lightweight 3D game frontend for the PlayStation Classic.

- 3D jewel-case carousel (OpenGL ES 3.0): spring physics, idle float, lighting,
  floor reflections, ambient colour taken from the selected cover
- platform pills with an animated platform switch (L1/R1)
- "play" animation: the case flips to show its back, zooms in and fades out
- per-game background image (`games/<n>/background.jpg|png`) at ~30 % opacity,
  crossfaded and decoded on a worker thread so browsing never stutters
- launches games in RetroArch and comes back to the same game afterwards
- tiny: a single ~75 KB binary; only covers near the selection stay in memory
- translated UI: Spanish (default), English, Portuguese, French, German, Italian
- game menu on Start: favourites and folders (shown as their own rows), and "Save and exit"
  on OPEN with a "continue where you left off?" question on the next Play
- homebrew apps (`layout = apps`, docs/systems.md) and themes (docs/themes.md)

Target: SDL2 2.0.4, SDL2_image 2.0.1, SDL2_ttf 2.0.14, OpenGL ES 3.2 (PowerVR GE8300),
Weston 1.11 / Wayland, glibc 2.24, Cortex-A35 (armhf), 1 GB RAM.

## What Lumen PSX upgrades on the console

Nothing is written to the console's internal memory: the new graphics stack is unpacked
to RAM at every boot and mounted over `/usr` (overlayfs), and RetroArch and the cores run
from the USB stick. Built from upstream sources by
[psc-platform](https://github.com/vcalderondev/psc-platform).

| Component | Stock console (Sony) | With Project Eris | Lumen PSX | How |
|---|---|---|---|---|
| Wayland (`libwayland-client`, `-server`, `-cursor`) | 1.12 | 1.12 | **1.26.0** | overlay on `/usr` (RAM) |
| wayland-protocols | — | — | **1.45** (1.46 for Weston 16) | build only |
| Compositor | Weston 1.11.0 | Weston 1.11.0 | **Weston 16.0.2** | overlay on `/usr` (RAM) |
| libdrm | 2.4.6x | 2.4.6x | **2.4.124** (with Weston 16 only) | overlay on `/usr` (RAM) |
| libseat | — | — | **0.9.1** (with Weston 16 only) | overlay on `/usr` (RAM) |
| libdisplay-info | — | — | **0.2.0** (static, in Weston 16) | overlay on `/usr` (RAM) |
| RetroArch | — (Sony's own `pcsx`) | 1.8.8 | **1.22.2** | USB (`lumen/retroarch`) |
| PCSX-ReARMed | — | PSC fork (2019) | **upstream** `libretro/pcsx_rearmed` (2026-10) | USB (`lumen/cores`) |
| Joypad profiles | — | RetroArch 1.8.8 set | **v1.22.0** | USB |
| Boot hook | — | Eris' loader | **Lumen's payload** (RAM only) | USB (`d8781dae-…/`) |
| overlayfs, xpad | not in the kernel | flashed Eris kernel | **modules from RAM** (stock kernel) | USB (in the payload) |

Kept from the console, unchanged: Linux 4.4.22, glibc 2.24, the PowerVR GE8300 driver
(DDK 1.9) with its Mesa 17 EGL / GLES / `libgbm` (closed, it cannot be replaced; a gbm
compat shim covers what Weston 16 needs), `libwayland-egl` (the old layout that EGL
expects), SDL 2.0.4, ALSA, udev and libxkbcommon.

## Build

Requires a cross toolchain for the console (GCC targeting armhf / glibc 2.24, linking
against the console's own libraries) exposed as `psc-env` + `psc-check`:

```sh
./toolchain/psc lumen/build.sh      # -> lumen/out/lumen
```

## Install

From a release (no Project Eris, any PlayStation Classic):
1. Format a USB stick FAT32 with the name `SONY`. exFAT works only on consoles already
   modded with Project Eris or BleemSync.
2. Unzip `lumen-psx-vX.Y.Z.zip` from
   [Releases](https://github.com/vcalderondev/lumen-psx/releases) at the root of the stick.
3. Plug it in and power on.

The zip carries Lumen's own boot payload (`d8781dae-…/LDIAG.BIN`, from psc-platform's
`boot/`). Sony's USB hook runs it from the stick: it shows the splash and starts Lumen
from RAM, and writes nothing to the console. To get the original menu back, create an
empty `lumen/disable-boot` on the stick.

From a checkout, onto an existing stick:

```sh
tools/install.sh --cores <dir with *_libretro.so>   # -> USB:/lumen
tools/install.sh --bg psx/22 re3.jpg                # optional per-game background
```

USB layout:

```
/lumen/lumen                  frontend binary
/lumen/boot.sh                boot entry: takes over the display, runs a session,
                              falls back to the stock Sony menu when it ends
/lumen/run.sh                 session loop (frontend <-> RetroArch)
/lumen/lumen.conf             optional: RETROARCH=..., LUMEN_MAIN_SAVES/STATES,
                              LUMEN_CORE_OPT_DIR, run_retroarch() override
/lumen/gamecontrollerdb.txt   controller mappings
/lumen/assets/                runtime art (assets/README.md): splash.png, icons/<name>.png,
                              fonts/{regular,bold}.ttf (optional, default: the system
                              Liberation Sans), systems/<id>/logo.png and sounds;
                              installs that keep icons/, fonts/ and systems/<id>/ art
                              directly under /lumen still work (fallback)
/lumen/cores/*.so             libretro cores (PlayStation: pcsx_rearmed_libretro.so)
/lumen/state.txt, logs/       last selection, logs (lumen.log and buttons.log are fsync'ed
                              at most once per second, at once for error lines, and at
                              exit, so they survive a power-off on the exFAT stick)
/lumen/settings.ini           settings (key=value; created with the defaults on first exit)
/lumen/lang/<code>.lang       UI translations (es, en, pt, fr, de, it)
/lumen/profiles/<id>/         one folder per extra profile: profile.ini, saves/, states/
/lumen/systems/<id>.ini       one declaration per system (docs/systems.md)
/lumen/systems/psxstock.games metadata of the console's built-in games (layout = stock)
/games/<id>/...               one library per system, e.g.
/games/psx/<n>/Game.ini, <disc>.cue, <disc>.png, disc.png, background.jpg
/games/snes/<name>.sfc, <name>.png, <name>-cart.png, <name>-background.jpg
```

At power-on the console's USB loader must hand control to `/lumen/boot.sh`
(it never returns; the loader reboots when it does). `run.sh` alone starts a
session from an already running system.

## Languages and settings

The UI language comes from `language=` in `/lumen/settings.ini` (default `es`); any
`<code>.lang` in `/lumen/lang/` can be chosen. A `.lang` file is UTF-8, one
`key = value` per line, `#` comments, `\n` for a line break, and named placeholders
in braces (`{n}`, `{name}`, `{system}`, `{title}`) that a translation may reorder.
Keys missing in a language fall back to English, then to the key itself.
To add a language, copy `dist/lang/es.lang`, translate the values (including
`lang.name`) and run `tools/check-lang.sh`, which checks that every file has exactly
the Spanish key set and keeps its placeholders.

`settings.ini` holds one `key=value` per line; unknown keys are kept. Keys:
`language`, `profile`, `screensaver_minutes` (0 = off), `demo_enabled`, `demo_seconds`,
`attract_demo_delay`, `graphics.<system id>` (`original` / `enhanced`),
`show_stock_games` (`0`/`1`), `version`.
It is written atomically (temporary file + rename) and only when something changed.

## Enhanced graphics

Settings → Graphics lists every system whose declaration has a `[graphics]` section
(see `docs/systems.md`) and switches it between Original and Enhanced. PlayStation
Enhanced turns on PCSX-ReARMed's high-resolution renderer (some games may run slower);
the other shipped systems smooth the picture with RetroArch's bilinear filter. The
choice is applied at every launch: `run.sh` merges the core options into RetroArch's
per-core `.opt` file (keeping a `.lumen-bak` copy of the original) and passes the
RetroArch overrides in the extra config.

## Profiles

Each profile has its own memory cards and save states. `main` (shown as
"Principal") always exists and keeps the save folders RetroArch already uses
(`LUMEN_MAIN_SAVES` / `LUMEN_MAIN_STATES` in `lumen.conf`; when unset, RetroArch's own
config decides), so existing saves are never lost. Up to 7 more profiles (`p1`…`p9`)
are created in the settings panel; each gets `/lumen/profiles/<id>/profile.ini`
(`name`, `color`, `created`), `saves/` and `states/`, and every game launched under it
starts with an empty memory card. The active profile (`profile=` in `settings.ini`) is
shown at the top right and on the back of the case. Deleting a profile asks for
confirmation and removes its folder with all its saves; `main` cannot be deleted.

Launch request: the frontend exits with code 10 after writing `/tmp/lumen-launch`
as `key=value` lines (`version=2`, `core`, `content`, `system`, `title`, `profile`,
`mode`, `demo_seconds`). `run.sh` parses it (the old two-line form still works),
picks the profile's save folders and writes them to `/tmp/lumen-ra-extra.cfg`, whose
path is exported as `LUMEN_RA_EXTRA`; the default `run_retroarch()` passes it with
`--appendconfig`, and a `lumen.conf` override must pass it with `--appendconfig` too
(RetroArch keeps the first copy of a key within one file, so concatenating it after
`retroarch.cfg` does not work).

## Screensaver and game demos

After `screensaver_minutes` without input (settings panel, Screensaver page; Never
turns it off) the carousel dims into an attract scene, like an arcade cabinet: it
rotates through the games and systems on its own, the ambient light cycles colour and
a localized PRESS START blinks. Any button brings back the game that was selected
before, without launching anything.

With game demos on, after `attract_demo_delay` seconds of attract the frontend picks
a game of a system that allows demos (`[demo] allowed = 1`, see docs/systems.md;
shipped: PlayStation and Arcade) and exits with code **11** after writing a
`mode=demo` launch request. `run.sh` runs it as an ephemeral session: saves and save
states in `/tmp/lumen-demo` (removed afterwards), auto save/load states, history and
play-time logs off, never the profile folders. `lumen-buttons --demo-watch <seconds>
--pid <pid>` stops RetroArch when the demo time (`demo_seconds`, or the system's
`[demo] seconds`) is over or a button is pressed; the frontend then comes back with
`--attract` (the scene resumes) or `--focus-demo` (the carousel, with the demoed game
selected). Without `lumen-buttons` the demo only ends by time. "Preview now" on the
Screensaver page starts the scene at once.

## Controls

| Input | Action |
|---|---|
| D-pad left/right (hold to scroll) | browse |
| L1 / R1 (or D-pad up/down) | previous / next platform |
| Cross, Start | play |
| Triangle, or Select tapped alone | open the settings panel |
| Select, then Start | leave Lumen (opens the stock Sony menu) |

Inside the settings panel: D-pad moves and changes values (left/right), Cross
selects, Circle or Square goes back (closes the panel on the first page), Triangle
closes. The on-screen keyboard (profile names): Cross types, Square erases, Start
accepts, Triangle cancels. On a USB keyboard: arrows, Enter, Tab or F1 (settings),
Backspace (back), Escape (leave).

## Settings panel

Pages: **Language** (applied live, no restart), **Profile** (switch, create, rename,
colour, delete), **Screensaver** (start
after Never/1–30 min, game demos on/off, length of each demo, preview), **Graphics**
(per-system presets; empty until a system declares them), **Library** (built-in games
Hidden / Visible, rescanned live) and **About** (version,
games, systems, profiles, free space on the USB). Changes are written to `settings.ini` when
the panel closes.

## Layout

```
src/main.c, src/gl.h          frontend (src/lumen.h: shared data model and render services)
src/i18n.c, src/settings.c    translation layer, settings store
src/panel.c                   settings panel, on-screen keyboard, confirm dialog
src/profiles.c                profiles (separate memory cards and save states)
src/launch.c                  launch request v2 writer
src/attract.c                 screensaver / attract scene, demo picker
src/buttons.c                 lumen-buttons: console buttons daemon, demo watcher
dist/lang/*.lang              UI translations
scripts/boot.sh               boot entry
scripts/run.sh                session loop
dist/                         data files installed next to the binary
third_party/include/SDL2      SDL_image 2.0.1 / SDL_ttf 2.0.14 headers (zlib licence)
tools/install.sh              USB installer
tools/check-lang.sh           checks the translation files against es.lang
```

## Releases and license

Tagged releases (`vX.Y.Z`, see `CHANGELOG.md`) are built by `.github/workflows/release.yml`:
Lumen with the PSC toolchain, plus the compositor (Wayland 1.26 + Weston), RetroArch 1.22
and the cores from the [psc-platform](https://github.com/vcalderondev/psc-platform)
release pinned in `platform.lock`, packaged as the USB tree (`scripts/package.sh`).

Lumen PSX is free software under the GNU GPL v3.0 or later (`LICENSE`). The Outfit font
is under the SIL Open Font License (`assets/fonts/OFL.txt`).
