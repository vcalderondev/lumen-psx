/* Top-right status of the dashboard: clock (only when the time is known), network.
 * The PSC has no RTC battery: every boot starts at 2018-09-01, so the clock is shown only
 * when the system time looks synced (year >= 2024). Polled at most every 2 s. */
#ifndef LUMEN_STATUS_H
#define LUMEN_STATUS_H

typedef struct {
   int  clock_ok;          /* time is plausible: show the clock */
   char clock[8];          /* "HH:MM" (24 h) when clock_ok */
   int  net_up;            /* a non-loopback interface is up with carrier */
} status_t;

void status_update(double t, status_t *out);   /* t = now_s(); refreshes every 2 s, else copies the cache */

#endif
