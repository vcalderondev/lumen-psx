# Settings, languages, profiles, enhanced graphics and attract mode — implementation spec

Status: design, to be implemented in five sequential steps (section 10).
Every implementer follows this document exactly; where it says "must", it is a
requirement checked by the acceptance list of the step. Anything not specified is
left to the implementer, within the hard rules of section 1.

Features (user request):

1. a settings panel
2. several UI languages (Spanish first)
3. a screensaver / attract mode that runs short **demo sessions** of some games,
   like an arcade cabinet, without leaving any trace (ephemeral session)
4. enhanced graphics per system (Original / Enhanced)
5. profiles, each with its own memory cards and save states

---

## 1. Hard rules (apply to every step)

- Target: SDL2 **2.0.4**, SDL2_image 2.0.1, SDL2_ttf 2.0.14, GLES 3.0 shaders, glibc 2.24,
  Cortex-A35, 1280x720. No SDL API newer than 2.0.4 (it fails to link against the console's SDL).
- Build only with `./toolchain/psc lumen/build.sh` from the workspace root. Never run two
  builds at once. Zero warnings; `psc-check` must print `[OK]` for every binary.
- New `.c` files are added to the `lumen` link line of `build.sh` (section 11).
- Performance: no allocation per frame, no blocking file I/O on the render thread except
  tiny files (settings, profile.ini, launch request, statvfs). Every animation uses
  `spring_step` (gl.h) or the existing `clampf`/smoothstep helpers.
- Every user-visible string goes through the translation layer (`tr()` / `trf()`).
  Exceptions: the brand label `lumen`, system names from the declarations, game metadata,
  profile names typed by the user, and numbers.
- Nothing in this repository may name or refer to the third-party loader/firmware the
  console boots from. Paths of the RetroArch installation, its config and its existing
  save folders are only known by `lumen.conf` (`psc/lumen.conf`,
  outside the repo). The repo never hard-codes them.
- Commits: plain subject/body, no attribution trailers of any kind. Do not push.
  Commit only the files of your step (other sessions may have unrelated uncommitted
  work in the tree: never `git add -A`).
- Never touch `/Volumes` (the USB stick) and never run an installer.

## 2. Existing code this spec hooks into

All names below exist today in `src/main.c` unless noted.

| Area | Names |
|---|---|
| data model | `game_t`, `platform_t`, `g_games`, `g_ngames`, `g_plat`, `g_nplat`, `g_root`, `g_home`, `g_core_dir`, `g_state` |
| declarations | `decl_get()`, `platform_from_decl()`, `load_systems()` |
| GL / 2D | `g_prog_2d`, `u2_mvp/u2_tex/u2_color/u2_mode`, `FS_2D` (modes 0–4), `draw_2d()`, `text_tex()`, `surface_to_tex()`, `label_t` + `label_set()`, fonts `g_font_title/sub/small/cover` |
| text baked into textures | `card_tex()` (back of the case: "jugador(es)", "Memory card 1"), `spine_tex()` |
| input | `enum { IN_LEFT … IN_COUNT }`, `in_down[]`, `in_pressed[]`, `in_set()`, `in_raw_button()`, `in_event()` |
| help bar | `hint_t`, `g_hints[]`, `hint_gl_t`, `g_hint_gl[]`, `hints_load()`, `hints_draw()` |
| main loop | `main()`: event poll → logical actions (`play`, `move`, `plat_move`, `held_dir`) → background/cover streaming → animation targets → 3D render → "2D overlay" block → `SDL_GL_SwapWindow` |
| labels in `main()` | `l_title`, `l_sub`, `l_count`, `l_brand`, `l_start` ("Iniciando…"), `l_coin1` ("INSERT COIN"), `l_coin2` ("CREDIT 1") |
| exit | after the loop: `state.txt` (`sel_plat sel`), `/tmp/lumen-launch` (v1: two lines core/content) when `exit_code == 10` |
| session | `scripts/run.sh`: loop, `run_retroarch(core, content, logfile)` default + override from `lumen.conf`, exit code 10 = launch |
| buttons | `src/buttons.c`: `open_buttons()` (gpio-keys), `find_proc()`, daemon loop in `main()` |

Note: the game libraries are moving to `<root>/games/<system id>/` (work in progress in
another session; `platform_from_decl()` defaults `paths` to that). Nothing in this spec
depends on the library layout; always use `game_t.content` as the content path.

## 3. New source layout

```
src/lumen.h       shared data model + render services exported by main.c   (step 1)
src/version.h     #define LUMEN_VERSION "0.2.0"                             (step 1)
src/i18n.c/.h     translation layer                                        (step 1)
src/settings.c/.h settings store                                           (step 1)
src/panel.c/.h    settings panel, on-screen keyboard, confirm dialog       (step 2)
src/profiles.c/.h profiles (separate memory cards)                         (step 3)
src/launch.c/.h   launch request v2 writer                                 (step 3)
src/attract.c/.h  screensaver / attract scene + demo picker                (step 5)
dist/lang/{es,en,pt,fr,de,it}.lang                                         (step 1)
tools/check-lang.sh   verifies that every .lang has exactly the keys of es.lang (step 1)
```

`main.c` only receives hooks: calls into the modules, the input additions, and the
removal of `static` from the functions/globals listed in `lumen.h`.

### 3.1 `src/lumen.h` (step 1)

Moves the data model out of `main.c` and exposes the render services the new modules need.
`main.c` keeps the definitions; it drops `static` from exactly these symbols:

```c
#ifndef LUMEN_H
#define LUMEN_H
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include "gl.h"

#define W 1280
#define H 720
#define MAX_GAMES 2048
#define MAX_PLATFORMS 16

enum { KIND_DISC, KIND_CART, KIND_ARCADE };
enum { ANIM_OPEN_CASE_DISC, ANIM_INSERT_CART, ANIM_INSERT_COIN };

typedef struct { ... } game_t;       /* unchanged, moved from main.c */
typedef struct { ... } platform_t;   /* moved; fields added in steps 4 and 5 */

extern game_t     g_games[MAX_GAMES];
extern int        g_ngames;
extern platform_t g_plat[MAX_PLATFORMS];
extern int        g_nplat;
extern char       g_root[256], g_home[256], g_core_dir[256];
extern TTF_Font  *g_font_title, *g_font_sub, *g_font_small, *g_font_cover;

typedef struct { GLuint tex; int w, h; char key[200]; } label_t;

void   logf_(const char *fmt, ...);
double now_s(void);
void   draw_2d(GLuint tex, float x, float y, float w, float h, const float rgba[4], int mode);
void   draw_round_rect(float x, float y, float w, float h, float radius, const float rgba[4], float border_alpha);
GLuint text_tex(TTF_Font *f, const char *s, int *w, int *h);
void   label_set(label_t *l, TTF_Font *f, const char *s);
void   label_free(label_t *l);
void   gl_use_2d(void);        /* glUseProgram(g_prog_2d) + sampler 0 */
#endif
```

`gl.h` is already header-only (`static inline`), so it can be included by every unit.

### 3.2 2D shader addition (step 2)

`FS_2D` gets **mode 5 = glass rounded rectangle**, used by the panel, the keyboard and the
dialogs. A new uniform `uniform vec4 rect;` (`u2_rect`: x = width px, y = height px,
z = corner radius px, w = border alpha) is set by `draw_round_rect()`:

- signed distance to a rounded rectangle in pixels (`v_uv * rect.xy`), 1 px anti-aliased edge;
- fill = `color` with a subtle vertical sheen (`+0.06` at the top, `-0.04` at the bottom);
- inner 1 px border `vec3(1.0)` with alpha `rect.w`.

`draw_2d()` keeps its signature; it sets `u2_rect` to `vec4(w, h, 0, 0)` only when mode 5 is
used through `draw_round_rect()`. No other mode changes.

---

## 4. Settings store — `src/settings.c/.h` (step 1)

File: `<home>/settings.ini` (`g_home`, i.e. `/media/lumen/settings.ini` on the console).
Format: one `key=value` per line, `#` comments, no sections, UTF-8, whitespace around
`=` trimmed. Unknown keys are preserved on save (the store keeps every pair it read).

| key | values | default | used by |
|---|---|---|---|
| `version` | `1` | `1` | future migrations |
| `language` | code of a file in `<home>/lang/` | `es` | i18n |
| `profile` | profile id | `main` | profiles, launch |
| `screensaver_minutes` | `0` (off), 1–60 | `5` | attract |
| `demo_enabled` | `0`/`1` | `1` | attract |
| `demo_seconds` | 20–300 | `60` | attract, launch |
| `attract_demo_delay` | seconds of attract before a demo, 5–120 | `20` | attract |
| `show_stock_games` | `0` (hidden) / `1` (visible): list the console's built-in games (systems with `layout = stock`, docs/systems.md) | `0` | library |
| `graphics.<system id>` | `original` / `enhanced` | `original` | launch |

Invalid values are clamped or replaced by the default when read (never written back
until the next save).

```c
/* settings.h */
#define SETTINGS_MAX 64
int         settings_load(const char *home);             /* 0 ok, -1 missing (defaults) */
const char *settings_get(const char *key, const char *def);
int         settings_get_int(const char *key, int def, int lo, int hi);
void        settings_set(const char *key, const char *value);   /* marks dirty if changed */
void        settings_set_int(const char *key, int value);
int         settings_dirty(void);
int         settings_save(void);                          /* 0 ok, -1 error; clears dirty */
```

Storage: static array of `SETTINGS_MAX` pairs (`char key[48]; char val[96];`), no heap.

Atomic save (`settings_save`): write `<home>/settings.ini.tmp` → `fflush` → `fsync(fileno)` →
`fclose` → `rename()` over `settings.ini` → `fsync()` of the folder (never a global
`sync()`, which blocks the render thread on every dirty page of the stick). On failure, log it, keep the old file,
keep `dirty` set. Saving happens:

- when the panel closes and `settings_dirty()`;
- a profile switch (or the fallback to `main` after a delete) only marks settings dirty:
  it is saved with the panel close or on exit (section 7);
- at program exit if dirty (before writing `state.txt`).

Never on every left/right press.

---

## 5. Translation layer — `src/i18n.c/.h` (step 1)

### 5.1 Files

`<home>/lang/<code>.lang`, UTF-8 (a BOM is skipped), one `key = value` per line,
`#` comment lines, blank lines ignored, `\n` in a value is a line break.
Placeholders are named, in braces: `{n}`, `{name}`, `{system}`, `{title}`. A translation may
reorder or omit placeholders; unknown placeholders are left as-is. **Never use printf
format strings from a .lang file.**

The language list shown in the panel is every `*.lang` in `<home>/lang/`, sorted by code,
with its display name from its own `lang.name` key. Shipped: `es` (default), `en`, `pt`,
`fr`, `de`, `it`. Max 16 languages.

### 5.2 API

```c
/* i18n.h */
int         i18n_init(const char *home);          /* scans <home>/lang; loads "en" as fallback */
int         i18n_set_language(const char *code);  /* 0 ok; -1 unknown code (stays on previous) */
const char *i18n_language(void);                  /* active code */
int         i18n_count(void);
const char *i18n_code(int i);
const char *i18n_name(int i);                     /* that language's "lang.name" */
unsigned    i18n_generation(void);                /* ++ on every successful language change */
const char *tr(const char *key);                  /* active -> en -> key itself */
const char *trf(char *buf, size_t n, const char *key, ...);
            /* name/value pairs, NULL terminated:
               trf(s, sizeof s, "top.games", "n", "12", "system", pl->name, NULL) */
```

Implementation: each loaded language is one heap block (file read once) with an index of
`{key, value}` pointers sorted with `qsort` and looked up with `bsearch`. Loading happens
only at start and on language change (never per frame). `tr()` returns pointers valid
until the next `i18n_set_language()`; callers that keep text across frames must keep it in
a `label_t` (which copies) — `label_set()` re-renders only when the string changes, so
calling `label_set(&l, font, tr("key"))` every frame is the intended pattern.

If `<home>/lang` is missing, `tr()` returns keys (logged once as a warning).

### 5.3 Live language change

`i18n_set_language()` is called by the panel; then the panel calls
`ui_language_changed()` (defined in `main.c`), which:

1. calls `hints_reload()` (deletes and rebuilds every hint text texture);
2. deletes `back_tex` of every game (`glDeleteTextures`, set to 0) so `card_tex(g, 1)`
   regenerates lazily for the selection;
3. nothing else: every other label is set each frame through `label_set(…, tr(…))`.

### 5.4 Strings that move to `tr()` in step 1

| current literal in main.c | key |
|---|---|
| `"Iniciando…"` (`l_start`) | `launch.starting` |
| `"INSERT COIN"`, `"CREDIT 1"` | `coin.insert`, `coin.credit` |
| `"%d juegos  ·  %s"` (`l_count`) | `top.games_one` / `top.games` |
| `"Agrega juegos a /games"` | `empty.library` |
| `"%s jugador%s"` in `card_tex()` | `card.players_one` / `card.players` |
| `"Memory card 1"` in `card_tex()` | `card.memory_card` (shows the active profile name from step 3; until then `{name}` = `tr("profile.main")`) |
| `g_hints[]` texts "Jugar", "Plataforma", "Salir" | `hint.play`, `hint.platform`, `hint.exit` |
| `g_hints[]` fallbacks `"L1 / R1"`, `"Select + Start"`, `"✕ / ○"` | `btn.l1_r1`, `btn.select_start`, `btn.cross_circle` |

`hint_t` changes to `{ const char *icons[3]; const char *fallback_key; const char *text_key; }`.

### 5.5 Full key list (es / en)

`pt`, `fr`, `de`, `it` must contain the same keys (checked by `tools/check-lang.sh`),
translated by the implementer of step 1. Arcade terms (`coin.*`, `attract.press_start`
in en) intentionally stay English-style in es too where shown.

| key | es | en |
|---|---|---|
| `lang.name` | Español | English |
| **carousel** | | |
| `top.games_one` | 1 juego · {system} | 1 game · {system} |
| `top.games` | {n} juegos · {system} | {n} games · {system} |
| `empty.library` | Agrega juegos a /games | Add games to /games |
| `launch.starting` | Iniciando… | Starting… |
| `coin.insert` | INSERT COIN | INSERT COIN |
| `coin.credit` | CREDIT 1 | CREDIT 1 |
| `card.players_one` | {n} jugador | {n} player |
| `card.players` | {n} jugadores | {n} players |
| `card.memory_card` | Memory card · {name} | Memory card · {name} |
| **help bar** | | |
| `hint.play` | Jugar | Play |
| `hint.platform` | Plataforma | Platform |
| `hint.exit` | Salir | Exit |
| `hint.settings` | Ajustes | Settings |
| `hint.move` | Mover | Move |
| `hint.change` | Cambiar | Change |
| `hint.select` | Aceptar | Select |
| `hint.back` | Volver | Back |
| `hint.close` | Cerrar | Close |
| `hint.type` | Escribir | Type |
| `hint.erase` | Borrar | Erase |
| `hint.done` | Listo | Done |
| `hint.cancel` | Cancelar | Cancel |
| `btn.l1_r1` | L1 / R1 | L1 / R1 |
| `btn.select_start` | Select + Start | Select + Start |
| `btn.cross_circle` | ✕ / ○ | ✕ / ○ |
| `btn.triangle` | △ | △ |
| `btn.square` | □ | □ |
| `btn.dpad` | Cruceta | D-pad |
| `btn.start` | Start | Start |
| **panel** | | |
| `panel.title` | Ajustes | Settings |
| `section.language` | Idioma | Language |
| `section.profile` | Perfil | Profile |
| `section.screensaver` | Protector de pantalla | Screensaver |
| `section.graphics` | Gráficos | Graphics |
| `section.library` | Biblioteca | Library |
| `section.about` | Acerca de | About |
| `value.on` | Sí | On |
| `value.off` | No | Off |
| **profiles** | | |
| `profile.main` | Principal | Main |
| `profile.active` | Perfil activo | Active profile |
| `profile.create` | Crear perfil | Create profile |
| `profile.rename` | Cambiar nombre | Rename |
| `profile.color` | Color | Colour |
| `profile.delete` | Eliminar perfil | Delete profile |
| `profile.count` | {n} de 8 perfiles | {n} of 8 profiles |
| `profile.full` | Ya hay 8 perfiles | There are already 8 profiles |
| `profile.default_name` | Jugador {n} | Player {n} |
| `profile.name_title` | Nombre del perfil | Profile name |
| `profile.name_taken` | Ese nombre ya existe | That name already exists |
| `profile.name_empty` | Escribe un nombre | Type a name |
| `profile.main_locked` | El perfil principal no se puede eliminar | The main profile can't be deleted |
| `profile.delete_confirm` | ¿Eliminar «{name}» y todas sus partidas guardadas? | Delete "{name}" and all its saved games? |
| `profile.delete_note` | Esta acción no se puede deshacer. | This can't be undone. |
| `profile.cards_note` | Cada perfil tiene sus propias memory cards y estados guardados. | Each profile has its own memory cards and save states. |
| `confirm.delete` | Eliminar | Delete |
| `confirm.cancel` | Cancelar | Cancel |
| **screensaver** | | |
| `ss.after` | Activar tras | Start after |
| `ss.minutes` | {n} min | {n} min |
| `ss.never` | Nunca | Never |
| `ss.demos` | Demos de juegos | Game demos |
| `ss.demo_length` | Duración de cada demo | Length of each demo |
| `ss.seconds` | {n} s | {n} s |
| `ss.preview` | Probar ahora | Preview now |
| `ss.note` | Las demos no guardan partidas. Pulsa cualquier botón para volver. | Demos never save. Press any button to come back. |
| **graphics** | | |
| `gfx.original` | Original | Original |
| `gfx.enhanced` | Mejorado | Enhanced |
| `gfx.note` | «Mejorado» aumenta la resolución o suaviza la imagen; algunos juegos pueden ir más lentos. | "Enhanced" raises the resolution or smooths the picture; some games may run slower. |
| `gfx.none` | Ningún sistema ofrece gráficos mejorados | No system offers enhanced graphics |
| `lib.stock` | Juegos de fábrica | Built-in games |
| `lib.hidden` | Ocultos | Hidden |
| `lib.visible` | Visibles | Visible |
| `lib.stock_note` | Los 20 juegos que trae la consola. Solo lectura: sus partidas se guardan en el perfil activo. | The 20 games that come with the console. Read-only: their saves go to the active profile. |
| **about** | | |
| `about.version` | Versión | Version |
| `about.games` | Juegos | Games |
| `about.systems` | Sistemas | Systems |
| `about.profiles` | Perfiles | Profiles |
| `about.free` | Espacio libre | Free space |
| `about.gb` | {n} GB | {n} GB |
| `about.mb` | {n} MB | {n} MB |
| **keyboard** | | |
| `osk.space` | Espacio | Space |
| `osk.erase` | Borrar | Erase |
| `osk.shift` | Mayús | Shift |
| `osk.ok` | OK | OK |
| **attract** | | |
| `attract.press_start` | PULSA START | PRESS START |
| `attract.demo` | DEMO | DEMO |
| `attract.demo_of` | Demo · {title} | Demo · {title} |
| **errors (toast)** | | |
| `error.save` | No se pudieron guardar los ajustes | Couldn't save the settings |
| `error.profile` | No se pudo modificar el perfil | Couldn't change the profile |

Toast: a small glass pill at the bottom centre above the help bar, 2.5 s, drawn by the panel
module (`panel_toast(key)`), usable outside the panel too.

---

## 6. Settings panel — `src/panel.c/.h` (step 2)

### 6.1 Input additions (main.c)

- New logical actions appended before `IN_COUNT`: `IN_OPTIONS` (Triangle) and `IN_BACK` (Square).
- `in_raw_button()`: raw 0 → `IN_OPTIONS`, raw 3 → `IN_BACK` (PSC pad layout documented there).
- `in_event()` game controllers: `SDL_CONTROLLER_BUTTON_Y` → `IN_OPTIONS`, `SDL_CONTROLLER_BUTTON_X` → `IN_BACK`.
- Keyboard: `Tab`, `F1` → `IN_OPTIONS`; `Backspace` → `IN_BACK`. **Change** `SDLK_UP/DOWN` to
  `IN_UP/IN_DOWN` (they are `IN_L1/IN_R1` today; the carousel already treats `IN_UP/IN_DOWN`
  like L1/R1, so carousel behaviour is unchanged) and keep `PageUp/PageDown` → `IN_L1/IN_R1`.
- `in_set()` records `in_last_activity = now_s()` on every press edge, and `in_event()` also
  on any key/button/hat/axis event beyond the dead zone (used by attract, step 5).

Navigation struct passed to UI modules (in `lumen.h`):

```c
typedef struct { int up, down, left, right, confirm, back, options, start, l1, r1; } ui_nav_t;
```

`main()` fills it from `in_pressed[]`, with auto-repeat for up/down/left/right using the
same delays as the carousel (`0.32 s` first, then `0.085 s`). `confirm` = `IN_PLAY`
(Cross or Circle), `back` = `IN_BACK`, `options` = `IN_OPTIONS`.

### 6.2 API

```c
/* panel.h */
enum { HINTS_CAROUSEL, HINTS_PANEL_ROOT, HINTS_PANEL_PAGE, HINTS_OSK, HINTS_CONFIRM, HINTS_ATTRACT, HINTS_COUNT };
void  panel_init(void);                 /* after fonts + i18n + settings */
void  panel_open(void);
int   panel_active(void);               /* 1 while open or closing (input owned by the panel) */
float panel_amount(void);               /* 0..1 open spring, for the backdrop and hint fade */
void  panel_update(const ui_nav_t *nav, double t, float dt);
void  panel_draw(double t);             /* call inside the 2D overlay block, after hints, before flash/fade */
int   panel_hint_set(void);             /* which HINTS_* to show */
void  panel_toast(const char *key);
void  panel_toast_draw(double t);
```

Callbacks implemented in `main.c` (declared in `panel.h`):

```c
void ui_language_changed(void);      /* section 5.3 */
void ui_start_attract(void);         /* "Preview now", step 5; before step 5 a no-op */
```

### 6.3 Behaviour in `main()`

- Carousel, not launching, panel closed: `in_pressed[IN_OPTIONS]` → `panel_open()`.
- While `panel_active()`: the carousel's `move`, `plat_move`, `play` and the Select+Start exit
  are forced to 0 (same place where `play_t >= 0` zeroes them). The carousel keeps
  animating (springs, idle float, background crossfade, cover streaming) behind the panel.
- Backdrop: full-screen black quad, alpha `0.45 * panel_amount()`, drawn before the panel.
- The top bar, pills and game title fade to `1 - 0.6 * panel_amount()`.
- Help bar: `hints_draw(set, y, alpha)` with `set = panel_active() ? panel_hint_set() : HINTS_CAROUSEL`;
  when the set changes, the old set fades out and the new one in over 0.15 s.

### 6.4 Look

- Right side panel: x = `W - 24 - 520`, y = 24, 520 x 672, radius 22, fill
  `(0.10, 0.10, 0.14, 0.78)`, border alpha 0.12, drawn with `draw_round_rect()`.
- Open/close: spring `slide` (k 170, c 18) from +560 px to 0; `fade` spring (k 90, c 14).
  Closing keeps `panel_active()` true until `slide > 540`.
- Title `panel.title` in `g_font_title` at the top; the page title below it in `g_font_sub`.
- Rows: height 52, label left (`g_font_sub`), value right (`< value >` arrows dimmed when at
  an end), selected row = highlight `draw_round_rect` (fill `(1,1,1,0.10)`), which moves with
  a spring (k 220, c 22) to the selected row's y.
- Page change: horizontal slide of the content (spring, ±80 px + fade).
- Profile colours: 8 swatches (section 7.3) drawn as 18 px circles (mode 3 is soft; use
  `draw_round_rect` with radius = size / 2).

### 6.5 Pages

Root page: six entries — Language, Profile, Screensaver, Graphics, Library, About (each
with its current value on the right: the language name, the profile name, "5 min" / "Nunca";
nothing for Graphics, Library and About). The root rows are the pages in enum order. Cross enters the page; Triangle or Square closes the panel.

Inside a page: Up/Down move, Left/Right change `CHOICE` rows (applied immediately to the
setting, saved on close), Cross triggers `ACTION` rows, Square or Triangle goes back to root.

| page | rows |
|---|---|
| Language | one `ACTION` row per language (`i18n_name(i)`), a check mark on the active one; Cross applies live (`i18n_set_language`, `settings_set("language")`, `ui_language_changed()`) |
| Profile | `profile.active` CHOICE over existing profiles (swatch + name); `profile.create` ACTION (disabled + `profile.full` when 8); `profile.rename` ACTION → keyboard prefilled; `profile.color` CHOICE over 8 swatches; `profile.delete` ACTION (disabled on `main`, shows `profile.main_locked`) → confirm dialog; footer `profile.count` and `profile.cards_note` |
| Screensaver | `ss.after` CHOICE: Never, 1, 2, 3, 5, 10, 15, 30 min; `ss.demos` CHOICE On/Off; `ss.demo_length` CHOICE: 30, 45, 60, 90, 120, 180 s (dimmed when demos are off); `ss.preview` ACTION → closes the panel and calls `ui_start_attract()`; footer `ss.note` |
| Graphics | one CHOICE row per platform with a `[graphics]` section (step 4): label = system name, values Original / Enhanced, key `graphics.<id>`; footer `gfx.note`; if none: `gfx.none` |
| Library | `lib.stock` CHOICE Hidden / Visible (`lib.hidden` / `lib.visible`), key `show_stock_games`; every change calls `ui_library_changed()` and `main.c` rescans the declarations and libraries at the top of the next frame (selection kept by system id + content path, else the same system, else the first); footer `lib.stock_note` |
| About | INFO rows: version (`LUMEN_VERSION`), games (`g_ngames`), systems (`g_nplat`), profiles (`profiles_count()`, from step 3), free space of `g_root` (`statvfs` once when the page opens; GB with one decimal ≥ 1 GB, else MB) |

Before step 3 exists, the Profile page shows only the active profile row with "Principal"
and no actions; before step 4, Graphics shows `gfx.none`. Each later step fills its page.

### 6.6 On-screen keyboard (part of panel.c)

Modal over the panel (its own glass rect, 760 x 330, centred on screen, spring scale-in
0.92 → 1 + fade). Header: `profile.name_title`, the text field with a blinking caret.

Grid (10 columns, 4 letter rows + 1 action row):

```
1 2 3 4 5 6 7 8 9 0
A B C D E F G H I J
K L M N O P Q R S T
U V W X Y Z Ñ - . _
[ Mayús ][    Espacio    ][ Borrar ][ OK ]
```

- Shift toggles letters to lower case (Ñ → ñ). The first letter starts upper case.
- D-pad moves (wraps), Cross types / presses, Square = erase last character (UTF-8 aware:
  remove continuation bytes), Start = OK, Triangle = cancel.
- Max 16 characters (UTF-8, ≤ 48 bytes). OK with an empty/blank name shows
  `profile.name_empty`; a name equal (case-insensitive) to another profile shows
  `profile.name_taken`.
- Glyph textures for the 40 keys + 4 action labels are rendered once when the keyboard
  opens (`label_t` array) and freed when it closes; the text field is one `label_t`.

### 6.7 Confirm dialog

Small glass rect (560 x 220) centred: message (`trf("profile.delete_confirm", "name", …)`,
wrapped with `TTF_RenderUTF8_Blended_Wrapped`, which exists in SDL_ttf 2.0.14),
`profile.delete_note`, two buttons `confirm.cancel` (selected by default) and
`confirm.delete` (red tint). Left/Right choose, Cross confirms, Square/Triangle cancel.

### 6.8 Help bar sets

`hint_t` entries become per-set tables; icons as today (`<home>/assets/icons/<name>.png`, old installs `<home>/icons/`,
`triangle.png` and `square.png` are already installed), fallback text keys from 5.5.

| set | hints |
|---|---|
| `HINTS_CAROUSEL` | cross+circle `hint.play` · l1+r1 `hint.platform` · triangle `hint.settings` · select+start `hint.exit` |
| `HINTS_PANEL_ROOT` | dpad `hint.move` · cross `hint.select` · triangle `hint.close` |
| `HINTS_PANEL_PAGE` | dpad `hint.change` · cross `hint.select` · square `hint.back` |
| `HINTS_OSK` | cross `hint.type` · square `hint.erase` · start `hint.done` · triangle `hint.cancel` |
| `HINTS_CONFIRM` | dpad `hint.move` · cross `hint.select` · square `hint.cancel` |
| `HINTS_ATTRACT` | (none; the attract scene draws its own "PRESS START") |

**Controls revision (implemented in step 2).** The controls commit that landed before
step 2 made Cross/Start play, Circle `IN_BACK` (no longer play) and Select tapped alone
open the settings. Step 2 keeps that and adds the spec's mapping on top: Triangle (and
Tab/F1) = `IN_OPTIONS`, Square also = `IN_BACK`, `confirm` = Cross only. A Select tap while
the panel is open acts as `options`. The help bar sets therefore use Circle for back:
`HINTS_CAROUSEL` = cross `hint.play` · l1+r1 `hint.platform` · select `hint.settings`;
`HINTS_PANEL_ROOT` closes with circle; `HINTS_PANEL_PAGE` and `HINTS_CONFIRM` go back /
cancel with circle; `HINTS_OSK` is unchanged. New fallback keys: `btn.cross`, `btn.circle`,
`btn.select`. While the panel is open the help bar is centred in the free space left of it.

`hints_load(home, font)` becomes `hints_load(home)` + `hints_reload()` (rebuilds texts only;
icons are loaded once). `hints_draw(int set, float y, float alpha)`.

---

## 7. Profiles — `src/profiles.c/.h` (step 3)

### 7.1 On disk

```
<home>/profiles/<id>/profile.ini     name=..., color=0..7, created=<unix time>
<home>/profiles/<id>/saves/          memory cards (.srm) — not used for id "main"
<home>/profiles/<id>/states/         save states        — not used for id "main"
```

- `main` is built in and always exists, even without a folder. Its `profile.ini` is created on
  first rename/recolour and only stores `name`/`color`; an empty name displays `tr("profile.main")`.
  **`main` keeps the save folders RetroArch already uses** (mapped by run.sh from `lumen.conf`,
  section 8.4), so no existing save is lost.
- Other ids are `p1` … `p9` (lowest free number). Ids never change; rename only edits `name`.
- At most 8 profiles including `main`.
- Delete: refuse `main`; `rename()` the folder to `<home>/profiles/.trash-<id>-<time>` (instant),
  switch to `main` if it was active, save settings; a detached `SDL_CreateThread` removes the
  trash folder recursively (`nftw`, `FTW_DEPTH | FTW_PHYS`). `run.sh` also removes leftover
  `.trash-*` folders at session start.
- `profile.ini` writes are atomic (tmp + rename + sync) like settings.

### 7.2 API

```c
/* profiles.h */
#define MAX_PROFILES 8
typedef struct { char id[16]; char name[52]; int color; long created; } profile_t;

int              profiles_load(const char *home);        /* scans <home>/profiles, adds "main" first */
int              profiles_count(void);
const profile_t *profiles_get(int i);                    /* 0 = main, others sorted by created */
int              profiles_find(const char *id);          /* index or -1 */
const profile_t *profiles_active(void);                  /* from settings "profile", falls back to main */
const char      *profiles_display_name(const profile_t *p);  /* name or tr("profile.main") */
int              profiles_set_active(const char *id);    /* settings_set + settings_save */
int              profiles_create(const char *name, int color, char *id_out, size_t n);
                 /* 0 ok, -1 full, -2 name taken, -3 I/O; creates saves/ and states/ */
int              profiles_rename(const char *id, const char *name);   /* 0 / -2 / -3 */
int              profiles_set_color(const char *id, int color);
int              profiles_delete(const char *id);        /* 0 / -1 main / -3 I/O */
void             profiles_color(int idx, float rgb[3]);
```

### 7.3 Colours (index → sRGB)

0 `#5B8CFF` blue · 1 `#FF5B7A` pink · 2 `#3DDC97` green · 3 `#FFB547` amber ·
4 `#B07CFF` violet · 5 `#2EC5D3` cyan · 6 `#FF7F3F` orange · 7 `#C9CBD6` silver.
New profiles get the first colour not used by another profile.

### 7.4 UI hooks (main.c)

- Top-right: a profile chip (swatch circle 14 px + `profiles_display_name()` in
  `g_font_small`) right-aligned at x = `W - 36`, and `top.games` text to its left, separated by
  16 px. Replaces the current `l_count` placement; `l_count` becomes `trf("top.games"...)`.
- `card_tex()` back text: `trf("card.memory_card", "name", profiles_display_name(active))`.
  Switching profile calls `ui_language_changed()`'s step 2 (invalidate `back_tex`), exposed as
  `ui_cards_changed()`.
- The panel's Profile page (6.5) calls the API; any non-zero result shows `panel_toast("error.profile")`
  or the specific message.

### 7.5 Launch request v2 — `src/launch.c/.h` (step 3)

`/tmp/lumen-launch` becomes `key=value` lines (value = rest of the line, no quoting).
Written atomically (`/tmp/lumen-launch.tmp` + `rename`). Lumen rejects a request whose
values contain `\n`/`\r` (logs and does not exit 10/11).

```
version=2
core=/media/lumen/cores/pcsx_rearmed_libretro.so
content=/media/games/psx/12/Crash Bandicoot.cue
system=psx
title=Crash Bandicoot
profile=main
mode=normal                    # normal | demo
demo_seconds=60                # only meaningful with mode=demo
graphics=original              # original | enhanced                (step 4)
opt_core_name=PCSX-ReARMed     # folder/file name of RetroArch's per-core .opt (step 4)
opt.pcsx_rearmed_neon_enhancement_enable=disabled                     (step 4, 0..n lines)
ra.video_smooth=false                                                 (step 4, 0..n lines)
```

```c
/* launch.h */
typedef struct {
   const char *core, *content, *system, *title, *profile;
   int demo, demo_seconds;
   const char *graphics, *opt_core_name;   /* NULL until step 4 */
   const char *opt_list, *ra_list;         /* "k=v, k=v" from the declaration, or NULL */
} launch_req_t;
int launch_write(const char *path, const launch_req_t *r);   /* 0 ok, -1 invalid, -2 I/O */
```

Validation in `launch_write`: keys of `opt_list`/`ra_list` match `[A-Za-z0-9_]+`; values
contain none of `" , = \n \r`; invalid pairs are skipped and logged. `profile` matches
`[a-z0-9_-]+`.

The exit block of `main()` replaces the v1 `fprintf` with `launch_write()`.

Exit codes of `lumen`: `0` leave to the stock menu, `10` launch (mode=normal), `11` demo
launch (mode=demo, step 5). 

---

## 8. Session script and RetroArch glue (steps 3–5)

### 8.1 run.sh structure (POSIX sh, busybox-compatible: no arrays, no `[[ ]]`, no `local` reliance, no `awk` requirement)

New helpers defined in `run.sh` **before** `lumen.conf` is sourced, so `lumen.conf` only
sets paths and its `run_retroarch()`:

```sh
lumen_parse_launch FILE      # sets L_CORE L_CONTENT L_SYSTEM L_PROFILE L_MODE L_DEMO_SECONDS
                             # L_GRAPHICS L_OPT_CORE_NAME; writes opt.* pairs to /tmp/lumen-opt.kv
                             # and ra.* pairs to /tmp/lumen-ra.kv ("key=value" lines).
                             # v1 fallback: if line 1 has no "=", L_CORE=line1, L_CONTENT=line2.
lumen_profile_dirs           # sets L_SAVES L_STATES (section 8.4)
lumen_merge_opt OPTFILE KV   # section 8.5
lumen_build_ra_extra         # writes /tmp/lumen-ra-extra.cfg, exports LUMEN_RA_EXTRA
lumen_run_demo               # section 8.6
```

Parsing: `while IFS= read -r line; do key=${line%%=*}; val=${line#*=}; case "$key" in … esac; done < FILE`.
`profile` is accepted only if it matches `case "$v" in *[!a-z0-9_-]*|"") → main`.

Loop:

```
rm -rf "$LUMEN_HOME"/profiles/.trash-*  (once, at start)
ARGS=""
while true; do
  rm -f /tmp/lumen-launch
  lumen --home … --log … $ARGS ; rc=$?
  ARGS=""
  case rc: 10 → normal session; 11 → demo session; anything else → break
  lumen_parse_launch /tmp/lumen-launch || break
  [ rc = 11 ] && L_MODE=demo   (the exit code wins over the file)
  if graphics data present: lumen_merge_opt "$LUMEN_CORE_OPT_DIR/$L_OPT_CORE_NAME/$L_OPT_CORE_NAME.opt" /tmp/lumen-opt.kv
  normal: lumen_profile_dirs; lumen_build_ra_extra; run_retroarch "$L_CORE" "$L_CONTENT" "$LOG/retroarch.log"
  demo:   lumen_run_demo; ARGS according to its result (8.6)
done
```

Every step is logged to `session.log` as today.

### 8.2 Extra RetroArch config

`lumen_build_ra_extra` writes `/tmp/lumen-ra-extra.cfg` (RetroArch cfg syntax `key = "value"`)
and exports `LUMEN_RA_EXTRA=/tmp/lumen-ra-extra.cfg`:

- `savefile_directory`, `savestate_directory` when `L_SAVES`/`L_STATES` are non-empty;
- every `ra.*` pair from the request;
- in demo mode, the ephemeral block of 8.6.

The default `run_retroarch()` in `run.sh` becomes
`"$RETROARCH" --appendconfig "$LUMEN_RA_EXTRA" -L "$1" "$2" > "$3" 2>&1`.

### 8.3 lumen.conf (workspace, outside the repo)

Keeps its own paths; changes:

```sh
RA_DIR=...                                   # unchanged
LUMEN_CORE_OPT_DIR="${RA_DIR}/config"        # where <Core Name>/<Core Name>.opt live
LUMEN_MAIN_SAVES=...                         # RetroArch's current saves folder
LUMEN_MAIN_STATES=...                        # RetroArch's current savestates folder
run_retroarch() {  # core content logfile
  cp "${RA_DIR}/config/retroarch/retroarch.cfg" /tmp/lumen-ra.cfg
  echo 'config_save_on_exit = "false"' >> /tmp/lumen-ra.cfg
  [ -f "${LUMEN_RA_EXTRA}" ] && cat "${LUMEN_RA_EXTRA}" >> /tmp/lumen-ra.cfg
  XDG_CONFIG_HOME="${RA_DIR}/config" \
    "${RA_DIR}/retroarch" --config /tmp/lumen-ra.cfg -L "$1" "$2" > "$3" 2>&1
}
```

(Later keys in the cfg win in RetroArch, so appended overrides take effect.)

### 8.4 Profile → save folders (run.sh)

```
profile=main → L_SAVES=$LUMEN_MAIN_SAVES  L_STATES=$LUMEN_MAIN_STATES
               (empty when lumen.conf doesn't set them → RetroArch's own config decides)
other id     → L_SAVES=$LUMEN_HOME/profiles/<id>/saves  L_STATES=$LUMEN_HOME/profiles/<id>/states
               (mkdir -p both; if the profile folder doesn't exist, fall back to main and log it)
```

### 8.5 Per-core options merge (`lumen_merge_opt`, step 4)

- Target: `$LUMEN_CORE_OPT_DIR/<opt_core_name>/<opt_core_name>.opt` (RetroArch 1.8.8's
  per-core options file, lines `key = "value"`). Skip entirely when `LUMEN_CORE_OPT_DIR` or
  `opt_core_name` is empty or there are no `opt.*` pairs.
- First time only: copy an existing file to `<file>.lumen-bak`.
- Build `<file>.tmp`: for each existing line, if its key (text before ` =`) is in the KV
  file, write `key = "newvalue"`; otherwise copy the line. Then append every KV key not
  already present. `mv` the tmp over the file, `sync`.
- Pure `sh` + `grep`/`sed` (`while IFS= read -r`), no awk dependency.
- No `core_options_path` line: the merged file is RetroArch's own per-core file (its
  `rgui_config_directory`), which it loads by itself; see `docs/systems.md`.

### 8.6 Demo session (`lumen_run_demo`, step 5)

```sh
rm -rf /tmp/lumen-demo; mkdir -p /tmp/lumen-demo/saves /tmp/lumen-demo/states
L_SAVES=/tmp/lumen-demo/saves; L_STATES=/tmp/lumen-demo/states
lumen_build_ra_extra        # + ephemeral block:
  savestate_auto_save = "false"      savestate_auto_load = "false"
  history_list_enable = "false"      content_runtime_log = "false"
  content_runtime_log_aggregate = "false"
  sort_savefiles_enable = "false"    sort_savestates_enable = "false"
  savefiles_in_content_dir = "false" savestates_in_content_dir = "false"
  (keys unknown to RetroArch 1.8.8 are ignored by it)
run_retroarch "$L_CORE" "$L_CONTENT" "$LOG/retroarch-demo.log" &
RA_JOB=$!
if [ -x "$LUMEN_HOME/lumen-buttons" ]; then
  "$LUMEN_HOME/lumen-buttons" --demo-watch "$L_DEMO_SECONDS" --pid "$RA_JOB" --log "$LOG/buttons.log"
  w=$?
else   # fallback: timeout only
  ( sleep "$L_DEMO_SECONDS"; killall -TERM retroarch ) & T=$!; w=2
fi
wait "$RA_JOB"; [ -n "$T" ] && kill "$T" 2>/dev/null
rm -rf /tmp/lumen-demo
case $w in 1) ARGS="--focus-demo" ;; *) ARGS="--attract" ;; esac
```

Watcher result → next Lumen start:
- `1` (a button was pressed): `--focus-demo` — Lumen starts in the normal carousel with the
  demoed game selected (the player probably wants it).
- `0` (RetroArch ended by itself), `2` (timeout), other: `--attract` — Lumen resumes the attract scene.

Demo sessions never use the profile folders, never write history, and the per-core options
merge still applies (the demo shows the user's graphics choice).

### 8.7 `lumen-buttons --demo-watch` (step 5, `src/buttons.c`)

```
lumen-buttons --demo-watch <seconds> --pid <pid> [--log file]
exit 0: <pid> exited by itself   1: input   2: timeout   3: error
```

`main()` dispatches to a new `static int demo_watch(int seconds, pid_t pid)` when
`--demo-watch` is present; the daemon path is unchanged and prints `ready` only in daemon mode.

- Devices: every `/dev/input/event*`, opened `O_RDONLY | O_NONBLOCK` (never grabbed —
  RetroArch reads the same devices). Keep a device if its name is `gpio-keys`, or
  `EVIOCGBIT(EV_KEY)` has any bit in `BTN_JOYSTICK (0x120) … BTN_THUMBR (0x13e)` or `BTN_SOUTH`,
  or it has `KEY_ENTER` (keyboards, for development). Rescan every 2 s for hot-plug. Max 16 fds.
- Triggers (after a 1.0 s grace period from start): `EV_KEY` with value 1 (press) for any code
  except `KEY_SLEEP` (POWER is the suspend handshake, handled by the daemon); `EV_ABS` on
  `ABS_X/ABS_Y/ABS_HAT0X/ABS_HAT0Y` that moves more than 50 % of the range away from the value
  read with `EVIOCGABS` at open (the PSC pad's d-pad is axes 0/1).
- Loop: `poll()` 100 ms; each tick `kill(pid, 0)` — on `ESRCH` exit 0; on timeout exit 2.
- Stop RetroArch: find processes with comm `retroarch` that descend from `pid` (walk
  `/proc/<n>/stat` ppid chains; reuse/extend `find_proc()`), else `pid` itself; send `SIGTERM`,
  wait up to 3 s (poll `kill(p,0)`), then `SIGKILL`. Log the reason.
- The daemon instance started by boot.sh keeps running: RESET also kills RetroArch there
  (harmless double kill).

---

## 9. Enhanced graphics (step 4)

### 9.1 Declaration format (`docs/systems.md` gets this section)

```ini
[graphics]
core_options_name  = PCSX-ReARMed          ; folder/file name RetroArch uses for the per-core .opt
enhanced           = pcsx_rearmed_neon_enhancement_enable=enabled, pcsx_rearmed_neon_enhancement_no_main=enabled
original           = pcsx_rearmed_neon_enhancement_enable=disabled, pcsx_rearmed_neon_enhancement_no_main=disabled
retroarch_enhanced = video_smooth=false     ; frontend overrides; alias: retroarch
retroarch_original = video_smooth=false
```

- **Lists are comma-separated** (`k=v, k=v`). The spec sketch used `;`, but `decl_get()` treats
  `;` as the start of a comment, so `;` cannot separate pairs. Values may not contain `,` `=` `"`.
- A platform "has graphics" when any of `enhanced` / `retroarch_enhanced` is non-empty; only
  those appear on the Graphics page.
- `platform_t` gains: `char gfx_core[48], gfx_enh[320], gfx_orig[320], gfx_ra_enh[160], gfx_ra_orig[160]; int has_gfx;`
  filled in `platform_from_decl()`.
- At launch: `graphics = settings_get("graphics.<id>", "original")`; `opt_list`/`ra_list` =
  the matching pair of fields; `opt_core_name = gfx_core`.

### 9.2 Shipped values (`dist/systems/*.ini`)

| system | core_options_name | enhanced / original | retroarch_enhanced / retroarch_original |
|---|---|---|---|
| psx | PCSX-ReARMed | `pcsx_rearmed_neon_enhancement_enable=enabled, pcsx_rearmed_neon_enhancement_no_main=enabled` / both `disabled` | — |
| snes | — | — | `video_smooth=true` / `video_smooth=false` |
| megadrive | — | — | `video_smooth=true` / `video_smooth=false` |
| gba | — | — | `video_smooth=true` / `video_smooth=false` |
| arcade | — | — | `video_smooth=true` / `video_smooth=false` |
| n64 (if the file exists when step 4 runs) | — | — | `video_smooth=true` / `video_smooth=false` |

Default for every system: `original`.

---

## 10. Screensaver / attract mode — `src/attract.c/.h` (step 5)

### 10.1 Declaration

New optional section in system declarations (documented in `docs/systems.md`):

```ini
[demo]
allowed = 1        ; games of this system may run as attract-mode demos (default 0)
seconds = 0        ; per-system demo length override; 0 = settings "demo_seconds"
```

Shipped: `psx` and `arcade` `allowed = 1`; others `allowed = 0`.
`platform_t` gains `int demo_allowed, demo_seconds;`.

### 10.2 States

```
OFF ──idle ≥ screensaver_minutes (carousel, panel closed, not launching)──▶ ENTER (1.2 s)
ENTER ─▶ IDLE ──demo_enabled && candidates && t ≥ attract_demo_delay──▶ DEMO_OUT (1.5 s) ─▶ exit 11
any state ──any input──▶ LEAVE (0.6 s) ─▶ OFF   (that input is swallowed, never launches a game)
```

- Started with `--attract`: begin in `ENTER` after the start fade.
- `ui_start_attract()` ("Preview now") enters `ENTER` immediately.
- `screensaver_minutes = 0`: never automatically.
- Ignore input for the first 0.4 s after start (a button still held from the demo).

### 10.3 Scene

- Dim: a black full-screen quad after the 3D pass, alpha `0.35 * amount` (spring).
- Top bar, pills, title/subtitle and help bar fade out with `1 - amount`.
- Auto-rotation: every 4 s `move = +1` through the normal carousel path (covers stream as
  usual); after 6 moves, `plat_move = +1` if more than one platform has games.
- Orbit: the attract module provides per-case offsets added to the carousel targets in the
  animation loop (`dx`, `dy`, `dry`), small (≤ 0.08 units / 0.12 rad) sinusoids with a phase
  per offset `o`, scaled by `amount`.
- Ambient colour cycling: the `bg[]` spring target gets a slow hue rotation (period 24 s) of
  the cover colour, blended by `amount`.
- "PRESS START": `tr("attract.press_start")` in `g_font_title`, centred at y = 600, alpha
  blinking `0.25 ↔ 1.0` with a 1.6 s period, smoothstep edges; the `lumen` brand at the top-left
  stays visible.
- DEMO_OUT: fade to black; `trf("attract.demo_of", "title", …)` centred, then exit 11.

### 10.4 API

```c
/* attract.h */
typedef struct { int move, plat_move, launch_demo; float amount, dim; } attract_out_t;
void  attract_init(int start_active, const char *home);
void  attract_note_input(double t);                 /* any input this frame */
int   attract_active(void);                         /* ENTER/IDLE/DEMO_OUT */
void  attract_start(double t);
void  attract_update(double t, float dt, int can_start, attract_out_t *out);
void  attract_case_offset(int o, double t, float *dx, float *dy, float *dry);
void  attract_bg_target(const float base[3], double t, float out[3]);
void  attract_draw(double t);                       /* PRESS START / demo title, in the 2D block */
int   attract_pick_demo(void);                      /* g_games index or -1 */
```

### 10.5 Demo pick and hand-off (main.c + attract.c)

- Candidates: games whose platform has `demo_allowed` and a non-empty `core`. Random
  (`srand(time ^ getpid())` once), avoiding the last 4 picks stored in `/tmp/lumen-demo-history`
  (one content path per line, Lumen restarts between demos).
- Before exit 11, `main()`:
  1. writes `state.txt` with the **user's** selection from before the attract scene
     (attract saves `home_plat/home_sel` when it starts and restores them on LEAVE);
  2. writes `/tmp/lumen-demo-game` (`system=…` and `content=…`);
  3. `launch_write()` with `demo = 1`, `demo_seconds` = platform override or setting,
     the active graphics choice, profile = active (run.sh ignores it for demos).
- `--focus-demo`: read `/tmp/lumen-demo-game`, select that game (platform by `system`,
  game by `content`), delete the file, start in the normal carousel.

---

## 11. Implementation order, files and acceptance

Each step: implement, build with `./toolchain/psc lumen/build.sh` (no warnings, all `[OK]`),
update docs, commit (only its files, plain message). Each step must leave Lumen fully working.

### Step 1 — i18n + settings store

Creates: `src/lumen.h`, `src/version.h`, `src/i18n.c/.h`, `src/settings.c/.h`,
`dist/lang/{es,en,pt,fr,de,it}.lang`, `tools/check-lang.sh`.
Edits: `src/main.c` (move types/externs to `lumen.h`, drop `static` on the exported symbols,
`settings_load` + `i18n_init` + `i18n_set_language(settings_get("language","es"))` after
`TTF_Init`, every literal of 5.4 through `tr`/`trf`, `hint_t` keys, `hints_reload`,
`ui_language_changed`, settings save at exit), `build.sh` (add the .c files to the `lumen`
link line), `README.md` (USB layout: `/lumen/lang/`, `/lumen/settings.ini`).
Workspace (outside repo): `tools/install-lumen-boot.sh` and repo `tools/install.sh` copy
`dist/lang/*.lang` to `USB:/lumen/lang/` (do not run them).

Accept:
- builds clean; `tools/check-lang.sh` passes (all six files have exactly the es key set, no
  `{placeholder}` missing compared with es).
- `grep -n '"[A-Za-zÁÉÍÓÚáéíóú][a-záéíóúñ ]\{3,\}' src/main.c` finds no user-visible literal
  outside log messages and asset paths.
- with `language=en` in settings.ini every carousel string is English; with a missing key in
  `pt.lang` the en text appears; with no `lang/` folder the keys appear and one warning is logged.
- deleting `settings.ini` → defaults; after exit the file exists with all keys; a `.tmp` file
  never remains.

### Step 2 — panel UI + on-screen keyboard

Creates: `src/panel.c/.h`.
Edits: `src/main.c` (`IN_OPTIONS`, `IN_BACK`, keyboard remap, `ui_nav_t` with repeat, panel
hooks 6.3, FS_2D mode 5 + `u2_rect` + `draw_round_rect`, hint sets 6.8, `ui_start_attract`
stub), `build.sh`, `README.md` (controls table: Triangle = settings).

Accept:
- Triangle / Tab / F1 opens; the carousel keeps animating dimmed; Square/Triangle closes; the
  help bar shows the panel hints and crossfades back.
- Language page switches the whole UI live (help bar, top bar, back of the case after
  re-selection, panel itself) without restart; the choice persists after restart.
- Screensaver values cycle and persist; Graphics shows `gfx.none`; About shows version, game
  count, systems and free space.
- The keyboard and the confirm dialog are implemented and compiled (they are first reachable
  from the Profile page in step 3, where they are tested); the fps log stays at the same level with the panel open
  (60 fps target as today).
- No `malloc` inside `panel_update`/`panel_draw` (only in open/close of the keyboard).

### Step 3 — profiles + launch protocol v2 + run.sh/lumen.conf

Creates: `src/profiles.c/.h`, `src/launch.c/.h`.
Edits: `src/main.c` (profile chip, back-card text, launch via `launch_write`), `src/panel.c`
(Profile page, keyboard + confirm wiring), `scripts/run.sh` (8.1, 8.2, 8.4; v1 fallback),
`build.sh`, `README.md` (profiles, USB layout `/lumen/profiles/`), workspace
`psc/lumen.conf` (8.3: `LUMEN_MAIN_SAVES`, `LUMEN_MAIN_STATES`,
`LUMEN_CORE_OPT_DIR`, append `LUMEN_RA_EXTRA`).

Accept:
- create "Ana" → `profiles/p1/{profile.ini,saves,states}`; rename, recolour, switch, delete
  (with confirmation; `main` cannot be deleted; 9th profile refused).
- `/tmp/lumen-launch` is v2 and `sh -n scripts/run.sh` passes; run.sh run on the Mac with a
  fake `lumen` (writes a request, exits 10) and a fake `run_retroarch` (prints its env and
  `LUMEN_RA_EXTRA` content) shows: profile `main` → `LUMEN_MAIN_*` dirs (or none), profile `p1`
  → `<home>/profiles/p1/saves|states`; a v1 two-line request still launches.
- on the console (by the user): a game saved under `main` keeps its existing memory card;
  under `p1` it starts with an empty card.
- no string containing the loader's name appears in the repo (`git grep -i` check).

### Step 4 — graphics presets

Edits: `src/main.c` (`platform_t` gfx fields, `platform_from_decl`, launch fields),
`src/panel.c` (Graphics page), `src/launch.c` (opt/ra lines), `scripts/run.sh`
(`lumen_merge_opt`, `ra.*` into the extra cfg), `dist/systems/*.ini` (9.2),
`docs/systems.md` (9.1).

Accept:
- Graphics page lists psx, snes, megadrive, gba, arcade; values persist as `graphics.<id>`.
- a fake run (as in step 3) with an existing `.opt` containing other keys: declared keys are
  replaced, missing ones appended, others untouched, `.lumen-bak` created once; switching back
  to Original writes the `original` values.
- `ra.video_smooth` appears in the extra cfg.
- on the console (by the user): PlayStation Enhanced renders at high internal resolution.

### Step 5 — screensaver / demo + lumen-buttons watcher

Creates: `src/attract.c/.h`.
Edits: `src/main.c` (`--attract`, `--focus-demo`, idle tracking, attract hooks in the logical
actions, animation targets, ambient colour, 2D block, exit 11 path), `src/panel.c`
(`ss.preview`), `src/buttons.c` (8.7), `scripts/run.sh` (exit 11, `lumen_run_demo`, ARGS),
`dist/systems/*.ini` (`[demo]`), `docs/systems.md` (`[demo]`), `README.md` (attract mode),
`build.sh` (attract.c).

Accept:
- with `screensaver_minutes=1`: after 1 min idle the attract scene fades in, rotates, cycles
  colour, blinks the localized PRESS START; any button returns smoothly to the user's game
  without launching it.
- with demos on and `attract_demo_delay=5`: Lumen exits 11 with a `mode=demo` request for a
  psx/arcade game, not one of the last 4.
- `lumen-buttons --demo-watch 10 --pid <sleep pid>` on the Mac is not possible (Linux input);
  instead: builds clean, `psc-check` OK, and on the console (by the user) the demo ends after
  `demo_seconds` → attract resumes; a button press ends it → carousel with that game selected;
  `/tmp/lumen-demo` is gone and no new file appears in the main or profile save folders.
- `sh -n scripts/run.sh`; the fallback path (no lumen-buttons) is exercised with the fake
  `run_retroarch` (a `sleep 100`) and a short `demo_seconds`.
- no regression: normal launches, profiles and graphics from steps 3–4 still work.

---

## 12. Out of scope

- A "DEMO" badge inside RetroArch (not possible with RetroArch 1.8.8 without patching it).
- Per-game graphics overrides, per-game demo exclusion, non-Latin scripts (would need fonts).
- Cloud/USB export of profiles.
