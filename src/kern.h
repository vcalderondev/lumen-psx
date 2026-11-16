/* The console's kernel (lumen-design: kernel-restore.html, kernel-restore-done.html):
 * Settings, About shows which one runs; on a kernel that is not Sony's stock build a card
 * offers to put Sony's back (or, later, the previous one) with scripts/kernel.sh, the only
 * thing in Lumen that writes to the internal memory. */
#ifndef LUMEN_KERN_H
#define LUMEN_KERN_H
#include "lumen.h"

void        kern_init(void);           /* asks kernel.sh status on a thread */
const char *kern_name_key(void);       /* tr key of the running kernel ("" until known) */
void        kern_open(void);
int         kern_active(void);
int         kern_open_now(void);
float       kern_amount(void);
int         kern_hint_set(void);
void        kern_update(const ui_nav_t *nav, double t, float dt);
void        kern_draw(double t);

#endif
