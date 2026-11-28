/* UI kit: the components every menu is built from (lumen-design, guidelines/04).
 * A menu is the dashboard with a glass card on it: the art stays behind a scrim, the
 * focus is always a ring (the theme's `focus`, white in Lumen's own look), selection is
 * never a filled block of colour. Every colour here comes from the theme (theme.h). */
#ifndef LUMEN_UI_H
#define LUMEN_UI_H
#include "lumen.h"
#include "theme.h"

#define UI_REST      0.08f   /* glass.rest: buttons, keys, "you are here"   */
#define UI_EDGE      0.16f   /* glass.edge                                  */
#define UI_CARD_R    22.0f
#define UI_CTL_R     14.0f   /* buttons and rows: a control is not a pill (chips and keycaps are) */

void ui_scrim_skip(int on);   /* 1: the next ui_scrim() draws nothing (its scrim is folded into the frame) */
void ui_scrim(float a);                                          /* full screen, under a menu's card */
/* the theme's amounts (theme.h TV_*): the scrim fully up behind the game menu and dialogs,
 * behind Settings, and the focused element's glass fill */
#define ui_scrim_alpha()    theme_value(TV_SCRIM)
#define ui_scrim_settings() theme_value(TV_SCRIM_SETTINGS)
#define UI_SELECTED         theme_value(TV_SELECTED)
void ui_card(float x, float y, float w, float h, float a);       /* glass card + elev.3 */
void ui_rest(float x, float y, float w, float h, float radius, float a);    /* glass.rest shape with its edge */
void ui_ring(float x, float y, float w, float h, float radius, float gap, float a);   /* 3 px focus ring, gap px away */
void ui_focus(float x, float y, float w, float h, float radius, float a);   /* glass.selected + elev.1 + ring */
void ui_fill(float x, float y, float w, float h, float radius, const float rgb[3], float a);   /* flat shape */

/* s, or s cut with an ellipsis so it fits max_w logical px in font f */
const char *ui_fit(TTF_Font *f, const char *s, float max_w, char *buf, size_t n);
/* the kicker above a title: `text` in upper case (ASCII and Latin-1), muted, caps font */
void ui_kicker(label_t *l, const char *text, float x, float y, float a);

/* ---- Controls: the value side of a row (lumen-design, guidelines/04 "Controls").
 * One component per kind of value, and no screen draws its own:
 *   on / off                      ui_switch
 *   one of 2 or 3 named options   ui_segmented
 *   one of many, or a number      ui_stepper     ( < value > inside a well )
 *   an amount, 0 .. 100 %         ui_slider
 *   opens a page or a card        ui_chevron
 *   the chosen item of a list     ui_mark
 * Each is drawn right-aligned at xr and centred on cy (the row's middle), and returns the
 * width it took, so a row lays its value side out right to left. `a` is the row's alpha.
 * The caller keeps the text caches (label_t), one per row, like every label here. */
#define UI_CTL_H     36.0f   /* height of segmented, stepper: the wells */
#define UI_WELL_R    12.0f   /* a well's corners; its inner pill has 9 */
typedef struct { label_t l[3]; } ui_seg_t;
float ui_switch(float xr, float cy, int on, float a);
float ui_segmented(ui_seg_t *s, float xr, float cy, const char *const *opts, int n, int sel, float a);
/* dot: the colour of a swatch shown before the value (a profile's colour), NULL none */
float ui_stepper(label_t *l, float xr, float cy, const char *value, float max_w, int at_lo, int at_hi, const float *dot, float a);
float ui_slider(label_t *l, float xr, float cy, float v, const char *text, float a);   /* v 0..1 */
float ui_chevron(float xr, float cy, float a);
float ui_mark(float xr, float cy, float a);
/* a group of rows: its name in small caps (the kicker's style), at the rows' left edge */
void  ui_group(label_t *l, const char *text, float x, float y, float a);

#endif
