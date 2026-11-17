# Changelog

Format: [Keep a Changelog](https://keepachangelog.com/); versions: [SemVer](https://semver.org/).
Each release zip also lists the psc-platform components it ships (`lumen/VERSION`).

## [0.20.0] - 2026-11-17

Connections: Wi-Fi, Bluetooth controllers and a USB network cable, from RAM, and the
console's kernel: which one runs, and putting Sony's back so all of it works. Verified on
the console: SDL3 runs Lumen; the kernel restore (Project Eris v47 to Sony's) with its full
backup first; on Sony's kernel the network modules load. Not yet tried with real Wi-Fi /
Bluetooth / Ethernet adapters.

### Added
- Settings, Connections: switches for Wi-Fi and Bluetooth, the networks in range (the
  password on the on-screen keyboard, only its WPA hash kept on the stick), Bluetooth
  controllers (search, pair with no PIN, forget), a USB Ethernet adapter that connects by
  itself, and the clock set over the internet. The settings rail fits nine sections.
- Settings, About, Kernel: shows the running kernel (Sony, Project Eris v47, ModMyClassic
  2019, other). On one that is not Sony's, it can put Sony's back from the console's own
  recovery partition: a full read-only firmware backup first (also on its own, Square),
  the current kernel copied to the stick, the write checked and undone at once if it does
  not match, and a way back to the previous kernel. The only place Lumen writes the
  internal memory; while it is being proven on more consoles it needs the file
  `lumen/kernel-write-ok` on the stick. Recovery guide: https://lpsx.dev/recovery
- The on-screen keyboard has a symbols layer (Shift: A, a, #+=) and longer texts.

### Changed
- SDL3 3.4.20 with sdl2-compat runs Lumen on the PSC compositor (psc-platform 2.5.0): the
  same SDL2 calls, newer controller support. If Lumen shows no picture with it, the next
  start uses SDL 2.0.4 again by itself; `sdl=stock` in settings.ini turns it off.
- POWER waits while the kernel is being written.

### Platform
- psc-platform 2.5.0: `psc-sdl3`, `psc-net`, `psc-bt` and `psc-netmod-stock` ship in
  `lumen/compositor/`.

## [0.19.0] - 2026-11-16

Less stutter: at start-up, on the dashboard, with a menu open and in the launch.
Verified on the console through five rounds of test builds (logs with `debug/perf`):
start-up 2072 ms -> about 800 ms, no more frames of 0.2 to 0.9 s when the selection
moves to another system, the settings at 60 fps instead of 31 to 50. Not yet timed on
the console: the game menu with the last change to the cards, and the start-up with the
last two reads moved off the main thread.

### Changed
- Start-up: the library, the launch declarations of every system, the interface's icons
  and the saved selection are read from the stick on a thread while the splash plays, and
  the audio device opens on another. The logo's fade and fill and "Loading" animate at
  the display's rate instead of a few times a second, and the animation no longer adds
  its own 0.65 s to every start. The splash then stays until the covers on screen and the
  background are in (0.7 s at most) and dissolves into a finished dashboard.
- Nothing reads the stick on the main thread while the dashboard runs. A file on the
  stick goes through its FUSE daemon and a question to it waits while the stick is busy:
  the launch declaration read when the selection settled on another system (250 to 860 ms
  on the console), a help bar set's icons the first time it showed, the settings asking
  every frame whether the compositor package exists, the free space of the About page.
- Fonts are read once into memory: a new title or letter no longer reads the stick in
  the middle of a frame.
- An open menu costs the GPU half as much, same picture (to 2/255): its scrim is folded
  into the frame under it by the shaders instead of drawn as a full-screen layer
  (`scrim_fold=0` draws it), and inside a glass card the fill and the shadow under it are
  one layer (`card_merge=0`).
- Launch: the game's art is painted into the illustration and its texts are made once
  the selection settles, on a thread, not on the frame Play is pressed; the field that
  opens the launch hides the dashboard from the GPU as it grows.
- L1 / R1 land on a page whose covers are already there (the next and previous pages
  are decoded ahead; covers stay in a 32 MB cache), and a game's background is ready
  sooner (the last three decoded are kept, the next game's is decoded ahead).
- The help bar's sets are made when first shown (start-up: 722 ms -> 19 ms on the console).
- `debug/perf` logs every frame over 25 ms with what it was made of (`perf: slow
  frame`), where the main thread was when a frame hung over 150 ms (`perf: stuck at`,
  addresses for addr2line on the unstripped build), a line per start-up step (`boot:`),
  how long the dashboard took to be ready under the splash, and the backgrounds' and the
  launch art's times.

## [0.18.0] - 2026-11-16

Native ports of PlayStation games, USB transfer between two sticks, the library check, and
record mode. Checked in psc-sim only: nothing of this release has been verified on the
console yet (released so on purpose: there is no adapter at hand for a second USB drive).
In the simulator: the native ports (listing, chip, launch), the transfer (browse, copy both
ways, stop, the drive vanishing mid-copy, not enough space, the FAT32 4 GB limit, delete,
the offer, Settings) and the library check (a library broken on purpose, LibCrypt images,
a case-sensitive disk). Not verified on the console: detecting and mounting a real second
drive, remounting it writable, power drop-outs on the front ports, a real native port
started from Lumen, and the cost of the native-port launch on the console's GPU.

### Added
- Native ports of a system's games. A game recompiled to run on the console itself (no
  emulator) goes in `<root>/games/<id>-native/<name>/` (`games/psx-native/` for the
  PlayStation; `[library] ports` in the declaration), in the layout of a homebrew app
  (`app.ini`, the program, `cover.png`), and is listed among that system's games like any
  other, not under Homebrew. Its tile carries a "Native port" chip, and its launch is its
  own (lumen-design guidelines/06, "Launch: a native port"): the PlayStation takes the disc
  as always, then charges up: energy runs along its edges, an aura builds, arcs flicker,
  it shakes, rises on a shockwave and the energy floods the frame (3.4 s). A system
  without an energized machine shows the cover as a card that switches itself on. It
  starts as a program (`kind=app`); graphics levels, the CRT filter and attract-mode demos
  do not apply to it.
- Record mode, for videos made in a simulator: `LUMEN_RECORD=<file or fifo>` makes time
  advance exactly `1/LUMEN_RECORD_FPS` (default 60) per presented frame and appends every
  frame as raw RGBA; scripted inputs and screenshots follow that clock;
  `LUMEN_RECORD_SECONDS` ends the run. Normal runs are not affected.
- USB transfer: copy files between the Lumen stick and a second USB drive with the pad
  (`docs/usb-transfer.md`; lumen-design `usb-transfer*.html`). Plugging a second drive in
  shows a pill on the Home for 8 s (Square opens the transfer; nothing opens by itself),
  and Settings, Library has "Transfer files". One card with two panes, one per drive:
  Left / Right change pane, Cross opens a folder or marks a file, Square marks, Start
  copies what is marked to the other drive, Triangle opens the options. The
  destination is the folder open in the other pane or, on the Lumen stick, the games
  folder of any system declared in `systems/`. Both directions.
- The copy runs on its own thread with a progress card (bytes, file count, time left) and
  can be stopped. Free space is checked first; files already on the destination with the
  same size are skipped, so a copy cut short can be repeated. Each file is written as
  `<name>.lumen-part`, flushed and renamed; the second drive is mounted read-only and is
  writable only while it is the destination. A drive that disappears mid-copy ends the
  copy with a message and the unfinished file is removed. After a copy to the Lumen stick
  the library is rescanned.
- Library check: Settings, Library, "Check library" (`docs/library-check.md`; lumen-design
  `library-audit.html`). It reads every system declared in `systems/` and lists, game by
  game, what keeps a game from being listed or from starting: a folder without `Game.ini`,
  a `Game.ini` without `Discs=` or naming a disc that is not there, a `.cue` or `.m3u`
  that points to a missing file (or writes its name with other capitals on a
  case-sensitive stick), empty files, a `.bin` without its `.cue`, an empty game folder, a
  system whose core is missing, a homebrew app without `app.ini` or its program, an
  unfinished copy, an `.sbi` that is not named like its disc, and a LibCrypt-protected
  disc without its `.sbi`: the serial is read from the image (`SYSTEM.CNF`) and looked up
  in `systems/psx.protected` (197 serials, from PSX Data Center's SBI list). The
  native ports folders are checked too, with the rules of homebrew apps. Each problem
  shows the file and the folder. Read-only; the scan runs on its own thread.
- USB transfer, Triangle: options. "Eject USB", and "Delete N items" for what is marked on
  either drive, after a confirmation (Cancel focused). Lumen's own folder cannot be deleted.
- `LUMEN_USB2=<folder>` makes a folder play the second drive (psc-sim) and
  `LUMEN_USB2_KBPS=<n>` slows the copy down, to test the flow without hardware.

## [0.17.0] - 2026-11-15

Interface sounds. Checked in psc-sim (the sim has no audio output: the sounds were reviewed
from `LUMEN_SFX_DUMP`) and on the console.

### Added
- Interface sounds on the dashboard, Settings, the game menu and the keyboard
  (lumen-design guidelines/06 "Sound"): a tick per move, a soft wooden tap for a system
  (higher for the next, lower for the previous), two quick taps for a page, short notes for
  open / back / confirm / play. Synthesised at start like the launch clicks, no sound
  files; up to four at once. Nothing sounds when nothing moved, during the screensaver or
  during the launch.
- Settings, Sound, "Interface sounds" (on by default, setting `ui_sounds`); the Volume of
  that page scales them. The launch click plays either way.
- `LUMEN_SFX_DUMP=<dir>` writes every effect as a WAV, to review them or put them on a video.

## [0.16.0] - 2026-11-15

The first start asks for the language. Checked in psc-sim only; not yet verified on the
console.

### Added
- On a new install (no `settings.ini` yet) the welcome opens with a language page: every
  installed language in its own name, Up / Down to pick, Cross to apply it. The
  presentation that follows is already in the chosen language. The page is shown even when
  the presentation is hidden, and only once: the answer is stored as `language_asked = 1`.
  Existing installs are not asked (a `settings.ini` without the key counts as answered).

### Changed
- The default language is English (`language = en`, was `es`). It is the one focused on
  the language page.

## [0.15.0] - 2026-11-15

Flat launch animations. Checked in psc-sim only; not yet verified on the console.

### Changed
- The launch animations are flat illustrations instead of 3D models (`launch = flat`, the
  design of lumen-design's "Launch: flat"). A field of the game's colour wipes over the
  dashboard, the system's machine drops in and floats, and its medium goes into it: the
  disc under the PlayStation's lid (no jewel case any more), the cartridge into the slot
  of the Nintendo 64, Super Nintendo and Mega Drive, through the front door of the NES,
  into the back of the Game Boy, Game Boy Advance and PSP, the coin into the arcade
  cabinet. A machine with a screen lights it up in the tones of the game's cover and the
  screen grows into the frame; the others light their lamp and hop. No white flash before
  the fade. The cover on the medium is painted in five flat tones of its own palette.
  Every layer is one textured quad: no models, no depth buffer.
- A system without an illustration (homebrew apps, a minimal declaration) gets the generic
  launch on the same field: the game's cover as a card that floats, turns once and grows
  into the frame (`docs/systems.md`, "The launch").

### Removed
- The 3D launch: the jewel and keep cases, the cartridges and consoles as models, the
  coin and "INSERT COIN / CREDIT 1", the white flash, their shaders, the multisampled
  target of the scene and the warm-up of all of it at start-up. `assets/models/` (2.3 MB)
  and `tools/models/` are gone; `src/main.c` is 1,300 lines shorter.
- Declaration keys that only that renderer read are ignored: `[media] shape`, `aspect`,
  `color`, `art`, `media_art` and `[animation] launch`, `slot`, `idle`. A game's own disc
  or cartridge picture is no longer read. The settings `msaa` and `scene_aa` are ignored:
  the window has no multisampling.
- Assets nothing loaded: the pixel-art button icons (`assets/icons/`, superseded by
  `assets/glyphs/`; `tools/make-icons.py`) and the brand's `logo.png` and `mark.png` (their
  SVG sources stay).

## [0.14.1] - 2026-11-15

The POWER button works: it suspends the console and brings the session back.

### Fixed
- POWER suspends the console (red LED) and POWER again resumes the Lumen session; it was
  ignored since 0.3.0 because the session did not come back. Cause: Sony's power manager
  unmounts the USB stick before it suspends, and at the resume the kernel disconnects the
  stick and finds it again a few seconds later, while Lumen's session scripts and its
  button daemon ran from the stick and kept logs open on it.
  Now `boot.sh`, `run.sh`, `power.sh` and `lumen-buttons` run from copies in RAM
  (`/tmp/lumen-run`) and log there (`logsync.sh` appends the logs to the stick);
  `power.sh suspend` closes the game (saved) and the frontend, writes the logs out and
  unmounts the stick; `power.sh resume` waits up to 20 s for the stick (sda1 / sdb1, label
  SONY*), mounts it and only then starts the compositor and the frontend. If the stick
  does not come back the console restarts instead of staying on a black screen.
  Verified on the console with a FAT32 stick (three suspend / resume cycles); not tested
  with an exFAT stick. An empty file `lumen/debug/no-power-suspend` on the stick turns it
  off (POWER ignored, everything runs from the stick as before).
  `lumen/logs/suspend.log` records every suspend and resume step.
- The power LED no longer stays orange during a Lumen session: Sony's start-up leaves the
  red LED on next to the green one and nothing turned it off; `boot.sh` now does.

## [0.14.0] - 2026-11-15

Menus that speak the dashboard's language, and a generic launch animation for systems
whose hardware Lumen does not know. Checked in psc-sim and built with the console
toolchain; not yet verified on the console.

### Added
- Generic launch animation, `launch = cover`: the game's cover turns once about itself
  with a halo of its accent colour and flies into the camera. It is now the fallback for
  every system that declares neither `[animation] launch` nor `[media] shape`, instead of
  a made-up cartridge and console (or a CD case). Homebrew apps use it (they were shown as
  a HuCard going into a PC Engine). Systems with a declared shape keep their hardware
  animation.

### Changed
- Menus redesigned to follow the dashboard's language (design source:
  [lumen-design](https://github.com/vcalderondev/lumen-design)): every menu is a glass card
  over the dimmed game art, the focus is the same white ring as on the tiles, and no
  selection is a filled bar of colour any more.
  - Settings: no longer an opaque full-screen list. The status row stays, the sections are
    a rail on the left and the focused section is already open in a card on the right
    (Cross / Right enter it, Circle / Left go back, Triangle closes). The Profile card
    shows the avatar, the name and the games played.
  - Game menu: a card on the right, so the focused cover and its title stay visible;
    "Play" is a white primary button.
  - File picker, on-screen keyboard and confirm dialogs: same cards, ring and buttons;
    the destructive answer is tinted, never filled.
- `src/ui.c`: the shared components (scrim, card, ring, focus, kicker, text fitting).
- The settings no longer clear the screen to a flat colour: the art keeps drawing under
  them (one more blended full-screen pass; to be measured on the console). Theme keys
  `pill` and `panel` are no longer used by these menus.
- Checked in psc-sim at 720p only; the in-game pause dialog still has the old look.

## [0.13.0] - 2026-11-15

A picture for your profile, picked from the stick, and a dashboard whose focus moves up to
the system tabs and the profile. Checked in psc-sim and built with the console toolchain;
not yet verified on the console.

### Added
- Profile avatar from a picture: Settings > Profile > Avatar opens a file picker on the
  USB stick; the chosen PNG / JPG / BMP / GIF is cropped to its centre square, scaled to
  256 px and saved as `profiles/<id>/avatar.png`. "Remove avatar" goes back to the
  initials.
- Dashboard focus moves up: Up from the games reaches the system tabs (Left / Right change
  the system, Cross or Down go back), Up again the profile (Cross opens Settings >
  Profile).
- File picker (`src/filepicker.c`): a reusable card that browses the stick's folders
  (folders first, files filtered by extension, preview of the highlighted image, last
  folder remembered per use in `picker.<id>`). Previews and the caller's `accept()` run on
  a worker thread, so a 12 MP photo never blocks a frame. `src/image.c` holds the decode /
  crop / scale / atomic PNG write helpers.

### Changed
- D-pad Up / Down no longer change the system directly: they move the dashboard focus.
  L2 / R2 change the system from anywhere, as before.

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
