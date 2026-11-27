/* Modules: the registry, the user's switches (<home>/modules/state.ini) and the extension
 * points "status" and "menu.game" (docs/modules.md). */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "lumen.h"
#include "i18n.h"
#include "module.h"
#include "settings.h"

/* the built-in modules (src/mod_net.c), in the order Settings > Modules lists them */
extern const lumen_module_t mod_wifi, mod_ethernet, mod_bluetooth;
static const lumen_module_t *const k_builtin[] = { &mod_wifi, &mod_ethernet, &mod_bluetooth };

#define MOD_MAX 32

typedef struct {
   const lumen_module_t *m;
   int  sw;              /* the user's switch                                  */
   int  boot_sw;         /* the switch when Lumen started (MOD_F_RESTART)      */
   int  running;         /* start() called, stop() not yet                     */
   char name_key[40], desc_key[48];
} mod_t;

static mod_t  s_mod[MOD_MAX];
static int    s_n;
static char   s_state_path[300];
static double s_next_check;

/* ------------------------------------------------------------------------ */
/* ids, state file                                                           */

static int id_ok(const char *id)
{
   static const char *const reserved[] = { "lumen", "core", "system", "settings" };
   size_t n = id ? strlen(id) : 0, i;
   if (n < 2 || n > 24) return 0;
   for (i = 0; i < n; i++)
      if (!((id[i] >= 'a' && id[i] <= 'z') || (id[i] >= '0' && id[i] <= '9') || id[i] == '-')) return 0;
   for (i = 0; i < sizeof(reserved) / sizeof(reserved[0]); i++) if (!strcmp(id, reserved[i])) return 0;
   return 1;
}

int module_index(const char *id)
{
   int i;
   if (!id) return -1;
   for (i = 0; i < s_n; i++) if (!strcmp(s_mod[i].m->id, id)) return i;
   return -1;
}

static void state_load(void)
{
   char line[128];
   FILE *f = fopen(s_state_path, "r");
   if (!f) return;
   while (fgets(line, sizeof(line), f))
   {
      char *eq = strchr(line, '='), *e;
      int i;
      if (line[0] == '#' || !eq) continue;
      *eq = 0;
      for (e = eq + 1; *e && *e != '\n' && *e != '\r'; e++) {}
      *e = 0;
      if ((i = module_index(line)) >= 0) s_mod[i].sw = atoi(eq + 1) ? 1 : 0;
   }
   fclose(f);
}

static void state_save(void)
{
   char tmp[320], dir[300];
   FILE *f;
   int i;
   snprintf(dir, sizeof(dir), "%s", s_state_path);
   if (strrchr(dir, '/')) *strrchr(dir, '/') = 0;
   mkdir(dir, 0755);
   snprintf(tmp, sizeof(tmp), "%s.tmp", s_state_path);
   if (!(f = fopen(tmp, "w"))) { logf_("modules: cannot write %s: %s", tmp, strerror(errno)); return; }
   fprintf(f, "# Lumen modules: 1 on, 0 off (Settings > Modules, docs/modules.md)\n");
   for (i = 0; i < s_n; i++) fprintf(f, "%s=%d\n", s_mod[i].m->id, s_mod[i].sw);
   fflush(f);
   fsync(fileno(f));
   fclose(f);
   if (rename(tmp, s_state_path)) { logf_("modules: cannot save %s: %s", s_state_path, strerror(errno)); return; }
   settings_fsync_dir(dir);
}

/* ------------------------------------------------------------------------ */
/* dependencies                                                              */

/* calls fn for each id in a space-separated list; stops when fn returns nonzero */
static int each_req(const char *list, int (*fn)(int idx, void *u), void *u)
{
   char id[32];
   const char *p = list;
   while (p && *p)
   {
      size_t n;
      while (*p == ' ') p++;
      n = strcspn(p, " ");
      if (n && n < sizeof(id))
      {
         int r;
         memcpy(id, p, n); id[n] = 0;
         if ((r = fn(module_index(id), u))) return r;
      }
      p += n;
   }
   return 0;
}

static int req_off(int idx, void *u) { if (idx < 0 || !s_mod[idx].sw) { *(int *)u = idx; return 1; } return 0; }
static int req_missing(int idx, void *u) { if (idx < 0) { *(int *)u = 1; return 1; } return 0; }

static int wanted_d(int i, int depth);
typedef struct { int b, depth; } req_ctx_t;
static int req_unwanted(int idx, void *u)
{
   req_ctx_t *c = u;
   if (idx < 0 || !wanted_d(idx, c->depth + 1)) { c->b = idx; return 1; }
   return 0;
}

/* the first module this one requires that cannot run now: switched off, or itself
 * unavailable or blocked (-1 none); -2 one is not installed */
static int blocker_d(int i, int depth)
{
   int missing = 0;
   req_ctx_t c = { -1, depth };
   each_req(s_mod[i].m->requires, req_missing, &missing);
   if (missing) return -2;
   return each_req(s_mod[i].m->requires, req_unwanted, &c) ? c.b : -1;
}
static int blocker(int i) { return blocker_d(i, 0); }

/* should module i run now: switched on (at boot, for MOD_F_RESTART), everything it
 * requires can run, and available() has no objection */
static int wanted_d(int i, int depth)
{
   const mod_t *d = &s_mod[i];
   int sw = d->m->flags & MOD_F_RESTART ? d->boot_sw : d->sw;
   if (depth > MOD_MAX || !sw || blocker_d(i, depth) != -1) return 0;
   return !d->m->available || !d->m->available();
}

static int requires_id(int i, const char *id)
{
   const char *p = s_mod[i].m->requires;
   size_t n = strlen(id);
   while (p && *p)
   {
      size_t k;
      while (*p == ' ') p++;
      k = strcspn(p, " ");
      if (k == n && !strncmp(p, id, n)) return 1;
      p += k;
   }
   return 0;
}

/* ------------------------------------------------------------------------ */
/* contributions: removed when their module stops                            */

typedef struct { int used; mod_status_item_t it; } status_slot_t;
static status_slot_t s_status[MOD_STATUS_MAX];
static mod_status_item_t s_status_sorted[MOD_STATUS_MAX];
static int s_nstatus_sorted = -1;   /* -1: sort again */

typedef struct { int used; mod_menu_t m; } menu_slot_t;
static menu_slot_t s_menu[MOD_MENU_MAX];

static void contributions_drop(const char *owner)
{
   int i;
   for (i = 0; i < MOD_STATUS_MAX; i++)
      if (s_status[i].used && !strcmp(s_status[i].it.owner, owner)) { s_status[i].used = 0; s_nstatus_sorted = -1; }
   for (i = 0; i < MOD_MENU_MAX; i++)
      if (s_menu[i].used && !strcmp(s_menu[i].m.owner, owner)) s_menu[i].used = 0;
}

/* ------------------------------------------------------------------------ */
/* lifecycle                                                                 */

static int wanted(int i) { return wanted_d(i, 0); }

static void start_one(int i)
{
   mod_t *d = &s_mod[i];
   if (d->running) return;
   d->running = 1;
   logf_("modules: start %s %s", d->m->id, d->m->version ? d->m->version : "");
   if (d->m->start) d->m->start();
}

static void stop_one(int i)
{
   mod_t *d = &s_mod[i];
   if (!d->running) return;
   logf_("modules: stop %s", d->m->id);
   if (d->m->stop) d->m->stop();
   d->running = 0;
   contributions_drop(d->m->id);
}

/* start what should run and stop what should not: dependencies first when starting,
 * dependants first when stopping (a module never runs without what it requires) */
static void reconcile(void)
{
   int i, pass;
   for (i = s_n - 1; i >= 0; i--) if (s_mod[i].running && !wanted(i)) stop_one(i);
   /* the registry lists a module after what it requires only by convention: repeat
    * until nothing changes (at most one pass per module) */
   for (pass = 0; pass < s_n; pass++)
   {
      int changed = 0;
      for (i = 0; i < s_n; i++)
      {
         int ok = 1, b = -1;
         if (s_mod[i].running || !wanted(i)) continue;
         /* everything it requires is running already */
         if (each_req(s_mod[i].m->requires, req_off, &b)) ok = 0;
         else
         {
            const char *p = s_mod[i].m->requires;
            while (p && *p && ok)
            {
               char id[32]; size_t k;
               while (*p == ' ') p++;
               k = strcspn(p, " ");
               if (k && k < sizeof(id)) { memcpy(id, p, k); id[k] = 0; ok = module_running(id); }
               p += k;
            }
         }
         if (ok) { start_one(i); changed = 1; }
      }
      if (!changed) break;
   }
}

void modules_init(const char *home)
{
   int i;
   s_n = 0;
   memset(s_status, 0, sizeof(s_status));
   memset(s_menu, 0, sizeof(s_menu));
   s_nstatus_sorted = -1;
   snprintf(s_state_path, sizeof(s_state_path), "%s/modules/state.ini", home);
   for (i = 0; i < (int)(sizeof(k_builtin) / sizeof(k_builtin[0])) && s_n < MOD_MAX; i++)
   {
      const lumen_module_t *m = k_builtin[i];
      mod_t *d;
      if (!id_ok(m->id) || m->api != LUMEN_MODULE_API || module_index(m->id) >= 0)
      { logf_("modules: built-in '%s' refused (id, api %d, or a duplicate)", m->id ? m->id : "?", m->api); continue; }
      d = &s_mod[s_n++];
      memset(d, 0, sizeof(*d));
      d->m = m;
      d->sw = m->flags & (MOD_F_DEFAULT_ON | MOD_F_REQUIRED) ? 1 : 0;
      snprintf(d->name_key, sizeof(d->name_key), "mod.%s.name", m->id);
      snprintf(d->desc_key, sizeof(d->desc_key), "mod.%s.description", m->id);
   }
   state_load();
   for (i = 0; i < s_n; i++)
   {
      if (s_mod[i].m->flags & MOD_F_REQUIRED) s_mod[i].sw = 1;
      s_mod[i].boot_sw = s_mod[i].sw;
   }
   reconcile();
   for (i = 0; i < s_n; i++)
      logf_("modules: %s %s", s_mod[i].m->id, s_mod[i].running ? "on" : s_mod[i].sw ? "on, not running" : "off");
   modules_emit(MEV_STARTED, NULL);
   s_next_check = now_s() + 1.0;
}

void modules_poll(double t)
{
   int i;
   if (t >= s_next_check) { reconcile(); s_next_check = t + 1.0; }   /* available() may change */
   for (i = 0; i < s_n; i++) if (s_mod[i].running && s_mod[i].m->poll) s_mod[i].m->poll(t);
}

void modules_emit(int ev, const void *arg)
{
   int i;
   for (i = 0; i < s_n; i++) if (s_mod[i].running && s_mod[i].m->event) s_mod[i].m->event(ev, arg);
}

int modules_count(void) { return s_n; }
const lumen_module_t *module_at(int i) { return i >= 0 && i < s_n ? s_mod[i].m : NULL; }
int module_switch(int i) { return i >= 0 && i < s_n ? s_mod[i].sw : 0; }
int module_running(const char *id) { int i = module_index(id); return i >= 0 && s_mod[i].running; }
const char *module_name_key(int i) { return i >= 0 && i < s_n ? s_mod[i].name_key : ""; }
const char *module_desc_key(int i) { return i >= 0 && i < s_n ? s_mod[i].desc_key : ""; }

int module_state(int i)
{
   const mod_t *d;
   if (i < 0 || i >= s_n) return MOD_OFF;
   d = &s_mod[i];
   if ((d->m->flags & MOD_F_RESTART) && d->sw != d->boot_sw) return MOD_RESTART;
   if (!d->sw) return MOD_OFF;
   if (blocker(i) != -1) return MOD_BLOCKED;
   if (d->m->available && d->m->available()) return MOD_UNAVAILABLE;
   return d->running ? MOD_ON : MOD_UNAVAILABLE;
}

const char *module_why(int i)
{
   const char *k;
   switch (module_state(i))
   {
   case MOD_BLOCKED: return blocker(i) == -2 ? "mods.missing_dep" : "mods.blocked";
   case MOD_UNAVAILABLE: return s_mod[i].m->available && (k = s_mod[i].m->available()) ? k : "mods.unavailable";
   default: return NULL;
   }
}

const char *module_blocker(int i)
{
   int b = i >= 0 && i < s_n ? blocker(i) : -1;
   return b >= 0 ? s_mod[b].m->id : NULL;
}

static int set_rec(int i, int on, int depth)
{
   int j, changed = 0;
   if (depth > MOD_MAX) return 0;
   if (!on && (s_mod[i].m->flags & MOD_F_REQUIRED)) return 0;
   if (s_mod[i].sw != on) { s_mod[i].sw = on; changed = depth > 0; }
   else if (depth > 0) return 0;
   if (on)
   {  /* on: what it requires too */
      const char *p = s_mod[i].m->requires;
      while (p && *p)
      {
         char id[32]; size_t k; int r;
         while (*p == ' ') p++;
         k = strcspn(p, " ");
         if (k && k < sizeof(id)) { memcpy(id, p, k); id[k] = 0; if ((r = module_index(id)) >= 0) changed += set_rec(r, 1, depth + 1); }
         p += k;
      }
   }
   else
   {  /* off: what requires it too */
      for (j = 0; j < s_n; j++) if (j != i && s_mod[j].sw && requires_id(j, s_mod[i].m->id)) changed += set_rec(j, 0, depth + 1);
   }
   return changed;
}

int module_set(int i, int on)
{
   int others;
   if (i < 0 || i >= s_n) return -1;
   if (!on && (s_mod[i].m->flags & MOD_F_REQUIRED)) return -1;
   others = set_rec(i, on ? 1 : 0, 0);
   logf_("modules: %s switched %s (%d more)", s_mod[i].m->id, on ? "on" : "off", others);
   state_save();
   reconcile();
   return others;
}

int modules_restart_pending(void)
{
   int i;
   for (i = 0; i < s_n; i++) if ((s_mod[i].m->flags & MOD_F_RESTART) && s_mod[i].sw != s_mod[i].boot_sw) return 1;
   return 0;
}

int module_key_ok(const char *owner, const char *key)
{
   size_t n;
   if (!owner || !key || strncmp(key, "mod.", 4)) return 0;
   n = strlen(owner);
   return !strncmp(key + 4, owner, n) && key[4 + n] == '.' && key[5 + n];
}

/* the module calling an extension point must be running */
static int owner_ok(const char *owner)
{
   if (module_running(owner)) return 1;
   logf_("modules: '%s' is not running: contribution refused", owner ? owner : "?");
   return 0;
}

/* ------------------------------------------------------------------------ */
/* extension point "status"                                                  */

int mod_status_set(const char *owner, const char *slot, const mod_status_t *s)
{
   int i, mine = 0, free_i = -1, at = -1;
   status_slot_t *st;
   if (!owner_ok(owner) || !slot || !*slot || !s || !s->glyph || !*s->glyph) return -1;
   for (i = 0; i < MOD_STATUS_MAX; i++)
   {
      if (!s_status[i].used) { if (free_i < 0) free_i = i; continue; }
      if (strcmp(s_status[i].it.owner, owner)) continue;
      mine++;
      if (!strcmp(s_status[i].it.slot, slot)) at = i;
   }
   if (at < 0)
   {
      if (mine >= MOD_STATUS_PER_MODULE || free_i < 0)
      { logf_("modules: %s: status item '%s' refused (limit)", owner, slot); return -1; }
      at = free_i;
   }
   st = &s_status[at];
   {  /* unchanged: nothing to sort again */
      mod_status_item_t n;
      memset(&n, 0, sizeof(n));
      snprintf(n.owner, sizeof(n.owner), "%s", owner);
      snprintf(n.slot, sizeof(n.slot), "%s", slot);
      snprintf(n.glyph, sizeof(n.glyph), "%s", s->glyph);
      snprintf(n.badge, sizeof(n.badge), "%s", s->badge ? s->badge : "");
      n.dim = s->dim ? 1 : 0;
      n.order = s->order;
      if (st->used && !memcmp(&st->it, &n, sizeof(n))) return 0;
      st->it = n;
      st->used = 1;
   }
   s_nstatus_sorted = -1;
   return 0;
}

void mod_status_clear(const char *owner, const char *slot)
{
   int i;
   if (!owner || !slot) return;
   for (i = 0; i < MOD_STATUS_MAX; i++)
      if (s_status[i].used && !strcmp(s_status[i].it.owner, owner) && !strcmp(s_status[i].it.slot, slot))
      { s_status[i].used = 0; s_nstatus_sorted = -1; }
}

static int cmp_status(const void *a, const void *b)
{
   const mod_status_item_t *x = a, *y = b;
   if (x->order != y->order) return x->order < y->order ? -1 : 1;
   return strcmp(x->owner, y->owner) ? strcmp(x->owner, y->owner) : strcmp(x->slot, y->slot);
}

int mod_status_count(void)
{
   if (s_nstatus_sorted < 0)
   {
      int i;
      s_nstatus_sorted = 0;
      for (i = 0; i < MOD_STATUS_MAX; i++) if (s_status[i].used) s_status_sorted[s_nstatus_sorted++] = s_status[i].it;
      qsort(s_status_sorted, s_nstatus_sorted, sizeof(s_status_sorted[0]), cmp_status);
   }
   return s_nstatus_sorted;
}

const mod_status_item_t *mod_status_item(int i)
{
   return i >= 0 && i < mod_status_count() ? &s_status_sorted[i] : NULL;
}

/* ------------------------------------------------------------------------ */
/* extension point "menu.game"                                               */

int mod_menu_add(const mod_menu_t *m)
{
   int i, mine = 0, free_i = -1;
   if (!m || !owner_ok(m->owner) || !m->id || !*m->id || !m->invoke) return -1;
   if (!module_key_ok(m->owner, m->label_key))
   { logf_("modules: %s: menu entry '%s' refused: label '%s' is not a mod.%s.* key", m->owner, m->id, m->label_key ? m->label_key : "", m->owner); return -1; }
   for (i = 0; i < MOD_MENU_MAX; i++)
   {
      if (!s_menu[i].used) { if (free_i < 0) free_i = i; continue; }
      if (strcmp(s_menu[i].m.owner, m->owner)) continue;
      if (!strcmp(s_menu[i].m.id, m->id)) { s_menu[i].m = *m; return 0; }   /* replaced */
      mine++;
   }
   if (mine >= MOD_MENU_PER_MODULE || free_i < 0) { logf_("modules: %s: menu entry '%s' refused (limit)", m->owner, m->id); return -1; }
   s_menu[free_i].used = 1;
   s_menu[free_i].m = *m;
   return 0;
}

void mod_menu_remove(const char *owner, const char *id)
{
   int i;
   if (!owner || !id) return;
   for (i = 0; i < MOD_MENU_MAX; i++)
      if (s_menu[i].used && !strcmp(s_menu[i].m.owner, owner) && !strcmp(s_menu[i].m.id, id)) s_menu[i].used = 0;
}

int mod_menu_list(int gi, int *out, int max)
{
   int i, j, n = 0;
   for (i = 0; i < MOD_MENU_MAX && n < max; i++)
   {
      const mod_menu_t *m = &s_menu[i].m;
      if (!s_menu[i].used || !module_running(m->owner)) continue;
      if (m->visible && !m->visible(gi, m->user)) continue;
      /* insertion by order, then owner, so the menu is stable */
      for (j = n; j > 0; j--)
      {
         const mod_menu_t *p = &s_menu[out[j - 1]].m;
         if (p->order < m->order || (p->order == m->order && strcmp(p->owner, m->owner) <= 0)) break;
         out[j] = out[j - 1];
      }
      out[j] = i;
      n++;
   }
   return n;
}

const char *mod_menu_label(int h)
{
   return h >= 0 && h < MOD_MENU_MAX && s_menu[h].used ? tr(s_menu[h].m.label_key) : "";
}

void mod_menu_invoke(int h, int gi)
{
   mod_menu_t m;
   if (h < 0 || h >= MOD_MENU_MAX || !s_menu[h].used || !module_running(s_menu[h].m.owner)) return;
   m = s_menu[h].m;   /* invoke may remove its own entry */
   logf_("modules: %s: menu '%s' on game %d", m.owner, m.id, gi);
   m.invoke(gi, m.user);
}
