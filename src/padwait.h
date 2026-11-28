/* "No controller": a screen of its own whenever no controller is connected: the logo, one
 * pill saying what to do (press PS on a paired controller, pair a new one, or plug one in by
 * cable) and a controller in outline at the foot. With a Bluetooth receiver, Bluetooth is
 * switched on and the first game controller put in pairing mode is paired with no question
 * (net_pad_watch). It closes by itself as soon as a controller is connected, by cable or
 * over the air; Circle = not now. The receiver's own state is the status bar's Bluetooth
 * glyph. LUMEN_PADWAIT=0 turns it off (psc-sim, which has no controllers). */
#ifndef LUMEN_PADWAIT_H
#define LUMEN_PADWAIT_H
#include "lumen.h"

/* each frame: njoy = SDL_NumJoysticks(); blocked = another full-screen flow is up (the
 * welcome, the screensaver, a launch) and the card must not open now */
void  padwait_update(const ui_nav_t *nav, double t, float dt, int njoy, int blocked);
void  padwait_draw(double t);
int   padwait_open_now(void);    /* open: owns the input */
float padwait_amount(void);

#endif
