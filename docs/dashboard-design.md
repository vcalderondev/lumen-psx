# Dashboard redesign (PS5-style home)

Status: approved direction (user, 2026-11-14), replaces the 3D carousel as the main
browsing screen. Reference: a PS5-style home (full-screen game art, tab row, a row
of rounded cover tiles with the selection enlarged, large title + stats, hint bar).
Interactive mockup shown in the session (widget `lumen_ps5_style_dashboard`).

## Why

Measured on the console (self-benchmark, 720p, vsync 60): 3D carousel with all
effects 24 fps (MSAA 4x) / 30 fps (MSAA off); without the 8 side cases 40 fps; only
the centre case 60 fps. Side cases cost per-draw overhead (~8 quads each). A flat
layout needs a handful of draws and runs at 60 fps; 3D is kept for one hero object
and the launch animation.

## Layout (1280x720)

| Area | Content |
|---|---|
| Background | selected game's art full screen (`<game>/background.jpg`, fallback: cover colour field), crossfade 0.6 s, left 58 % darkened for legibility |
| Top-left | profile avatar (image or initials on a colour) + profile name; second line: memory card / game count |
| Top-centre | icons: info, media/gallery, settings |
| Top-right | status: connected controllers (one glyph per pad, active/inactive), network (only when an adapter is up), clock (only when time is known, see below) |
| Tabs (y ~17 %) | `L1` system tabs with logos `R1` (PlayStation, SNES, NES, N64, Mega Drive, GBA, Arcade) |
| Tile row (y ~31 %) | rounded square covers 172 px, selected 224 px with a white focus ring, subtle glass sheen and a media badge (disc / cartridge / coin); row scrolls so the selection stays anchored left |
| Info (y ~62 %) | title (52 px), stats: year, publisher, players, time played (per profile), last played |
| Hint bar (bottom) | `Cross` Play · `L1/R1` Platform · `Select` Settings; right: controller glyphs |

Signature moments: tile focus spring (scale + ring), background crossfade, sheen on
the selected tile; launch: the tile lifts, the disc (label art) slides out and spins
to the centre, flash, fade ("Iniciando…"). Cartridge systems: cartridge drops into a
slot; arcade: coin + "CREDIT 1".

## Render plan (target 60 fps, MSAA off)

- 1 draw: background (art + darkening + vignette in one shader, two textures).
- 1 draw per visible tile (~8), or one instanced/batched draw; rounded corners and
  focus ring antialiased in the fragment shader (no MSAA needed for flat UI).
- Text: cached textures (already cheap).
- 3D only for the launch animation (one case/disc), where MSAA can stay off.

## Profiles, avatars, achievements

- Profiles: name, avatar (`<home>/profiles/<id>/avatar.png` or initials + colour),
  per-profile play time, last played, favourites (settings session owns profiles).
- Achievements: RetroAchievements needs network + an account (RetroArch supports
  it). Offline: local Lumen milestones (hours played, games finished, systems
  played) shown per profile.

## Clock and connectivity

- The PSC has no RTC battery: every boot starts at 2018-09-01. Show the clock only
  when time was synced (NTP when a network adapter is up) or set manually this
  session; otherwise hide it.
- Controllers: count and state from SDL joystick events (USB / Bluetooth).
- Network: show an icon only when `eth0`/`wlan0` is up.

## Icons

Flat, clean vector-style glyphs (PS5-like), not pixel art: button glyphs (cross,
circle, triangle, square, L1/R1, Select/Start), status glyphs (pad, network, clock),
top icons (info, gallery, settings), media badges (disc, cartridge, coin). Exported
as white-on-transparent PNG at 2x size, tinted in the shader.

## Generated background (no background image)

When a game has no `background.jpg|png`, the background shader builds one from
the cover colours (computed when the cover is decoded, worker thread):

- palette: dominant colour A, accent B (most saturated cluster), base = A darkened
- composition: diagonal linear gradient base -> A, plus two soft radial light
  blobs (B top-right, A bottom-left) drifting very slowly (period ~20 s)
- ordered dithering / light grain to avoid banding on 8-bit output
- same single background pass (no extra cost); crossfades like images

## Per-game music

- File: `games/<system>/<game>/music.ogg` (also `.flac` / `.wav`). The console's
  SDL2_mixer 2.0.1 only decodes WAV and FLAC (no OGG/MP3), so OGG is decoded by
  Lumen with `stb_vorbis` (public domain, single file), streamed, looped.
- Behaviour: after the selection settles ~0.8 s the track fades in (1 s) and
  loops; changing game crossfades; launching fades it out with the flash; games
  without a track fall back to the menu theme (`<home>/assets/music/menu.ogg`).
- Settings (per profile): music on/off, volume, preview delay.
- Sources: user-provided files; or extracted from the game's own disc: 11 of the
  86 PS1 games carry their soundtrack as CD-DA tracks (multi-track .cue), which a
  Mac-side tool can rip with ffmpeg (title/menu track -> 90 s loop, OGG q4).
  Single-audio-track discs are usually the "do not play in a CD player" warning.
  Music stored as XA inside the data track needs a dedicated extractor (later).
