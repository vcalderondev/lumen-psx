/* First-run presentation (src/welcome.c). */
#ifndef LUMEN_WELCOME_H
#define LUMEN_WELCOME_H
#include "lumen.h"

void  welcome_init(void);                     /* after settings, fonts and i18n */
int   welcome_active(void);                   /* 1 while shown or fading out: owns the input */
float welcome_amount(void);                   /* 0..1 */
void  welcome_update(const ui_nav_t *nav, float dt);
void  welcome_draw(double t);                 /* in the 2D overlay, over everything but the fade */
int   welcome_last_page(void);                /* help bar: "Next" or "Start" */
int   welcome_language_page(void);            /* help bar: the language list (first run) */

#endif
