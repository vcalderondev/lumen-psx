# Changelog

Format: [Keep a Changelog](https://keepachangelog.com/); versions: [SemVer](https://semver.org/).
Each release zip also lists the psc-platform components it ships (`lumen/VERSION`).

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
