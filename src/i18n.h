/* Translation layer: <home>/lang/<code>.lang, "key = value" lines (docs/settings-design.md 5). */
#ifndef LUMEN_I18N_H
#define LUMEN_I18N_H
#include <stddef.h>

#define I18N_MAX_LANGS 16

int         i18n_init(const char *home);          /* scans <home>/lang; loads "en" as fallback */
int         i18n_set_language(const char *code);  /* 0 ok; -1 unknown code (stays on previous) */
const char *i18n_language(void);                  /* active code */
int         i18n_count(void);
const char *i18n_code(int i);
const char *i18n_name(int i);                     /* that language's "lang.name" */
const char *i18n_main_profile_name(int i);        /* that language's "profile.main" ("" if none) */
unsigned    i18n_generation(void);                /* ++ on every successful language change */
const char *tr(const char *key);                  /* active -> en -> key itself */
const char *trf(char *buf, size_t n, const char *key, ...);
            /* name/value pairs, NULL terminated:
               trf(s, sizeof s, "top.games", "n", "12", "system", pl->name, NULL) */

#endif
