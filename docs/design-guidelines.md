# Lumen design guidelines

> Superseded (2026-11-15): the source of the design is the
> [lumen-design](https://github.com/vcalderondev/lumen-design) repository (guidelines,
> tokens, a mockup of every screen). This file is kept as the original proposal.

Status: proposal for review (2026-11-14). Rules for every screen of Lumen so new work
stays consistent. Direction: a hybrid of two references the user shared:

- **Cinematic home** (Paweł Durczok, "Playstation 5 Interface Concept", Dribbble
  10181519): moody full-bleed art that owns the screen, a thin status bar, small
  tracked category tabs with an outlined selection, a low row of square tiles with a
  media badge, button hints bottom-right. Palette: deep navy-black (`#040608`,
  `#203549`, `#2D4A5F`), warm neutrals (`#93836E`, `#C3C5BC`), blue `#326399`.
- **Store detail** (PS5 store concept): game clear-logo over the art and a glass
  info card with labelled facts and pill buttons, shown on demand (Square: details). Target: PlayStation Classic, 1280x720 at 60 fps, viewed from a
couch on a TV.

## 1. Principles

1. **The game is the hero.** Art fills the screen; UI sits on top of it and never
   competes with it. One focus at a time.
2. **Glass, not boxes.** Surfaces are translucent tints of white over darkened art,
   with hairline borders and soft shadows. No opaque grey slabs over the art.
3. **Readable from 3 m.** Nothing under 16 px; body 21 px; titles 50 px. Text over
   art always has a scrim or a text shadow.
4. **Calm motion.** Springs, not linear tweens; nothing loops faster than every 6 s.
5. **Restrained colour.** Navy-black, white and one accent; saturation belongs to the
   cover art only. Background art is graded down (section 4).
6. **Cheap by construction.** Every effect must fit the frame budget (section 9);
   effects that need a blur or an extra full-screen pass are faked (pre-blurred
   mips, baked masks).
7. **TV-safe.** Everything important inside the safe area (section 3).

## 2. Tokens

### Colour

| Token | Value | Use |
|---|---|---|
| `bg.base` | `#040608` | opaque sections (settings), clear colour |
| `bg.raised` | `#0D1620` | dialogs, keyboard on the settings section |
| `tint.navy` | `#203549` | hairlines on navy, inactive pill outlines, scrollbars |
| `scrim.uniform` | black, `bg_dim` (default 45 %) | dark layer over all background art |
| `scrim.left` | black 0 -> 70 % over the left 64 % | legibility of the info column |
| `scrim.top` / `scrim.bottom` | black 45 % / 60 % bands | status row / hint bar |
| `glass.fill` | white 8 % (rest), 14 % (card), 24 % (selected pill) | tabs, info card, badges |
| `glass.edge` | white 16 % (rest), 45 % (selected) | 1 px hairline inside glass |
| `text.primary` | `#FAFAFC` | titles, values |
| `text.secondary` | `#E6E6EE` @ 90 % | metadata values |
| `text.muted` | `#D6D6E0` @ 75 % | labels ("Lanzamiento"), hints, captions |
| `accent` | `#3B82F6` (blue, brighter than the shot's `#326399` for contrast on TV) | focus ring, selected-tab outline, progress |
| `accent.alt` | profile colour | avatar, per-profile touches only |
| `danger` | `#FF6B6B` | destructive rows / dialogs only |

One accent per screen. Orange "buy"-style colours from the reference are not used:
the primary action ("Jugar") is a white pill with dark text; the accent is reserved
for focus.

### Type: Outfit (OFL, `assets/fonts`)

| Role | Weight | Size / line | Notes |
|---|---|---|---|
| Hero title | SemiBold 600 | 50 / 56 | game title in the info card; shrink to fit, never wrap |
| Section title | SemiBold 600 | 34 / 40 | settings section header |
| Name / emphasis | Medium 500 | 21 / 26 | profile name, tab labels (19), button labels |
| Body | Regular 400 | 21 / 30 | description (max 3 lines, ellipsis) |
| Label | Regular 400 | 16 / 20 | fact labels; muted colour |
| Tab / hint caps | Medium 500 | 15 / 18, tracking +8 % | the one place uppercase is allowed: category tabs and hint labels (as in the cinematic reference) |
| Value | Medium 500 | 21 / 26 | fact values (date, players, time played) |

Sentence case everywhere (system names keep their own casing). Numbers: tabular
digits are not available in Outfit; align numeric columns left, not right.

### Spacing, radius, elevation

- 4 px grid; common steps 8 / 12 / 16 / 24 / 32 / 48.
- Radius: 14 px tiles, 20 px cards, 999 (pill) tabs/buttons/badges, 50 % avatars.
- Shadows (always soft, black, offset down):
  - `elev.1` tabs, badges: blur 12, y 4, 25-45 %; cut out under translucent shapes.
  - `elev.2` tiles: blur 14, y 6, 42 %; focused tile blur 22, y 10, 60 %.
  - `elev.3` info card, dialogs: blur 28, y 12, 45 %.
- Focus ring: 3 px `accent` (white allowed on very blue art), 4 px gap from the
  element, radius = element radius + gap.

## 3. Layout grid (1280x720)

- Safe area: 84 px left/right, 40 px top, 48 px bottom (TV overscan ~3 %).
- Columns: 12 columns, 24 px gutters inside the safe area (column ~ 72 px).
- Rows (y): status 40-88 · logo/tabs 104-160 · tile row 200-430 · info card
  450-660 · hint bar 672.
- The info column never exceeds 7 columns (~ 640 px) so the right side shows art.

## 4. Home (hybrid)

Two states of the same screen: **Home** (cinematic, default) and **Details** (Square,
or after resting 3 s on a game: the card slides in; any move hides it again).

```
 18:42 ◔                         ◖1 ◖2                     🔔  (P) Principal
 L1 ( TODOS )  LUCHA  ACCIÓN  CARRERAS  PLATAFORMAS   R1        ← categories
                       (art owns the middle of the screen)
       [game clear logo, bottom-left, over the art]
 ┌─────┐ ┌───────┐ ┌─────┐ ┌─────┐ ┌─────┐ ┌─────┐ ┌───
 │     │ │ focus │ │     │ │     │ │     │ │     │ │
 └─────┘ │  ring │ └─────┘ └─────┘ └─────┘ └─────┘ └───
         └───────┘
         Bloody Roar II · 1999 · 3 h 10 min        ○ OPCIONES  □ DETALLES  ✕ JUGAR
```

- **Background:** art cover-fitted, slow drift, darkened with `scrim.uniform` and a
  bottom scrim (the row lives there) instead of the left one. Colour grade (same
  shader pass): desaturate ~35 % and pull the shadows towards `#203549`, so the art
  reads moody and never outshouts the covers. The covers are the only fully
  saturated elements on screen; UI chrome stays white / navy / one accent. No art: generated
  gradient from the cover palette, biased to the navy palette.
- **Status bar (y 40):** left clock (synced only) + network; centre one glyph per
  connected pad, numbered; right avatar + profile name (+ games played).
- **Tabs (y 104):** categories of the current system (all, then genres from the
  metadata) in tracked caps 15 px; the selected one has a 1.5 px accent outline pill;
  L1/R1 small outlined glyphs. Systems move to L2/R2 (or a system pill at the start
  of the row) — to be decided.
- **Game logo:** LaunchBox Clear Logo above the row, left-aligned with the focused
  tile, max 380x110, fades with the selection; absent = the title text instead.
- **Tiles (y ~470):** square covers 150 px, focused 190 px (row anchored ~1/5 from the
  left, previous tiles still visible but dimmed to 60 %, like the shot), radius 10,
  media badge (disc / cartridge / coin) as a small dark circle bottom-right, system
  label under the focused tile.
- **Under the row:** one muted line: title · year · time played.
- **Hint bar:** bottom-right, glyph + tracked caps label (OPCIONES, DETALLES, JUGAR).
- **Details state:** the glass card from the store reference (title, ≤3-line
  description, facts row, pill buttons "✕ Jugar" / "△ Opciones"), left side above
  the row, the row slides down 40 px and dims; Circle closes it.

## 5. Components

| Component | Rest | Focus / selected | Notes |
|---|---|---|---|
| Tab | caps text 65 %, no fill | 1.5 px accent outline pill, text 100 % | height 32, padding 14 |
| Tile | cover, elev.2, 60 % left of focus | 190 px, accent ring, sheen every 6 s, elev.2 strong | badge: disc / cartridge / coin, 28 px dark circle |
| Button (pill) | glass 14 %, edge 20 % | primary: white fill, `#0B0C10` text | glyph 26 px + label Medium 21 |
| Info card | glass 14 %, edge 12 %, elev.3 | — | max 3 description lines |
| Settings row | text primary, separator 5 % | white 10 % rounded highlight | values right-aligned, ‹ › for choices |
| Toast | glass pill above the hint bar | — | 2.5 s |
| Hint bar | glyph 30 px + label 16 muted | — | centred at y 672 |

## 6. Settings (section)

Full-screen opaque `bg.base` section (no art underneath: nothing else is drawn
while it is open). Centred 860 px column: section title, breadcrumb (page name),
hairline, rows of 58 px with separators, footer note. Opens with a 0.25 s fade and
a 34 px rise; closes the same way.

## 7. Motion

| What | How |
|---|---|
| Selection | spring (k 210, c 24) on the row position; tile size follows continuously |
| Title block | fade + 18 px rise, spring (k 170, c 17) |
| Background | 0.6 s smoothstep crossfade; changes queue, never cut |
| Music | in after 0.8 s, 1 s fades, ducked 55 % under settings |
| Section open/close | fade + rise, spring (k 170, c 18) |
| Launch | UI fades in 0.35 s under the field of the game's colour, the flat machine and its medium, fade (no flash) |

## 8. Content and copy

- Sentence case; verbs for actions ("Jugar", "Volver"); no exclamation marks.
- Every string through `tr()` in the six languages; no text baked into images.
- Facts show only what is known (no "Desconocido"); missing description = no line.

## 9. Performance budget (GE8300, 720p, MSAA 2x)

- Frame: ≤ 12 ms GPU. Full-screen passes ≤ 3 (art, next art during crossfades,
  scrim). Draw calls ≤ 80 per frame.
- Glass blur is faked: the background texture gets mipmaps once when uploaded, and
  the card samples it with a LOD bias (one texture fetch), plus the tint.
- Text is cached in textures and only re-rendered when it changes.
- Measure with the `bench` file after each visual change; regressions under 55 fps
  are bugs.

## 10. Data needed for the hybrid

- Genres per game (for the category tabs): LaunchBox `Genres`, mapped to a short
  list (Lucha, Acción, Aventura, Carreras, Plataformas, Rol, Deportes, Puzle,
  Disparos, Otros).
- `logo.png` (Clear Logo) and `description` per game: from LaunchBox Metadata.xml
  (already downloaded: Clear Logo for most of the 86 games, Overview text), written
  by a Mac tool into each game folder (`logo.png`, `description.txt`, Spanish when
  the metadata has it, otherwise English).
