/* Connections cards over the Settings panel (lumen-design: connections-wifi.html,
 * connections-bluetooth.html): the Wi-Fi networks and the Bluetooth controllers, in the
 * library check's card. Data and actions: net.h. */
#ifndef LUMEN_CONN_H
#define LUMEN_CONN_H
#include "lumen.h"

void  conn_open_wifi(void);
void  conn_open_bt(void);
int   conn_active(void);          /* open, away for a keyboard / question, or fading out */
int   conn_open_now(void);        /* open: owns the input */
int   conn_wifi_shown(void);      /* which card is up (or about to come back) */
int   conn_bt_shown(void);
float conn_amount(void);
int   conn_hint_set(void);        /* HINTS_WIFI / HINTS_BT */
void  conn_update(const ui_nav_t *nav, double t, float dt);
void  conn_draw(double t);        /* 2D overlay, over the panel, under the toast */

#endif
