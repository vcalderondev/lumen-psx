# Changelog

Format: [Keep a Changelog](https://keepachangelog.com/); versions: [SemVer](https://semver.org/).
Each release zip also lists the psc-platform components it ships (`lumen/VERSION`).

## [0.12.2] - 2026-11-15

Games start again and no longer stall on a slow stick. psc-platform 2.4.1.

### Fixed
- Every game closed a few seconds after starting (`retroarch exit=139`) since 0.12.0:
  RetroArch crashed on the `GET_STATUS` question of `lumen-buttons`' watchdog. Fixed in
  psc-platform 2.4.1 (`platform.lock`), verified on the console.
- Games on a disc image no longer stop for ~300 ms every couple of seconds on a slow USB
  stick. `run.sh` sets the kernel read-ahead of the stick to 2 MB (default: 128 KB), so
  the image is read ahead in long bursts instead of small reads that each took ~200 ms.
  Measured on the console with Bloody Roar II on a Kingston DT 101 G2: 32 stalls (11.4 s)
  in 98 s before, 4 stalls (1.7 s) in 130 s with the 2 MB read-ahead.

## [0.12.1] - 2026-11-15

Packaging only: the console runs exactly what 0.12.0 runs.

### Added
- Release manifest: `lumen/MANIFEST` in the zip, and `lumen-psx-v0.12.1.manifest` next to
  it (covered by `SHA256SUMS`), list every folder and file of the package with its
  SHA-256 and size. The web installer (lumen-web `/install`) compares it with the copy
  left on the stick by the previous install, downloads only the changed files as byte
  ranges of the zip, deletes the files a release dropped and repairs damaged ones.

## [0.12.0] - 2026-11-15

A boot that shows it is alive: the logo and "Loading" instead of a black screen.
psc-platform 2.4.0. Verified on the console (1080p, Weston 16) on top of 0.9.1; with the
0.10.0 and 0.11.0 changes it has only run in the simulator.

### Added
- Boot: no more long black screen between Sony's animation and the dashboard. The boot
  splash (the logo with dark letters, the first frame of Lumen's own splash) now carries
  "Loading" at the bottom right with three dots that light up in turn, in the language
  set in Settings (`boot.loading` in every `.lang`; `assets/boot/loading-<lang>[-1080].png`
  for the boot payload, made by `tools/make-boot-text.py`). `boot.sh` keeps the payload's
  splash up until the compositor is swapped (the package is unpacked before, not after,
  stopping Sony's Weston), starts it again on the new compositor and closes it when
  Lumen's splash is on screen; Lumen's splash shows the same text and keeps the dots
  moving during the library scan. Needs the boot payload of psc-platform that ships
  `splash.sh`; with an older payload the screen stays black as before.
- `tools/sim-boot.sh`: the whole boot sequence on the desktop simulator, filmed, with the
  black stretches listed.

## [0.11.0] - 2026-11-15

A more responsive dashboard, and antialiasing only where the 3D is. Measured in the
simulator; not yet verified on the console. Three switches in `settings.ini` undo the
riskier parts one by one: `scene_aa=0`, `tile_split=0`, `shader_cache=0`.

### Changed
- A more responsive dashboard at 1080p, same picture. The window has no multisampling
  any more: the launch's 3D is antialiased in its own 4x target (`scene_aa=0` restores
  the window's MSAA) and the disc's and coin's round edges are smooth. A change of
  background is one pass instead of two, covers no longer make the GPU shade the
  background under them (`tile_split=0` to compare), soft shadows are baked once per
  size, the settings section costs a tenth, generated backgrounds redraw 20 times a
  second.
- Nothing waits for the USB stick on the render thread: the log is written by its own
  thread, a decoded background is uploaded a strip per frame.
- Start-up: the library scan lists each folder once (it asked the stick for up to 12
  file names per game) and the linked shaders are cached on the stick
  (`cache/shaders`, `shader_cache=0` to turn it off). `boot:` lines in the log time
  each start-up phase; an empty `debug/perf` file adds frame times, stalls and input
  latency once per second.
- Launching a game with the same graphics preset as last time writes nothing to the stick.

## [0.10.0] - 2026-11-15

Nintendo 64 that plays: lighter core options, a Native graphics level, and a watchdog for
games that hang. psc-platform 2.3.1.

### Added
- Graphics: a third level, **Native**, for systems that declare it (`native_720` /
  `native_1080` in the system's `[graphics]`): the core renders at the display's own
  resolution. Nintendo 64 (960x720 / 1440x1080) and PSP (1440x816 / 1920x1088). Settings
  > Graphics steps Original, Enhanced, Native; systems without it keep two levels.
  Checked in the simulator; not yet verified on the console.
- Nintendo 64 graphics presets (`systems/n64.ini`): Mupen64Plus-Next's defaults ran Super
  Mario 64 slow on the console. Every level now runs GLideN64 on its own thread, without
  colour / depth buffer copies to RDRAM, LOD emulation or hybrid filter, and dupes frames;
  Original is the console's own 320x240 (verified on the console: Super Mario 64 plays
  well, and cleaner than before), Enhanced is 640x480. Core option keys may contain `-`.
- Game watchdog (`lumen-buttons`): a RetroArch whose main loop stopped is closed and the
  dashboard comes back, instead of a console that looks frozen (a hung core answers
  neither RESET, OPEN nor Start+Select). The daemon asks RetroArch `GET_STATUS` every 2 s
  over its command interface: 20 s without an answer (60 s for a game that never answered:
  core and content load before the loop starts) closes it, and RESET on a game that does
  not answer closes it at once. `logs/buttons.log` and `logs/session.log` say so;
  `debug/no-watchdog` turns it off. Tested under ARMv7 emulation (a core hung from the
  start, a game stopped mid-session, a healthy game); not yet verified on the console.
- `logs/session.log` records the size of the launched content (a truncated copy is the
  usual reason a game never starts).

### Fixed
- Nintendo 64: a ROM that never starts (a truncated copy) no longer freezes the console
  (psc-platform 2.3.1: the core gives control back to RetroArch before the game's first
  frame).

## [0.9.2] - 2026-11-15

Lighter launch animations. Measured in the simulator; not yet timed on a console.

### Changed
- Launch animations: several times cheaper for the console's GPU, same picture. The
  solid parts of the 3D models are drawn with blending off (the PowerVR then shades each
  pixel once instead of every layer of every part), with their own small shader (lighting
  per vertex) instead of the general one, and the models ship optimised
  (`tools/models/optimize.py`: parts merged by material, 16-bit indices, decimated within
  0.05 - 0.10 mm; 134,000 -> 104,000 triangles, 402,000 -> 86,000 vertices, 12.6 -> 2.3 MB
  to read from the USB stick at start-up).
- The launch runs on real time down to 10 fps (it ran slow under 20 fps).
- `logs/lumen.log` gets one line per launch with how it ran (`launch: N frames in X s
  (fps), worst frame`).

### Fixed
- Stripes across cartridge labels and case covers in the launch (the print fought with
  the paper behind it for the depth test; the depth buffer is also ten times finer now).

## [0.9.1] - 2026-11-15

### Fixed
- PlayStation, PSP and DVD cases: the cover art no longer shows mirrored on the inside of
  the booklet and through the clear case; the paper's other faces are plain paper.

## [0.9.0] - 2026-11-15

The real hardware in every launch: 3D models of each console, cartridge and case.

### Added
- Launch animations for every system, with the real hardware modelled in 3D (Blender, by
  script, from published measurements, design-patent drawings and photos measured to scale;
  `tools/models/`, no logos or lettering). Lumen loads them from `assets/models/*.lmdl`
  (`src/meshes.inc`) with plastic shading; a missing model falls back to a procedural one.
  - Consoles: Nintendo 64, Super Famicom / PAL Super Nintendo, Mega Drive (model 1), NES,
    PC Engine, Game Boy and Game Boy Advance, each with its moving parts (dust flaps, the
    NES door, power switches, the PC Engine's card lock, LEDs, the handheld screens).
  - Media: N64, Super Famicom, Mega Drive, NES, Game Boy and GBA cartridges, the HuCard,
    the PS1 jewel case, DVD and UMD keep cases, the UMD.
  - Each cartridge goes in the way it really does, as deep as it really does: N64 and Super
    Famicom stand in their top slot, the Mega Drive cartridge in the disc's slot, the NES
    cartridge slides connector first through the door and is pushed down, the HuCard lies in the PC Engine's
    tray, the Game Boy and GBA take it at the back and turn round to light their screen.
- A click the moment the cartridge seats (synthesised; a firm latch on top loaders, a latch
  with a spring on front loaders, a small snap on handhelds), even with the music off.
- `systems/gb.ini` (Game Boy / Color, Gambatte). `[media] shape` is read (it was ignored).

### Changed
- `megadrive.ini` and `snes.ini` declare the real cartridge proportions (109 x 70 and
  128 x 87 mm); `apps.ini` the HuCard's.
- Model assets are installed with the others (`tools/install.sh`, `scripts/package.sh`).

## [0.8.0] - 2026-11-14

Hardware-rendered cores and a CRT look, for the dashboard and the games.

### Added
- PlayStation Portable (`systems/psp.ini`, games in `/media/games/psp`: iso, cso, chd, pbp)
  on PPSSPP, and Nintendo 64 on Mupen64Plus-Next, both drawing with GLES 3 on the console's
  GPU (psc-platform 2.3.0). The release installs PPSSPP's data in `retroarch/system/PPSSPP`.
  Graphics → PSP switches 480x272 (Original) and 960x544 (Enhanced).
- CRT filter (Settings → Display → CRT filter: Off, Soft, Strong; `crt=` in
  `settings.ini`): scanlines, aperture grille, slight curvature, halation and vignette.
  The dashboard draws through one extra GLES 3 pass (`src/crt.c`, MSAA kept, applied
  live, also on the splash, the launch animation and the in-game dialog); games get the
  same look from `lumen/shaders/crt-lumen-*.glslp`, which `run.sh` hands to RetroArch
  (`video_shader_enable` in the extra config, `--set-shader`) for software and GLES 3 cores.
  Launch request: new `crt=` line.

### Changed
- psc-platform 2.3.0 (`platform.lock`).

### Fixed
- In-game dialog: a slow first frame could throw the selection pill off screen (its
  frame step is now capped like the dashboard's).

## [0.7.0] - 2026-11-14

### Added
- Game menu: Start on the dashboard opens a card over the selected game with Play, Add to /
  Remove from favourites and Add to a folder, which drops down the folders (a check on the
  ones holding the game) and "New folder…" (on-screen keyboard). Favourites and every
  folder are rows after the systems (Favourites one press Up from the first system); they
  are per profile (`profiles/<id>/collections.tsv`) and a folder disappears with its last
  game. Cross still plays; the help bar shows Start = Options.
- Save and exit: OPEN in a game adds "Save and exit" to the pause dialog. The state goes to
  RetroArch's slot 9 (`LUMEN_RESUME_SLOT` in `lumen.conf`), `run.sh` checks it was written
  and records the game in `profiles/<id>/resume.tsv`; the next Play on it asks "Continue
  where you left off?" (Continue loads the state with `--entryslot`; Start over forgets it).
- Homebrew apps: `systems/apps.ini` (layout `apps`, `/media/games/apps/<name>/app.ini`)
  lists native programs; `run.sh` runs them from their folder, RESET or OPEN quit them
  (docs/systems.md).
- Themes: `lumen/themes/<id>/theme.ini` colours plus optional fonts, glyphs, menu music
  and system logos, chosen in Settings → Display → Theme and applied by restarting Lumen
  (docs/themes.md, example `dist/themes/sunset`).
- Power LED: green breathes while the dashboard is on screen, solid in a game
  (`led=solid` in `settings.ini` keeps it solid; an on/off-only LED gets a slow beat).
  The console has only green and red LEDs, so there is no blue.

### Changed
- Start no longer launches the selected game on the dashboard (it opens the game menu).
- The restart confirmations (Display, Theme) say "Restart" instead of "Delete" and drop
  the profile-deletion note.

## [0.6.0] - 2026-11-14

The Lumen PSX brand on the console, and a boot that no longer cuts Sony's animation.

### Added
- The Lumen PSX logo (frontal LPSX, one colour per letter) at the top centre of the dashboard,
  entering letter by letter each time the dashboard appears; 720p and 1080p renders in
  `assets/brand/` (`tools/make-brand.py`), installed by the zip and `tools/install.sh`.
- Startup splash: the LPSX logo fades in with dark letters and fills them with colour, voxel
  row by voxel row from L to X, as Lumen loads; then it dissolves into the dashboard. The boot
  screen before it (`assets/splash.png`) stays black, so the logo shows only once.

### Fixed
- Sony's boot animation plays to the end before Lumen starts (psc-platform 2.2.1 boot
  payload), instead of being cut mid-way.

Verified on the console: dashboard logo, splash and boot animation.

## [0.5.0] - 2026-11-14

No Project Eris at all: the release zip is ready to use on any PlayStation Classic.

### Added
- Lumen's own boot payload in the zip (`d8781dae-…/{diag,LDIAG.BIN}`, psc-platform 2.2.0
  `boot/`, signed in the release from a private input). Sony's USB hook runs it from the
  stick, and nothing is written to the console.
  - On a never-modded console it loads overlayfs and xpad as modules from RAM.
  - An empty `lumen/disable-boot` gives the original menu back.
  - Verified on the console: exFAT and FAT32 sticks, the hand-back to Sony's menu, a stick
    set up from scratch from a release zip, and a built-in game through RetroArch 1.22.
- The zip also has `lumen/logs/`, an empty `games/<system>/` per system, and new install
  instructions (FAT32 stick named SONY, unzip, plug in).

### Changed
- A stick with no games of its own lists the console's built-in games, even with
  `show_stock_games=0`, so a fresh install always has something to play.
- `boot.sh` waits up to 5 s (was 1 s) for the buttons daemon. On a cold first start from
  FAT32 it was late, and POWER stayed disabled for that session.

### Removed
- `boot/project_eris/bin/execute` and the Project Eris install steps.

## [0.4.0] - 2026-11-14

### Added
- NES, Super Nintendo, Mega Drive / Genesis, Game Boy / Color and Game Boy Advance cores
  (psc-platform 2.1.0: FCEUmm, Snes9x, Genesis Plus GX, Gambatte, gpSP); NES and SNES
  verified on the console.
- Cartridge launch animation: a 3D top-loading console rises and the game's own
  cartridge photo (`<rom>-cart.png`) drops into its slot, the power LED lights up.

### Changed
- Pressing Play no longer stalls: the launch work is warmed up before the animation and
  its clock never skips frames; less full-screen fill during the transition.
- Game Boy Advance runs on gpSP (ARM dynarec) instead of mGBA.

## [0.3.0] - 2026-11-14

Project Eris' emulator stack is gone: games run on Lumen's own RetroArch 1.22 and an
upstream PCSX-ReARMed, on a PSC compositor built from source.

### Added
- PSC compositor from psc-platform (Wayland 1.26 + Weston 16.0.2, atomic KMS with
  direct scanout), started from RAM; Sony's Weston 1.11 only if it does not start.
- RetroArch 1.22.2 + PCSX-ReARMed from upstream; saves, states, core options and BIOS
  copied once from the Eris install.
- Native KMS: when the PSC compositor cannot start, RetroArch 1.22 takes the display
  directly (no compositor) instead of falling back to RetroArch 1.8.8.
- 1080p output (Settings > Display: 720p / 1080p), direct scanout: 1080p dashboard
  from 24 to ~58 fps.
- First-run presentation of what changed against the stock console, with "don't show
  again".
- L1/R1 change the page of games, L2/R2 change the platform.
- PS5-style dashboard: translucent pill tabs, Outfit font, Settings as a full section,
  dark scrim over the art, MSAA 2x; design guidelines in docs/.
- Console buttons: RESET restarts the game (or reloads the dashboard); OPEN pauses the
  game and opens a dialog: continue, quit, or, in multi-disc games, pick the disc.
- Multi-disc games: the discs of a game folder (Game.ini `Discs=`, else every .cue) are
  given to RetroArch as one playlist, so discs can be swapped in game.
- Crash reporter for RetroArch (`lumen/debug/psc-crash.so`): a crash leaves
  `logs/crash.log`.
- Release workflow: builds Lumen, takes the pinned psc-platform release
  (`platform.lock`) and publishes the USB package with this changelog.

### Known issues
- POWER does nothing during a Lumen session: Sony's suspend to RAM goes to sleep but the
  session does not come back yet. Turn the console off by unplugging it (saves and
  states are written when a game exits). Being worked on.

### Removed
- Project Eris' RetroArch 1.8.8 and its cores (archived to backup_eris/ by the installer
  tools, never deleted).
- The compositor selector (the newest working compositor is used).

### Changed
- Boot screen: plain black until the Lumen PSX mark is designed.
- License: GPL-3.0-or-later.
