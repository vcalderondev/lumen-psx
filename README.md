# Lumen

A lightweight 3D game frontend for the PlayStation Classic.

- 3D jewel-case carousel (OpenGL ES 3.0): spring physics, idle float, lighting,
  floor reflections, ambient colour taken from the selected cover
- platform pills with an animated platform switch (L1/R1)
- "play" animation: the case flips to show its back, zooms in and fades out
- per-game background image (`games/<n>/background.jpg|png`) at ~30 % opacity,
  crossfaded and decoded on a worker thread so browsing never stutters
- launches games in RetroArch and comes back to the same game afterwards
- tiny: a single ~45 KB binary; only covers near the selection stay in memory

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
/lumen/lumen.conf             optional: RETROARCH=..., run_retroarch() override
/lumen/gamecontrollerdb.txt   controller mappings
/lumen/fonts/{regular,bold}.ttf   optional, defaults to the system Liberation Sans
/lumen/cores/*.so             libretro cores (PlayStation: pcsx_rearmed_libretro.so)
/lumen/state.txt, logs/       last selection, logs
/lumen/systems/<id>.ini       one declaration per system (docs/systems.md)
/games/<id>/...               one library per system, e.g.
/games/psx/<n>/Game.ini, <disc>.cue, <disc>.png, disc.png, background.jpg
/games/snes/<name>.sfc, <name>.png, <name>-cart.png, <name>-background.jpg
```

At power-on the console's USB loader must hand control to `/lumen/boot.sh`
(it never returns; the loader reboots when it does). `run.sh` alone starts a
session from an already running system.

## Controls

| Input | Action |
|---|---|
| D-pad left/right (hold to scroll) | browse |
| L1 / R1 | previous / next platform |
| Circle, Cross, Start | play |
| Select, then Start | leave Lumen (opens the stock Sony menu) |

## Layout

```
src/main.c, src/gl.h          frontend
scripts/boot.sh               boot entry
scripts/run.sh                session loop
dist/                         data files installed next to the binary
third_party/include/SDL2      SDL_image 2.0.1 / SDL_ttf 2.0.14 headers (zlib licence)
tools/install.sh              USB installer
```
