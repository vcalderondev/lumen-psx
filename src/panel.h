/* Settings panel, on-screen keyboard and confirm dialog (docs/settings-design.md 6). */
#ifndef LUMEN_PANEL_H
#define LUMEN_PANEL_H
#include "lumen.h"

/* help bar sets (main.c draws them; the panel says which one applies) */
enum { HINTS_CAROUSEL, HINTS_PANEL_ROOT, HINTS_PANEL_PAGE, HINTS_PANEL_LIST, HINTS_PANEL_INFO,
       HINTS_OSK, HINTS_CONFIRM, HINTS_ATTRACT, HINTS_WELCOME, HINTS_WELCOME_LAST, HINTS_WELCOME_LANG, HINTS_GAMEMENU,
       HINTS_GAMEMENU_FOLDERS, HINTS_FILEPICKER, HINTS_DASH_TABS, HINTS_DASH_PROFILE,
       HINTS_TRANSFER, HINTS_TRANSFER_COPY, HINTS_AUDIT, HINTS_COUNT };

void  panel_init(void);                 /* after fonts + i18n + settings */
void  panel_open(void);
void  panel_open_profile(void);        /* straight into the Profile page (dashboard: Cross on the avatar) */
void  panel_shutdown(void);             /* on exit: commit pending edits, save settings */
int   panel_active(void);               /* 1 while open or closing (input owned by the panel) */
float panel_amount(void);               /* 0..1 open spring, for the backdrop and hint fade */
void  panel_update(const ui_nav_t *nav, double t, float dt);
void  panel_draw(double t);             /* inside the 2D overlay block, after hints, before flash/fade */
int   panel_hint_set(void);             /* which HINTS_* to show */
void  panel_toast(const char *key);     /* translated message in a pill above the help bar, 2.5 s */
void  panel_toast_draw(double t);
/* the on-screen keyboard alone over the dashboard; accept returns NULL (done) or a tr key */
void  panel_osk(const char *title_key, const char *initial, const char *(*accept)(const char *));

/* callbacks implemented in main.c */
void  ui_language_changed(void);        /* section 5.3 */
void  ui_sound_changed(void);           /* "music" / "music_volume" changed: apply them */
void  ui_start_attract(void);           /* "Preview now" (step 5; a no-op before) */
void  ui_request_reboot(void);          /* Display page: save and restart the console */
void  ui_request_restart(void);         /* theme: restart Lumen only (run.sh starts it again) */

#endif
