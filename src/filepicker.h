/* File picker: a card over whatever is on screen that browses the folders of the USB
 * stick and returns one file. Generic: the caller says which files count, how to preview
 * them and what to do with the chosen one (first user: the profile avatar, panel.c).
 *
 *   static const char *use_it(const char *path, void *user) { ...slow work...; return NULL; }
 *   static void used(const char *path, void *user) { ...refresh the UI... }
 *   filepicker_open(&(filepicker_t){ .id = "wallpaper", .title_key = "wall.pick",
 *                                    .exts = FP_EXTS_IMAGE, .preview = FP_PREVIEW_FIT,
 *                                    .accept = use_it, .done = used });
 *
 * Controls: D-pad moves (Right / Cross open a folder, Left / Circle go up one), Cross on
 * a file picks it, L1 / R1 page, Circle at the top folder or Triangle cancel. */
#ifndef LUMEN_FILEPICKER_H
#define LUMEN_FILEPICKER_H
#include "lumen.h"

/* what the console's SDL_image decodes (no WebP library on the PSC) */
#define FP_EXTS_IMAGE "png,jpg,jpeg,bmp,gif"

enum { FP_PREVIEW_NONE,     /* name and size only                                    */
       FP_PREVIEW_FIT,      /* the whole image, fitted into the preview box           */
       FP_PREVIEW_ROUND };  /* centre square in a circle (how an avatar will look)    */

typedef struct {
   const char *id;         /* short name; the last folder used is kept in settings "picker.<id>" */
   const char *title_key;  /* tr key of the card's title                                          */
   const char *root;       /* top folder, the picker never leaves it; NULL = the stick (g_root)   */
   const char *exts;       /* "png,jpg" (lower case, no dots) = only those files; NULL = all      */
   const char *empty_key;  /* tr key shown in a folder with nothing to pick; NULL = "fp.empty"    */
   const char *busy_key;   /* tr key shown while accept runs; NULL = "fp.working"                 */
   int         preview;    /* FP_PREVIEW_*                                                        */
   /* accept: optional, runs on a WORKER thread with the chosen file (no GL, no UI calls):
    * do the slow part here (decode, convert, copy). NULL = accepted and the picker closes;
    * else a tr key shown as a toast, and the picker stays open. */
   const char *(*accept)(const char *path, void *user);
   /* done: optional, on the render thread right after the picker accepted the file */
   void        (*done)(const char *path, void *user);
   void       *user;
} filepicker_t;

void  filepicker_open(const filepicker_t *req);   /* the keys must stay valid while open (literals) */
int   filepicker_active(void);     /* open or still fading out */
int   filepicker_open_now(void);   /* open: owns the input */
float filepicker_amount(void);
void  filepicker_update(const ui_nav_t *nav, double t, float dt);
void  filepicker_draw(double t);   /* 2D overlay, over the panel, under the toast */

#endif
