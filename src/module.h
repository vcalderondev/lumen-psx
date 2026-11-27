/* Modules: features the user switches on and off in Settings > Modules, and the extension
 * points they contribute to (docs/modules.md, API version 1).
 *
 * A module never draws: it registers contributions (a status bar item, a game menu entry)
 * and Lumen draws them with its UI kit. Every text a module shows is a translation key
 * "mod.<id>.*" (all languages Lumen ships, English first). Everything here runs on the
 * render thread. */
#ifndef LUMEN_MODULE_H
#define LUMEN_MODULE_H
#include <stddef.h>

#define LUMEN_MODULE_API 1

/* lumen_module_t.flags */
#define MOD_F_DEFAULT_ON 0x1u   /* on until the user switches it off            */
#define MOD_F_RESTART    0x2u   /* the switch takes effect at the next start    */
#define MOD_F_REQUIRED   0x4u   /* cannot be switched off                       */

/* events (lumen_module_t.event) */
enum { MEV_STARTED, MEV_ONLINE, MEV_OFFLINE, MEV_LANGUAGE, MEV_LAUNCH, MEV_EXIT };

typedef struct {
   const char *id;                       /* a-z0-9-, 2..24 characters                    */
   int         api;                      /* LUMEN_MODULE_API                             */
   unsigned    flags;                    /* MOD_F_*                                      */
   const char *requires;                 /* space-separated module ids, or NULL          */
   const char *version;                  /* "1.0"                                        */
   const char *(*available)(void);       /* NULL = can run here; else a tr key: why not  */
   void (*start)(void);
   void (*stop)(void);
   void (*poll)(double t);               /* every frame while running                    */
   void (*event)(int ev, const void *arg);
} lumen_module_t;

/* state of a module (module_state) */
enum { MOD_OFF, MOD_ON, MOD_UNAVAILABLE, MOD_BLOCKED, MOD_RESTART };

/* ---- registry (main.c calls these) ---- */
void        modules_init(const char *home);    /* after settings, i18n and net_init: starts what is on */
void        modules_poll(double t);            /* every frame */
void        modules_emit(int ev, const void *arg);   /* to every running module */
int         modules_count(void);
const lumen_module_t *module_at(int i);        /* in display order */
int         module_index(const char *id);      /* -1 unknown */
int         module_state(int i);               /* MOD_* */
int         module_switch(int i);              /* the user's switch: 0/1 */
int         module_running(const char *id);    /* started and not stopped (MOD_ON) */
const char *module_why(int i);                 /* tr key for MOD_UNAVAILABLE / MOD_BLOCKED, else NULL */
const char *module_blocker(int i);             /* MOD_BLOCKED: the id of the required module that is off */
const char *module_name_key(int i);            /* "mod.<id>.name"        */
const char *module_desc_key(int i);            /* "mod.<id>.description" */
/* switch a module on/off: also on what it requires / off what requires it.
 * Returns how many other modules changed with it (for the toast), -1 refused (required). */
int         module_set(int i, int on);
int         modules_restart_pending(void);     /* a MOD_F_RESTART module changed its switch */

/* ---- extension point "status": the status bar (docs/modules.md 3) ---- */
#define MOD_STATUS_PER_MODULE 2
#define MOD_STATUS_MAX        6
typedef struct {
   const char *glyph;    /* assets/glyphs/<glyph>.png                    */
   int         dim;      /* drawn faint                                  */
   const char *badge;    /* up to 3 characters after the glyph, or NULL  */
   int         order;    /* lower = closer to the clock                  */
} mod_status_t;
int  mod_status_set(const char *owner, const char *slot, const mod_status_t *s);   /* 0 ok, -1 refused */
void mod_status_clear(const char *owner, const char *slot);
/* for the status bar: the items in order */
typedef struct { char owner[25], slot[16], glyph[48], badge[4]; int dim, order; } mod_status_item_t;
int  mod_status_count(void);
const mod_status_item_t *mod_status_item(int i);

/* ---- extension point "menu.game": the game menu (docs/modules.md 3) ---- */
#define MOD_MENU_PER_MODULE 2
#define MOD_MENU_MAX        4
typedef struct {
   const char *owner, *id;
   const char *label_key;                        /* "mod.<owner>.*"                     */
   int         order;
   int  (*visible)(int game_index, void *user);  /* NULL = always                       */
   void (*invoke)(int game_index, void *user);   /* the menu closes after it            */
   void       *user;
} mod_menu_t;
int  mod_menu_add(const mod_menu_t *m);          /* 0 ok, -1 refused; m's strings must outlive it */
void mod_menu_remove(const char *owner, const char *id);
/* for the game menu: the entries that apply to a game, in order */
int  mod_menu_list(int game_index, int *out, int max);   /* entry handles */
const char *mod_menu_label(int handle);          /* translated */
void mod_menu_invoke(int handle, int game_index);

/* is a translation key one this module may use ("mod.<id>." prefix) */
int  module_key_ok(const char *owner, const char *key);

#endif
