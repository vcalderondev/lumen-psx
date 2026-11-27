# Themes

A theme changes Lumen's colours and, optionally, any of its runtime art: fonts, button
glyphs, the menu music, the dashboard background. Settings → Display → Theme picks one
(`theme=<id>` in `settings.ini`); "Apply the theme" restarts Lumen (not the console),
because fonts and art are loaded at start.

```
/lumen/themes/<id>/theme.ini     required; <id>: lower case, digits, '-' and '_'
/lumen/themes/<id>/fonts/        regular.ttf, medium.ttf, semibold.ttf, bold.ttf (any of them)
/lumen/themes/<id>/glyphs/       glyphs, same names as assets/glyphs (cross.png, l1.png, pad.png ...)
/lumen/themes/<id>/music/menu.ogg
/lumen/themes/<id>/systems/<system>/logo.png   per-system logos
```

Any file a theme has wins over the one in `/lumen/assets/` with the same relative path;
everything else comes from `/lumen/assets/` as usual. `default` is Lumen's own look and
needs no folder.

## theme.ini

```ini
name       = Sunset           ; shown in Settings (default: the folder name)
author     = you
accent     = #FF9F45          ; checks, the text cursor, highlights
lit        = #FF9F45          ; the light: glow of the focused cover, bar under the selected system
                              ; (default: none = the accent of each game's cover)
focus      = #FFD2A6          ; the focus ring of every screen (default: white)
glass      = #FFE4D2          ; tint of the glass shapes: buttons, tabs, keys, the focused row (default: white)
primary    = #FF9F45          ; fill of the main button: Play, the keyboard's OK (default: white)
on_primary = #2A1206          ; text on the main button (default: near black)
card       = #2A1A1E          ; menu / dialog card
card_alpha = 0.94             ; how solid the card colour is under the glass: 0.2 .. 1 (1 = opaque)
radius     = 1                ; corner rounding of every shape: 1 = Lumen's, 0 = square corners
text       = #FFF6EE          ; primary text
dim        = #E2C3B4          ; secondary text
backdrop   = #120A0C          ; dialog backdrop and scrims
panel      = #1A0F12          ; settings panel background
danger     = #FF7070          ; destructive rows (Exit the game)
```

Colours are `#RRGGBB` or `r, g, b` (0..255, or 0..1). Missing keys keep Lumen's colours.
Every screen draws with the same tokens: the dashboard's tabs and focus rings, the game
menu, the settings, the keyboard, the questions and the in-game pause dialog. Selection is
always glass with the `focus` ring; `pill` (a filled selection) is no longer read.
Keep `focus` and `on_primary` readable: the ring sits on dark art, the text on `primary`.
`radius` scales the corners of every card, row, tile, pill and shadow at once (avatars and
discs stay round): `0.5` halves them, `0` gives the hard edges of a 1990s console menu.

Three themes ship with Lumen:

- `dist/themes/mono`: pure black and white for OLED and CRT sets; the light is white for
  every game, corners are half as round.

- `dist/themes/sunset`: colours only, a complete `theme.ini`.
- `dist/themes/retro`: a 1990s console look. The grey of the original console's shell,
  square corners (`radius = 0`), the four colours of its logo (yellow focus, blue main
  button), its own typeface in `fonts/` (Barlow, SIL Open Font License: `fonts/OFL.txt`)
  and button glyphs with hard corners in `glyphs/`.

## Glyphs of a theme

`radius` does not reach the glyphs: they are pictures. `tools/make-glyphs.py` draws the
set (white on transparent, 64 px high, tinted by Lumen), and can draw the ones with
corners (L1, R1, L2, R2, Select, Start, the d-pad) square and with the theme's font:

```
python3 tools/make-glyphs.py --square --font themes/<id>/fonts/bold.ttf themes/<id>/glyphs
```
