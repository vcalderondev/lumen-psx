/* Settings store: <home>/settings.ini (docs/settings-design.md section 4).
 * Static storage only; every pair read is kept and written back, so unknown keys survive. */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "lumen.h"
#include "settings.h"

typedef struct { char key[48]; char val[96]; } pair_t;

static pair_t s_pairs[SETTINGS_MAX];
static int    s_n;
static int    s_dirty;
static char   s_path[300];

/* defaults written to a new file (graphics.<id> keys are added on first change) */
static const char *const s_defaults[][2] = {
   { "version", "1" },
   { "language", "es" },
   { "profile", "main" },
   { "screensaver_minutes", "5" },
   { "demo_enabled", "1" },
   { "demo_seconds", "60" },
   { "attract_demo_delay", "20" },
};
#define NDEFAULTS ((int)(sizeof(s_defaults) / sizeof(s_defaults[0])))

static char *strip(char *s)
{
   char *e;
   while (*s && isspace((unsigned char)*s)) s++;
   e = s + strlen(s);
   while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
   return s;
}

static pair_t *find(const char *key)
{
   int i;
   for (i = 0; i < s_n; i++) if (!strcmp(s_pairs[i].key, key)) return &s_pairs[i];
   return NULL;
}

/* store without touching the dirty flag; returns the pair or NULL when full / invalid */
static pair_t *put(const char *key, const char *val)
{
   pair_t *p = find(key);
   if (!key[0] || strlen(key) >= sizeof(p->key)) { logf_("settings: key too long or empty: %s", key); return NULL; }
   if (!p)
   {
      if (s_n >= SETTINGS_MAX) { logf_("settings: full, dropping %s", key); return NULL; }
      p = &s_pairs[s_n++];
      snprintf(p->key, sizeof(p->key), "%s", key);
   }
   if (strlen(val) >= sizeof(p->val)) logf_("settings: value of %s truncated", key);
   snprintf(p->val, sizeof(p->val), "%s", val);
   return p;
}

int settings_load(const char *home)
{
   char line[256];
   FILE *f;
   int i, rc = 0;
   s_n = 0; s_dirty = 0;
   snprintf(s_path, sizeof(s_path), "%s/settings.ini", home);
   if ((f = fopen(s_path, "r")))
   {
      while (fgets(line, sizeof(line), f))
      {
         char *k = line, *eq;
         if (!strncmp(k, "\xEF\xBB\xBF", 3)) k += 3;
         k = strip(k);
         if (!*k || *k == '#' || !(eq = strchr(k, '='))) continue;
         *eq = 0;
         put(strip(k), strip(eq + 1));
      }
      fclose(f);
      logf_("settings: %d keys from %s", s_n, s_path);
   }
   else
   {
      rc = -1;
      logf_("settings: %s missing, using defaults", s_path);
   }
   /* make sure the file gets every documented key on the next save */
   for (i = 0; i < NDEFAULTS; i++)
      if (!find(s_defaults[i][0]) && put(s_defaults[i][0], s_defaults[i][1])) s_dirty = 1;
   return rc;
}

const char *settings_get(const char *key, const char *def)
{
   const pair_t *p = find(key);
   return p ? p->val : def;
}

int settings_get_int(const char *key, int def, int lo, int hi)
{
   const pair_t *p = find(key);
   char *end; long v;
   if (!p || !p->val[0]) return def;
   errno = 0;
   v = strtol(p->val, &end, 10);
   if (errno || *end) return def;
   if (v < lo) v = lo;
   if (v > hi) v = hi;
   return (int)v;
}

void settings_set(const char *key, const char *value)
{
   const pair_t *p = find(key);
   if (!value) value = "";
   if (p && !strcmp(p->val, value)) return;
   if (strchr(value, '\n') || strchr(value, '\r')) { logf_("settings: rejected multi-line value for %s", key); return; }
   if (put(key, value)) s_dirty = 1;
}

void settings_set_int(const char *key, int value)
{
   char v[16];
   snprintf(v, sizeof(v), "%d", value);
   settings_set(key, v);
}

int settings_dirty(void) { return s_dirty; }

/* make a rename / mkdir inside dir durable without a global sync() (which would flush
 * every dirty page of every mounted filesystem on the render thread) */
int settings_fsync_dir(const char *dir)
{
   int fd = open(dir, O_RDONLY | O_DIRECTORY), rc;
   if (fd < 0) return -1;
   rc = fsync(fd);
   close(fd);
   return rc;
}

/* atomic: tmp file -> fsync -> rename over settings.ini -> fsync of the folder */
int settings_save(void)
{
   char tmp[320];
   FILE *f;
   int i, ok;
   if (!s_path[0]) return -1;
   snprintf(tmp, sizeof(tmp), "%s.tmp", s_path);
   if (!(f = fopen(tmp, "w"))) { logf_("settings: cannot write %s: %s", tmp, strerror(errno)); return -1; }
   ok = fprintf(f, "# Lumen settings\n") > 0;
   for (i = 0; i < s_n && ok; i++) ok = fprintf(f, "%s=%s\n", s_pairs[i].key, s_pairs[i].val) > 0;
   if (ok) ok = fflush(f) == 0;
   if (ok) ok = fsync(fileno(f)) == 0;
   if (fclose(f) != 0) ok = 0;
   if (ok) ok = rename(tmp, s_path) == 0;
   if (!ok)
   {
      logf_("settings: save failed (%s), keeping the old file", strerror(errno));
      unlink(tmp);
      return -1;
   }
   {
      char dir[300];
      char *slash;
      snprintf(dir, sizeof(dir), "%s", s_path);
      if ((slash = strrchr(dir, '/'))) { *slash = 0; settings_fsync_dir(dir[0] ? dir : "/"); }
   }
   s_dirty = 0;
   logf_("settings: saved %d keys", s_n);
   return 0;
}
