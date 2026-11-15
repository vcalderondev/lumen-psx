/* Profiles: each one has its own memory cards and save states (docs/settings-design.md 7).
 * <home>/profiles/<id>/profile.ini (name, color, created) + saves/ + states/ + avatar.png (optional).
 * "main" is built in, always exists and keeps the save folders RetroArch already uses. */
#ifndef LUMEN_PROFILES_H
#define LUMEN_PROFILES_H
#include <stddef.h>

#define MAX_PROFILES 8
#define PROFILE_COLORS 8

typedef struct { char id[16]; char name[52]; int color; long created; int avatar; } profile_t;   /* avatar: has avatar.png */

int              profiles_load(const char *home);        /* scans <home>/profiles, adds "main" first */
int              profiles_count(void);
const profile_t *profiles_get(int i);                    /* 0 = main, others sorted by created */
int              profiles_find(const char *id);          /* index or -1 */
const profile_t *profiles_active(void);                  /* from settings "profile", falls back to main */
const char      *profiles_display_name(const profile_t *p);  /* name or tr("profile.main") */
int              profiles_set_active(const char *id);    /* settings_set only (saved on panel close / exit); 0 / -1 unknown */
int              profiles_create(const char *name, int color, char *id_out, size_t n);
                 /* 0 ok, -1 full, -2 name taken, -3 I/O; creates saves/ and states/.
                    color outside 0..7 = first colour not used by another profile */
int              profiles_rename(const char *id, const char *name);   /* 0 / -2 / -3 */
int              profiles_set_color(const char *id, int color);       /* 0 / -3 */
int              profiles_delete(const char *id);        /* 0 / -1 main / -3 I/O */
int              profiles_name_taken(const char *name, const char *except_id);  /* case-insensitive */
void             profiles_color(int idx, float rgb[3]);

/* Avatar: <home>/profiles/<id>/avatar.png, a square of at most AVATAR_PX (else initials). */
#define AVATAR_PX 256
int              profiles_set_avatar(const char *id, const char *image);
                 /* centre square of any image file, scaled down, written as avatar.png;
                    0 / -3. Slow (decodes the file) and thread-safe: meant for a worker */
int              profiles_clear_avatar(const char *id);  /* back to the initials; 0 / -3 */
void             profiles_avatar_refresh(const char *id);  /* render thread: re-read profile_t.avatar */

#endif
