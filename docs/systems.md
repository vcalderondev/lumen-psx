# System declarations

Every system Lumen shows is described by one file, `<home>/systems/<id>.ini`,
and its games live in `<root>/games/<id>/` (e.g. `/media/games/psx/`, `/media/games/snes/`).
Lumen ships no per-system logic: adding a system means adding a file. Assets the
declaration refers to (logos, shells, sounds) live next to it in
`<home>/systems/<id>/`.

## Format

```ini
[system]
id        = psx                 ; unique, file name without .ini
name      = PlayStation         ; fallback text when there is no logo
logo      = logo.png            ; <home>/systems/<id>/logo.png, white on transparent
order     = 10                  ; position of the tab

[library]
layout     = psc                ; psc: <path>/<n>/Game.ini folders | files: one file per game
paths      = /media/games/psx   ; ';'-separated; default: <root>/games/<id>
extensions = cue,pbp,chd,m3u    ; for layout = files (and multi-disc detection)

[launch]
core = pcsx_rearmed_libretro.so ; <home>/cores/<core>

[media]
kind   = disc                   ; disc | cartridge | arcade | card | floppy | tape
shape  = jewel                  ; 3D model: jewel | dvd | cart_snes | cart_md | cart_gba | cart_nes | marquee ...
aspect = 1.136                  ; width / height of the front face
color  = #141217                ; shell / tray colour
art    = cover                  ; image on the front: cover | label | marquee
media_art = disc                ; per-game image of the medium: <game>/disc.png, <game>/cart.png ...

[animation]
launch = open_case_disc         ; open_case_disc | insert_cartridge | insert_coin | ...
idle   = float                  ; float | none
sound  = launch.wav             ; optional, <home>/systems/<id>/launch.wav
```

## Enhanced graphics: `[graphics]`

Optional. A system with this section appears on the settings panel's Graphics page,
where the player chooses **Original** or **Enhanced** (stored as
`graphics.<id>` in `settings.ini`, default `original`).

```ini
[graphics]
core_options_name  = PCSX-ReARMed   ; folder/file name RetroArch uses for the per-core .opt
enhanced           = pcsx_rearmed_neon_enhancement_enable=enabled, pcsx_rearmed_neon_enhancement_no_main=enabled
original           = pcsx_rearmed_neon_enhancement_enable=disabled, pcsx_rearmed_neon_enhancement_no_main=disabled
retroarch_enhanced = video_smooth=true    ; RetroArch overrides; alias: retroarch
retroarch_original = video_smooth=false
```

- Lists are **comma-separated** `key=value` pairs (`;` starts a comment, so it cannot
  separate pairs). Keys are `[A-Za-z0-9_]`; values may not contain `,` `=` `"`.
  Invalid pairs are skipped (and logged) when the launch request is written.
- A system "has graphics" when `enhanced` or `retroarch_enhanced` is non-empty.
- `enhanced` / `original` are core options. At launch the chosen list goes into the
  launch request as `opt.*` lines, and `run.sh` merges them into
  `$LUMEN_CORE_OPT_DIR/<core_options_name>/<core_options_name>.opt`: declared keys are
  replaced, missing keys appended, every other line kept untouched. The first merge keeps
  the original file as `<file>.lumen-bak`. Choosing Original writes the `original` values
  back, so always declare both lists with the same keys. Nothing is merged when
  `LUMEN_CORE_OPT_DIR` (set by `lumen.conf`) or `core_options_name` is empty.
- `retroarch_enhanced` / `retroarch_original` are RetroArch settings. They become `ra.*`
  lines and `run.sh` writes them as `key = "value"` to the extra config handed to
  RetroArch (`$LUMEN_RA_EXTRA`), which `run_retroarch()` must pass with `--appendconfig`:
  RetroArch keeps the *first* copy of a key within one config file, so lines simply
  concatenated after its `retroarch.cfg` would be ignored.
- The merged `.opt` is the per-core file RetroArch already reads, so `LUMEN_CORE_OPT_DIR`
  must be RetroArch's real per-core config folder (its `rgui_config_directory`). No
  `core_options_path` is set. With `game_specific_options` on, a
  `<core_options_name>/<content name>.opt` overrides the preset for that game; `run.sh`
  logs a warning in `session.log` when one exists.

Shipped presets:

| system | Enhanced | Original |
|---|---|---|
| psx | PCSX-ReARMed NEON enhancement (high internal resolution) | enhancement off |
| snes, megadrive, gba, arcade, n64 | `video_smooth=true` (bilinear filter) | `video_smooth=false` |

## Attract-mode demos: `[demo]`

Optional. When the screensaver has been showing for `attract_demo_delay` seconds and
game demos are on (settings `demo_enabled`), Lumen picks a random game of a system
that allows demos (never one of the last 4 demoed) and runs it as an **ephemeral
session**: saves and save states go to `/tmp/lumen-demo` (deleted afterwards), no
history or play time is recorded, and the demo ends after its length or when any
button is pressed (then the carousel opens with that game selected).

```ini
[demo]
allowed = 1        ; games of this system may run as attract-mode demos (default 0)
seconds = 0        ; per-system demo length override; 0 = settings "demo_seconds"
```

Shipped: `psx` and `arcade` allow demos; every other system has `allowed = 0`.
A system also needs a `[launch] core` to be picked.

Unknown keys are ignored, so declarations stay forward-compatible. Every value
has a default; a minimal declaration only needs `[system] id/name`,
`[library]` and `[launch] core`.

## Built-in shapes and animations

| kind | shapes | launch animation |
|---|---|---|
| disc | `jewel` (PS1/CD), `dvd` | `open_case_disc`: the lid opens with the cover, the disc lifts and flies to the camera |
| cartridge | `cart_snes`, `cart_md`, `cart_nes`, `cart_gba`, `cart_gb` | `insert_cartridge`: the cartridge drops into a slot, the power LED lights up |
| arcade | `marquee` (backlit marquee panel) | `insert_coin`: a coin rolls into the slot, "CREDIT 1" blinks, the marquee flickers on |
| card | `hucard` | `insert_cartridge` |

Shapes and animations are code (one renderer each); declarations only choose
and parametrise them.
