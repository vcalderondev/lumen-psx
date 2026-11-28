/* Themes: <home>/themes/<id>/theme.ini plus optional art (docs/themes.md).
 * The active theme is settings "theme" (default: "default" = Lumen's own look, no folder).
 * A theme overrides colours (the TC_ tokens below, one key of theme.ini each) and any runtime asset: a file at <home>/themes/<id>/<rel> wins
 * over <home>/assets/<rel> (fonts/, glyphs/, icons/, music/menu.ogg, systems/<id>/logo.png ...). */
#ifndef LUMEN_THEME_H
#define LUMEN_THEME_H
#include <stddef.h>

#define THEME_MAX 16

enum {
   TC_ACCENT,     /* kicker text, checks, highlights          */
   TC_CARD,       /* dialog / menu card fill                   */
   TC_TEXT,       /* primary text                              */
   TC_DIM,        /* secondary text                            */
   TC_BACKDROP,   /* full-screen backdrop (dialog, scrims)     */
   TC_PANEL,      /* settings panel background                 */
   TC_DANGER,     /* destructive rows                          */
   TC_FOCUS,      /* focus ring of every screen                */
   TC_GLASS,      /* tint of glass shapes: buttons, tabs, the focused row, hairlines */
   TC_PRIMARY,    /* fill of the main button (Play)            */
   TC_ON_PRIMARY, /* text on the main button                   */
   TC_COUNT
};

/* amounts a theme sets (0 .. 1, one key of theme.ini each; docs/themes.md) */
enum {
   TV_SCRIM,          /* "scrim": how much the backdrop colour covers the screen behind the game menu and the dialogs */
   TV_SCRIM_SETTINGS, /* "scrim_settings": the same behind Settings                         */
   TV_SELECTED,       /* "selected": the glass fill of the focused row, button or key        */
   TV_CARD_LIGHT,     /* "card_light": the glass light down a card (0 = one flat colour)      */
   TV_FOCUS_GLOW,     /* "focus_glow": the soft halo of the focus colour around the ring       */
   TV_COUNT
};

void        theme_init(const char *home);        /* scans <home>/themes; loads settings "theme" */
int         theme_count(void);                   /* "default" included, first */
const char *theme_id(int i);
const char *theme_name(int i);                   /* name= of the ini, else the id */
void        theme_preview(int i, float card[3], float lit[3]);   /* theme i's swatch: its card and its light */
int         theme_active_index(void);
const char *theme_active_id(void);
void        theme_color(int which, float rgb[3]);
float       theme_card_alpha(void);              /* opacity of the card colour under the glass: 0.2 .. 1 */
float       theme_value(int which);              /* TV_*: the theme's amount, else Lumen's own */
/* the light of the focused game (tab underline, glow, kicker bar): 1 and rgb set when the
 * theme fixes it (`lit`); 0 = Lumen's own look, the light follows each game's cover */
int         theme_lit(float rgb[3]);
float       theme_radius(void);                  /* scale of every corner radius: 1 = Lumen's, 0 = square corners */
/* <home>/themes/<active>/<rel> when it exists: 1 and out set; else 0 (out untouched) */
int         theme_asset(char *out, size_t n, const char *rel);

#endif
