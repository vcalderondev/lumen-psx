/* UI kit: the components every menu is built from (lumen-design, guidelines/04).
 * A menu is the dashboard with a glass card on it: the art stays behind a scrim, the
 * focus is always a white ring, selection is never a filled block of colour. */
#ifndef LUMEN_UI_H
#define LUMEN_UI_H
#include "lumen.h"

#define UI_SCRIM     0.60f   /* scrim.menu                                  */
#define UI_REST      0.08f   /* glass.rest: buttons, keys, "you are here"   */
#define UI_SELECTED  0.24f   /* glass.selected: the focused element         */
#define UI_EDGE      0.16f   /* glass.edge                                  */
#define UI_CARD_R    20.0f

void ui_scrim_skip(int on);   /* 1: the next ui_scrim() draws nothing (its scrim is folded into the frame) */
void ui_scrim(float a);                                          /* full screen, under a menu's card */
void ui_card(float x, float y, float w, float h, float a);       /* glass card + elev.3 */
void ui_rest(float x, float y, float w, float h, float radius, float a);    /* glass.rest shape with its edge */
void ui_ring(float x, float y, float w, float h, float radius, float gap, float a);   /* 3 px white ring, gap px away */
void ui_focus(float x, float y, float w, float h, float radius, float a);   /* glass.selected + elev.1 + ring */
void ui_fill(float x, float y, float w, float h, float radius, const float rgb[3], float a);   /* flat shape */

/* s, or s cut with an ellipsis so it fits max_w logical px in font f */
const char *ui_fit(TTF_Font *f, const char *s, float max_w, char *buf, size_t n);
/* the kicker above a title: `text` in upper case (ASCII and Latin-1), muted, caps font */
void ui_kicker(label_t *l, const char *text, float x, float y, float a);

#endif
