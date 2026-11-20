/* "No controller": a card over the dashboard whenever no controller is connected, in two
 * steps. 1: plug a USB Bluetooth receiver into a free port (or a wired controller). 2, as soon
 * as a receiver is there (already, or once plugged in): Bluetooth is switched on and the first
 * game controller put in pairing mode is paired with no question (net_pad_watch); the card
 * shows how the usual controllers enter pairing mode and what is going on, and closes by
 * itself once a controller is connected. For consoles whose two front ports are all they
 * have (no OTG adapter at the back): one port for the receiver, controllers over the air.
 * LUMEN_PADWAIT=0 turns it off (psc-sim, which has no controllers). */
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
