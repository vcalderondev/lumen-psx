# Changelog

Format: [Keep a Changelog](https://keepachangelog.com/); versions: [SemVer](https://semver.org/).
Each release zip also lists the psc-platform components it ships (`lumen/VERSION`).

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
