# Changelog

Format: [Keep a Changelog](https://keepachangelog.com/); versions: [SemVer](https://semver.org/).
Each release zip also lists the psc-platform components it ships (`lumen/VERSION`).

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
