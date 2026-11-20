/* Software update over the network (Settings > System > Software update), the same way
 * lpsx.dev updates a stick (lumen-web, src/app/core/update.ts):
 *
 *   1. what is the latest release: lpsx.dev/services/releases (GitHub's API as a fallback),
 *      compared with the installed version (<home>/VERSION);
 *   2. its MANIFEST (SHA-256 checked against SHA256SUMS) against the one this install left
 *      on the stick (<home>/MANIFEST): only what changed is fetched, as byte ranges of the
 *      release zip, inflated and checked file by file (size + SHA-256);
 *   3. everything goes to <home>/.update/ first (files/, PLAN, MANIFEST); READY, written
 *      last, is the commit point. Nothing of the running install changes yet;
 *   4. "Restart and update": Lumen exits 13, boot.sh runs update.sh from RAM (nothing of
 *      Lumen is in use then), which moves the files into place, removes what the release
 *      dropped, writes MANIFEST last, deletes .update/ and restarts the console. A power cut
 *      in between: boot.sh finds READY at the next power-on and finishes the job.
 *
 * User data is never touched: settings.ini, state.txt, profiles/ (saves, memory cards,
 * avatars), logs/, backup/, the games and anything that is not in a release manifest.
 * lumen.conf and retroarch.cfg, when edited by the user, get the new copy as <file>.new.
 * A download that did not finish leaves nothing behind: .update/ without READY is removed.
 *
 * Settings: update_auto (0/1, default 1: look once a day while online), update_last
 * (epoch of the last successful check, only with the clock set over NTP). */
#ifndef LUMEN_UPDATE_H
#define LUMEN_UPDATE_H

enum {
   UPD_IDLE,         /* not checked yet this session */
   UPD_CHECKING,
   UPD_CURRENT,      /* up to date */
   UPD_AVAILABLE,    /* a newer release: update_info() */
   UPD_PREPARING,    /* comparing, downloading, verifying: update_progress() */
   UPD_READY,        /* staged and verified: restart to install */
   UPD_ERROR         /* update_error(): a tr key */
};

typedef struct {
   int    state;
   char   version[32];        /* the release's, "0.23.0" */
   char   installed[32];      /* this install's */
   long long download;        /* bytes to download (compressed), known once prepared */
   int    files;              /* files to write */
   float  progress;           /* 0..1 while UPD_PREPARING */
   const char *phase;         /* tr key of what UPD_PREPARING is doing */
   const char *error;         /* tr key, UPD_ERROR */
   long long last_check;      /* epoch, 0 = never */
} update_info_t;

void update_init(void);                 /* after settings + net: clears an unfinished download */
void update_poll(double now, int online, int clock_ok);   /* each frame: the daily check */
const update_info_t *update_info(void); /* the UI's copy, refreshed by update_poll */
const char *update_take_toast(void);    /* a tr key to show once ("update available" ...) */

void update_check(void);                /* "Check for updates" */
void update_prepare(void);              /* "Download": stage the update */
void update_cancel(void);               /* stop a download; the staging folder is removed */
int  update_staged(void);               /* READY on the stick: exit 13 installs it */

#endif
