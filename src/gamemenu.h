/* Game menu (Start on the dashboard) and the resume question (Play on a game that was
 * left with Save and exit). A card over the dashboard:
 *   Play / Add to favourites (Remove from) / Add to folder  v  (drops down: one row per
 *   folder with a check, "New folder...")
 * Resume: Continue where you left off / Start over. Circle closes (or folds the folders). */
#ifndef LUMEN_GAMEMENU_H
#define LUMEN_GAMEMENU_H
#include "lumen.h"

enum { GM_NONE, GM_PLAY, GM_PLAY_RESUME };

void  gamemenu_open(int game_index);     /* Start */
void  gamemenu_resume_open(int game_index);
int   gamemenu_active(void);             /* open or still fading out: owns the input when open */
int   gamemenu_open_now(void);           /* open (not fading out) */
float gamemenu_amount(void);
void  gamemenu_update(const ui_nav_t *nav, double t, float dt);
void  gamemenu_draw(double t);
int   gamemenu_folders_open(void);      /* the folder list is dropped down (help bar) */
int   gamemenu_take(void);               /* GM_* chosen since the last call (once) */

#endif
