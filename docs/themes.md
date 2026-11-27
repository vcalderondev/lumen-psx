# Themes

A theme changes Lumen's colours and, optionally, any of its runtime art: fonts, button
glyphs, the menu music, the dashboard background. Settings → Display → Theme picks one
(`theme=<id>` in `settings.ini`); "Apply the theme" restarts Lumen (not the console),
because fonts and art are loaded at start.

```
/lumen/themes/<id>/theme.ini     required; <id>: lower case, digits, '-' and '_'
/lumen/themes/<id>/fonts/        regular.ttf, medium.ttf, semibold.ttf, bold.ttf (any of them)
/lumen/themes/<id>/glyphs/       button glyphs, same names as assets/glyphs (cross.png ...)
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
accent     = #FF9F45          ; kickers, checks, highlights
pill       = #E8603C          ; selection pill of the game menu and the pause dialog
card       = #2A1A1E          ; menu / dialog card
card_alpha = 0.94             ; 0.2 .. 1
radius     = 1                ; corner rounding of every shape: 1 = Lumen's, 0 = square corners
text       = #FFF6EE          ; primary text
dim        = #E2C3B4          ; secondary text
backdrop   = #120A0C          ; dialog backdrop and scrims
panel      = #1A0F12          ; settings panel background
danger     = #FF7070          ; destructive rows (Exit the game)
```

Colours are `#RRGGBB` or `r, g, b` (0..255, or 0..1). Missing keys keep Lumen's colours.
`radius` scales the corners of every card, row, tile, pill and shadow at once (avatars and
discs stay round): `0.5` halves them, `0` gives the hard edges of a 1990s console menu.

Two themes ship with Lumen:

- `dist/themes/sunset`: colours only, a complete `theme.ini`.
- `dist/themes/retro`: a 1990s console look. The grey of the original console's shell,
  square corners (`radius = 0`), the four colours of its logo and its own typeface in
  `fonts/` (Barlow, SIL Open Font License: `fonts/OFL.txt`).
