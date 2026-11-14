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

Target: SDL2 2.0.4, SDL2_image 2.0.1, SDL2_ttf 2.0.14, OpenGL ES 3.2 (PowerVR GE8300),
Weston 1.11 / Wayland, glibc 2.24, Cortex-A35 (armhf), 1 GB RAM.

## Build

Requires a cross toolchain for the console (GCC targeting armhf / glibc 2.24, linking
against the console's own libraries) exposed as `psc-env` + `psc-check`:

```sh
./toolchain/psc lumen/build.sh      # -> lumen/out/lumen
```

## Install

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
/lumen/fonts/{regular,bold}.ttf   optional, defaults to the system Liberation Sans
/lumen/cores/*.so             libretro cores (PlayStation: pcsx_rearmed_libretro.so)
/lumen/state.txt, logs/       last selection, logs
/lumen/settings.ini           settings (key=value; created with the defaults on first exit)
/lumen/lang/<code>.lang       UI translations (es, en, pt, fr, de, it)
/lumen/profiles/<id>/         one folder per extra profile: profile.ini, saves/, states/
/lumen/systems/<id>.ini       one declaration per system (docs/systems.md)
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
`attract_demo_delay`, `graphics.<system id>` (`original` / `enhanced`), `version`.
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
`--appendconfig`, and a `lumen.conf` override must append it to its config too.

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
(per-system presets; empty until a system declares them) and **About** (version,
games, systems, profiles, free space on the USB). Changes are written to `settings.ini` when
the panel closes.

## Layout

```
src/main.c, src/gl.h          frontend (src/lumen.h: shared data model and render services)
src/i18n.c, src/settings.c    translation layer, settings store
src/panel.c                   settings panel, on-screen keyboard, confirm dialog
src/profiles.c                profiles (separate memory cards and save states)
src/launch.c                  launch request v2 writer
dist/lang/*.lang              UI translations
scripts/boot.sh               boot entry
scripts/run.sh                session loop
dist/                         data files installed next to the binary
third_party/include/SDL2      SDL_image 2.0.1 / SDL_ttf 2.0.14 headers (zlib licence)
tools/install.sh              USB installer
tools/check-lang.sh           checks the translation files against es.lang
```
