/* Profiles (docs/settings-design.md section 7). Static storage, at most MAX_PROFILES.
 * Every profile.ini write is atomic (tmp + fsync + rename + sync). Deleting renames the
 * folder to profiles/.trash-<id>-<time> (instant) and a detached thread removes it. */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "lumen.h"
#include "profiles.h"
#include "settings.h"
#include "i18n.h"

static profile_t s_prof[MAX_PROFILES];
static int       s_n;
static char      s_dir[300];          /* <home>/profiles */

static const unsigned k_colors[PROFILE_COLORS] = {
   0x5B8CFF, 0xFF5B7A, 0x3DDC97, 0xFFB547, 0xB07CFF, 0x2EC5D3, 0xFF7F3F, 0xC9CBD6
};

void profiles_color(int idx, float rgb[3])
{
   unsigned c = k_colors[(idx >= 0 && idx < PROFILE_COLORS) ? idx : 0];
   rgb[0] = ((c >> 16) & 0xFF) / 255.0f;
   rgb[1] = ((c >> 8) & 0xFF) / 255.0f;
   rgb[2] = (c & 0xFF) / 255.0f;
}

static int valid_id(const char *s)
{
   const char *p;
   if (!*s || strlen(s) >= sizeof(s_prof[0].id)) return 0;
   for (p = s; *p; p++)
      if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_' || *p == '-')) return 0;
   return 1;
}

static char *strip(char *s)
{
   char *e;
   while (*s && isspace((unsigned char)*s)) s++;
   e = s + strlen(s);
   while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
   return s;
}

/* reads profile.ini into p (keeps p's values for missing keys); 0 if the file exists */
static int ini_read(const char *id, profile_t *p)
{
   char path[400], line[160];
   FILE *f;
   snprintf(path, sizeof(path), "%s/%s/profile.ini", s_dir, id);
   if (!(f = fopen(path, "r"))) return -1;
   while (fgets(line, sizeof(line), f))
   {
      char *k = line, *eq, *v;
      if (!strncmp(k, "\xEF\xBB\xBF", 3)) k += 3;
      k = strip(k);
      if (!*k || *k == '#' || !(eq = strchr(k, '='))) continue;
      *eq = 0; v = strip(eq + 1); k = strip(k);
      if (!strcmp(k, "name")) snprintf(p->name, sizeof(p->name), "%s", v);
      else if (!strcmp(k, "color")) { int c = atoi(v); p->color = (c >= 0 && c < PROFILE_COLORS) ? c : 0; }
      else if (!strcmp(k, "created")) p->created = atol(v);
   }
   fclose(f);
   return 0;
}

static int mkdir_p1(const char *path)
{
   return (mkdir(path, 0755) == 0 || errno == EEXIST) ? 0 : -1;
}

static int ini_write(const profile_t *p)
{
   char dir[340], path[400], tmp[410];
   FILE *f;
   int ok;
   snprintf(dir, sizeof(dir), "%s/%s", s_dir, p->id);
   if (mkdir_p1(s_dir) || mkdir_p1(dir)) { logf_("profiles: mkdir %s: %s", dir, strerror(errno)); return -3; }
   snprintf(path, sizeof(path), "%s/profile.ini", dir);
   snprintf(tmp, sizeof(tmp), "%s.tmp", path);
   if (!(f = fopen(tmp, "w"))) { logf_("profiles: cannot write %s: %s", tmp, strerror(errno)); return -3; }
   if (!strcmp(p->id, "main")) ok = fprintf(f, "name=%s\ncolor=%d\n", p->name, p->color) > 0;
   else ok = fprintf(f, "name=%s\ncolor=%d\ncreated=%ld\n", p->name, p->color, p->created) > 0;
   if (ok) ok = fflush(f) == 0;
   if (ok) ok = fsync(fileno(f)) == 0;
   if (fclose(f) != 0) ok = 0;
   if (ok) ok = rename(tmp, path) == 0;
   if (!ok) { logf_("profiles: write %s failed: %s", path, strerror(errno)); unlink(tmp); return -3; }
   sync();
   return 0;
}

static int cmp_created(const void *a, const void *b)
{
   const profile_t *x = a, *y = b;
   if (x->created != y->created) return x->created < y->created ? -1 : 1;
   return strcmp(x->id, y->id);
}

int profiles_load(const char *home)
{
   DIR *d;
   struct dirent *e;
   snprintf(s_dir, sizeof(s_dir), "%s/profiles", home);
   memset(s_prof, 0, sizeof(s_prof));
   s_n = 1;
   snprintf(s_prof[0].id, sizeof(s_prof[0].id), "main");
   ini_read("main", &s_prof[0]);
   s_prof[0].created = 0;
   if ((d = opendir(s_dir)))
   {
      while ((e = readdir(d)))
      {
         char path[600];
         struct stat st;
         profile_t p;
         if (e->d_name[0] == '.' || !strcmp(e->d_name, "main") || !valid_id(e->d_name)) continue;
         snprintf(path, sizeof(path), "%s/%s", s_dir, e->d_name);
         if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
         if (s_n >= MAX_PROFILES) { logf_("profiles: more than %d, ignoring %s", MAX_PROFILES, e->d_name); continue; }
         memset(&p, 0, sizeof(p));
         snprintf(p.id, sizeof(p.id), "%s", e->d_name);
         if (ini_read(p.id, &p) != 0) p.created = (long)st.st_mtime;
         if (!p.name[0]) snprintf(p.name, sizeof(p.name), "%s", p.id);
         s_prof[s_n++] = p;
      }
      closedir(d);
   }
   if (s_n > 2) qsort(&s_prof[1], s_n - 1, sizeof(profile_t), cmp_created);
   logf_("profiles: %d (active %s)", s_n, profiles_active()->id);
   return s_n;
}

int profiles_count(void) { return s_n; }

const profile_t *profiles_get(int i) { return (i >= 0 && i < s_n) ? &s_prof[i] : NULL; }

int profiles_find(const char *id)
{
   int i;
   if (!id) return -1;
   for (i = 0; i < s_n; i++) if (!strcmp(s_prof[i].id, id)) return i;
   return -1;
}

const profile_t *profiles_active(void)
{
   int i = profiles_find(settings_get("profile", "main"));
   return &s_prof[i >= 0 ? i : 0];
}

const char *profiles_display_name(const profile_t *p)
{
   if (!p) return "";
   if (p->name[0]) return p->name;
   return !strcmp(p->id, "main") ? tr("profile.main") : p->id;
}

/* case-insensitive for ASCII and the Latin-1 letters of UTF-8 (C3 80..9E -> +0x20) */
static int name_eq(const char *a, const char *b)
{
   for (;;)
   {
      unsigned char x = (unsigned char)*a, y = (unsigned char)*b;
      if (x == 0xC3 && y == 0xC3 && a[1] && b[1])
      {
         unsigned char u = (unsigned char)a[1], v = (unsigned char)b[1];
         if (u >= 0x80 && u <= 0x9E && u != 0x97) u += 0x20;
         if (v >= 0x80 && v <= 0x9E && v != 0x97) v += 0x20;
         if (u != v) return 0;
         a += 2; b += 2; continue;
      }
      if (tolower(x) != tolower(y)) return 0;
      if (!x) return 1;
      a++; b++;
   }
}

int profiles_name_taken(const char *name, const char *except_id)
{
   int i;
   for (i = 0; i < s_n; i++)
   {
      if (except_id && !strcmp(s_prof[i].id, except_id)) continue;
      if (name_eq(profiles_display_name(&s_prof[i]), name)) return 1;
   }
   return 0;
}

int profiles_set_active(const char *id)
{
   if (profiles_find(id) < 0) return -1;
   settings_set("profile", id);
   if (settings_dirty() && settings_save() != 0) return -3;
   logf_("profiles: active %s", id);
   return 0;
}

static int clean_name(const char *in, char *out, size_t n)
{
   size_t len;
   while (*in == ' ') in++;
   snprintf(out, n, "%s", in);
   len = strlen(out);
   while (len > 0 && out[len - 1] == ' ') out[--len] = 0;
   return len > 0 && !strpbrk(out, "\n\r");
}

int profiles_create(const char *name, int color, char *id_out, size_t n)
{
   profile_t p;
   char dir[340], sub[360];
   int k, i, used;
   if (s_n >= MAX_PROFILES) return -1;
   memset(&p, 0, sizeof(p));
   if (!clean_name(name, p.name, sizeof(p.name))) return -3;
   if (profiles_name_taken(p.name, NULL)) return -2;
   /* lowest free id p1..p9 (not a loaded profile and no folder on disk) */
   for (k = 1; k <= 9; k++)
   {
      struct stat st;
      snprintf(p.id, sizeof(p.id), "p%d", k);
      snprintf(dir, sizeof(dir), "%s/%s", s_dir, p.id);
      if (profiles_find(p.id) < 0 && stat(dir, &st) != 0) break;
   }
   if (k > 9) return -1;
   if (color < 0 || color >= PROFILE_COLORS)
   {
      for (color = 0; color < PROFILE_COLORS; color++)
      {
         for (used = 0, i = 0; i < s_n; i++) if (s_prof[i].color == color) used = 1;
         if (!used) break;
      }
      if (color >= PROFILE_COLORS) color = s_n % PROFILE_COLORS;
   }
   p.color = color;
   p.created = (long)time(NULL);
   if (ini_write(&p) != 0) return -3;
   snprintf(sub, sizeof(sub), "%s/saves", dir);
   if (mkdir_p1(sub)) { logf_("profiles: mkdir %s: %s", sub, strerror(errno)); return -3; }
   snprintf(sub, sizeof(sub), "%s/states", dir);
   if (mkdir_p1(sub)) { logf_("profiles: mkdir %s: %s", sub, strerror(errno)); return -3; }
   sync();
   s_prof[s_n++] = p;
   if (id_out) snprintf(id_out, n, "%s", p.id);
   logf_("profiles: created %s", p.id);
   return 0;
}

int profiles_rename(const char *id, const char *name)
{
   int i = profiles_find(id);
   profile_t p;
   if (i < 0) return -3;
   p = s_prof[i];
   if (!clean_name(name, p.name, sizeof(p.name))) return -3;
   if (profiles_name_taken(p.name, id)) return -2;
   if (ini_write(&p) != 0) return -3;
   s_prof[i] = p;
   logf_("profiles: renamed %s", id);
   return 0;
}

int profiles_set_color(const char *id, int color)
{
   int i = profiles_find(id);
   profile_t p;
   if (i < 0 || color < 0 || color >= PROFILE_COLORS) return -3;
   if (s_prof[i].color == color) return 0;
   p = s_prof[i];
   p.color = color;
   if (ini_write(&p) != 0) return -3;
   s_prof[i] = p;
   return 0;
}

static int rm_entry(const char *path, const struct stat *st, int flag, struct FTW *ftw)
{
   (void)st; (void)flag; (void)ftw;
   if (remove(path) != 0) logf_("profiles: remove %s: %s", path, strerror(errno));
   return 0;
}

static int trash_thread(void *arg)
{
   char *path = arg;
   nftw(path, rm_entry, 8, FTW_DEPTH | FTW_PHYS);
   sync();
   logf_("profiles: removed %s", path);
   free(path);
   return 0;
}

int profiles_delete(const char *id)
{
   char dir[340], trash[400];
   struct stat st;
   int i = profiles_find(id), was_active;
   if (id && !strcmp(id, "main")) return -1;
   if (i < 0) return -3;
   snprintf(dir, sizeof(dir), "%s/%s", s_dir, id);
   if (stat(dir, &st) == 0)
   {
      SDL_Thread *th;
      char *copy;
      snprintf(trash, sizeof(trash), "%s/.trash-%s-%ld", s_dir, id, (long)time(NULL));
      if (rename(dir, trash) != 0) { logf_("profiles: cannot move %s: %s", dir, strerror(errno)); return -3; }
      sync();
      if ((copy = strdup(trash)) && (th = SDL_CreateThread(trash_thread, "trash", copy))) SDL_DetachThread(th);
      else { free(copy); logf_("profiles: %s left for the next session", trash); }
   }
   was_active = !strcmp(profiles_active()->id, id);
   memmove(&s_prof[i], &s_prof[i + 1], (size_t)(s_n - i - 1) * sizeof(profile_t));
   s_n--;
   logf_("profiles: deleted %s", id);
   if (was_active) return profiles_set_active("main") == 0 ? 0 : -3;
   return 0;
}
