/* Settings store: <home>/settings.ini, "key=value" lines, no sections (docs/settings-design.md 4). */
#ifndef LUMEN_SETTINGS_H
#define LUMEN_SETTINGS_H

#define SETTINGS_MAX 64

int         settings_load(const char *home);             /* 0 ok, -1 missing (defaults) */
const char *settings_get(const char *key, const char *def);
int         settings_get_int(const char *key, int def, int lo, int hi);
void        settings_set(const char *key, const char *value);   /* marks dirty if changed */
void        settings_set_int(const char *key, int value);
int         settings_dirty(void);
int         settings_save(void);                          /* 0 ok, -1 error; clears dirty */
int         settings_fsync_dir(const char *dir);          /* fsync a folder (durable rename/mkdir) */

#endif
