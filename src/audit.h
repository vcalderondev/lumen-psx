/* Library check (Settings, Library): looks at every game folder of every system declared
 * in <home>/systems/ and lists what would keep a game from showing up or from starting:
 * a missing Game.ini, a Discs= that names no file, a .cue that points to a file that is
 * not there, empty files, a .bin without its .cue, a missing core, a protected game
 * without its .sbi... (docs/library-check.md; lumen-design, library-audit.html).
 * Read-only: it reports, it never changes a file. The scan runs on its own thread. */
#ifndef LUMEN_AUDIT_H
#define LUMEN_AUDIT_H
#include "lumen.h"

void  audit_open(void);           /* opens the card and starts a check */
int   audit_active(void);         /* open or still fading out */
int   audit_open_now(void);       /* open: owns the input */
float audit_amount(void);
void  audit_update(const ui_nav_t *nav, double t, float dt);
void  audit_draw(double t);       /* 2D overlay, over the panel, under the toast */

#endif
