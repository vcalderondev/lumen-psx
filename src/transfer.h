/* USB transfer: copy files between the Lumen stick and a second USB drive with the pad
 * (lumen-design, usb-transfer*.html; docs/usb-transfer.md).
 *
 * A watcher thread notices a second USB storage device, mounts it read-only on
 * /tmp/lumen-usb2 and tells the UI, which offers the transfer with a pill above the help
 * bar (Square opens it; nothing opens by itself). Settings, Library opens it too.
 * The card has two panes, one per drive: Left / Right change pane, Cross opens a folder
 * or marks a file, Square marks, Start copies what is marked to the other drive (a list
 * of destinations: the folder open in the other pane, then one per system of the
 * library), Triangle ejects the second drive, Circle goes up one folder or closes.
 *
 * Nothing slow runs on the render thread: listings, the copy and every mount, sync and
 * unmount run on a worker. A copy writes each file under a temporary name, flushes it
 * and renames it; the second drive is writable only while it is the destination. */
#ifndef LUMEN_TRANSFER_H
#define LUMEN_TRANSFER_H
#include "lumen.h"

void  transfer_init(void);        /* after settings + i18n: starts the drive watcher */
void  transfer_shutdown(void);    /* on exit: stops a running copy, unmounts the second drive */
void  transfer_open(void);        /* a toast instead when there is no second drive */
int   transfer_active(void);      /* open or still fading out */
int   transfer_open_now(void);    /* open: owns the input */
float transfer_amount(void);
int   transfer_hint_set(void);    /* HINTS_* of the layer with the focus */
/* nav: this frame's input (zeroed by the caller when another layer owns it);
 * home_free: the Home has the input, so the offer pill may show and Square opens the card */
void  transfer_update(const ui_nav_t *nav, double t, float dt, int home_free);
void  transfer_draw(double t);    /* 2D overlay: over the panel and the file picker, under the toast */

#endif
