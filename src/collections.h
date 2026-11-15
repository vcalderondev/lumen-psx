/* Collections: favourites and folders of the active profile, and the "resume" marks left
 * by Save and exit (OPEN in a game).
 *
 * <home>/profiles/<profile>/collections.tsv, one entry per line (content path = game key):
 *    fav\t<content>
 *    folder\t<name>\t<content>
 * A folder exists while it holds a game: creating one always adds the current game, and
 * removing its last game removes it. Written atomically (tmp + rename) after each change.
 *
 * <home>/profiles/<profile>/resume.tsv (written by scripts/run.sh after Save and exit):
 *    <content>\t<epoch>
 * The state itself is RetroArch's slot LUMEN_RESUME_SLOT (run.sh), loaded with --entryslot. */
#ifndef LUMEN_COLLECTIONS_H
#define LUMEN_COLLECTIONS_H

#define COLL_MAX_FOLDERS 12
#define COLL_NAME_MAX    48

void        coll_load(const char *home, const char *profile_id);   /* (re)read; profile change */
int         coll_is_fav(const char *content);
void        coll_toggle_fav(const char *content);
int         coll_nfolders(void);
const char *coll_folder_name(int f);
int         coll_in_folder(int f, const char *content);
void        coll_toggle_folder(int f, const char *content);
/* new folder holding content: 0 ok, -1 full, -2 name taken (case-insensitive), -3 empty */
int         coll_create_folder(const char *name, const char *content);
int         coll_folder_find(const char *name);   /* index or -1 */
/* members, in file order (the dashboard sorts each row by title) */
int         coll_fav_count(void);
const char *coll_fav_at(int i);
int         coll_folder_count(int f);
const char *coll_folder_at(int f, int i);
unsigned    coll_generation(void);   /* bumps on every change: the dashboard rebuilds its rows */

int         resume_has(const char *content);     /* a Save and exit state is waiting */
void        resume_forget(const char *content);  /* "Start over": the state file is kept */

#endif
