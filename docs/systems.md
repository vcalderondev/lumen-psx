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
