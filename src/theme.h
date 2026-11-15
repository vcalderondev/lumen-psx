/* Themes: <home>/themes/<id>/theme.ini plus optional art (docs/themes.md).
 * The active theme is settings "theme" (default: "default" = Lumen's own look, no folder).
 * A theme overrides colours and any runtime asset: a file at <home>/themes/<id>/<rel> wins
 * over <home>/assets/<rel> (fonts/, glyphs/, icons/, music/menu.ogg, systems/<id>/logo.png ...). */
#ifndef LUMEN_THEME_H
#define LUMEN_THEME_H
#include <stddef.h>

#define THEME_MAX 16

enum {
   TC_ACCENT,     /* kicker text, checks, highlights          */
   TC_PILL,       /* selection pill of menus and dialogs       */
   TC_CARD,       /* dialog / menu card fill                   */
   TC_TEXT,       /* primary text                              */
   TC_DIM,        /* secondary text                            */
   TC_BACKDROP,   /* full-screen backdrop (dialog, scrims)     */
   TC_PANEL,      /* settings panel background                 */
   TC_DANGER,     /* destructive rows                          */
   TC_COUNT
};

void        theme_init(const char *home);        /* scans <home>/themes; loads settings "theme" */
int         theme_count(void);                   /* "default" included, first */
const char *theme_id(int i);
const char *theme_name(int i);                   /* name= of the ini, else the id */
int         theme_active_index(void);
const char *theme_active_id(void);
void        theme_color(int which, float rgb[3]);
float       theme_card_alpha(void);
/* <home>/themes/<active>/<rel> when it exists: 1 and out set; else 0 (out untouched) */
int         theme_asset(char *out, size_t n, const char *rel);

#endif
