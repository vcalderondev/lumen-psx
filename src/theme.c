/* Themes, see theme.h and docs/themes.md. */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "lumen.h"
#include "theme.h"
#include "settings.h"

typedef struct { char id[32], name[48]; } theme_entry_t;

static theme_entry_t s_list[THEME_MAX];
static int   s_n, s_active;
static char  s_home[256];
static float s_col[TC_COUNT][3];
static float s_card_alpha = 0.92f;
static float s_radius = 1.0f;     /* corner rounding of every shape: 1 = Lumen's, 0 = square */

static const char *const k_keys[TC_COUNT] = { "accent", "pill", "card", "text", "dim", "backdrop", "panel", "danger" };
/* Lumen's own look: the colours the frontend used before themes existed */
static const float k_default[TC_COUNT][3] = {
   { 0.36f, 0.60f, 1.00f },   /* accent   */
   { 0.23f, 0.51f, 0.96f },   /* pill     */
   { 0.10f, 0.12f, 0.16f },   /* card     */
   { 0.96f, 0.96f, 0.98f },   /* text     */
   { 0.70f, 0.73f, 0.80f },   /* dim      */
   { 0.016f, 0.024f, 0.031f },/* backdrop */
   { 0.043f, 0.047f, 0.062f },/* panel    */
   { 1.00f, 0.55f, 0.55f },   /* danger   */
};

static int valid_id(const char *s)
{
   if (!*s || strlen(s) >= sizeof(s_list[0].id)) return 0;
   for (; *s; s++) if (!(islower((unsigned char)*s) || isdigit((unsigned char)*s) || *s == '-' || *s == '_')) return 0;
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

/* "#RRGGBB" or "r, g, b" (0..255 or 0..1 floats) */
static int parse_rgb(const char *v, float out[3])
{
   unsigned x;
   float a, b, c;
   if (v[0] == '#' && strlen(v) >= 7 && sscanf(v + 1, "%6x", &x) == 1)
   {
      out[0] = ((x >> 16) & 0xFF) / 255.0f; out[1] = ((x >> 8) & 0xFF) / 255.0f; out[2] = (x & 0xFF) / 255.0f;
      return 1;
   }
   if (sscanf(v, "%f , %f , %f", &a, &b, &c) == 3)
   {
      float k = (a > 1.0f || b > 1.0f || c > 1.0f) ? 1.0f / 255.0f : 1.0f;
      out[0] = a * k; out[1] = b * k; out[2] = c * k;
      return 1;
   }
   return 0;
}

static void read_ini(const char *id, char *name, size_t nn, int apply)
{
   char path[400], line[256];
   FILE *f;
   snprintf(path, sizeof(path), "%s/themes/%s/theme.ini", s_home, id);
   if (!(f = fopen(path, "r"))) return;
   while (fgets(line, sizeof(line), f))
   {
      char *k = strip(line), *eq, *v;
      int i;
      if (*k == '#' || *k == ';' || *k == '[' || !(eq = strchr(k, '='))) continue;
      *eq = 0; v = strip(eq + 1); k = strip(k);
      if (!strcmp(k, "name") && name) snprintf(name, nn, "%s", v);
      if (!apply) continue;
      if (!strcmp(k, "card_alpha")) { float a = (float)atof(v); if (a > 0.2f && a <= 1.0f) s_card_alpha = a; continue; }
      if (!strcmp(k, "radius")) { float r = (float)atof(v); s_radius = r < 0.0f ? 0.0f : r > 1.0f ? 1.0f : r; continue; }
      for (i = 0; i < TC_COUNT; i++)
         if (!strcmp(k, k_keys[i]) && !parse_rgb(v, s_col[i])) logf_("theme %s: bad colour %s = %s", id, k, v);
   }
   fclose(f);
}

static int cmp_entry(const void *a, const void *b)
{
   const theme_entry_t *x = a, *y = b;
   if (!strcmp(x->id, "default")) return -1;
   if (!strcmp(y->id, "default")) return 1;
   return strcasecmp(x->name, y->name);
}

void theme_init(const char *home)
{
   char dir[300];
   const char *want;
   DIR *d;
   struct dirent *de;
   int i;
   snprintf(s_home, sizeof(s_home), "%s", home);
   s_n = 0;
   snprintf(s_list[s_n].id, sizeof(s_list[0].id), "default");
   snprintf(s_list[s_n].name, sizeof(s_list[0].name), "LPSX");
   s_n++;
   snprintf(dir, sizeof(dir), "%s/themes", home);
   if ((d = opendir(dir)))
   {
      while ((de = readdir(d)) && s_n < THEME_MAX)
      {
         char ini[600];
         struct stat st;
         if (de->d_name[0] == '.' || !valid_id(de->d_name) || !strcmp(de->d_name, "default")) continue;
         snprintf(ini, sizeof(ini), "%s/%s/theme.ini", dir, de->d_name);
         if (stat(ini, &st) != 0) continue;
         snprintf(s_list[s_n].id, sizeof(s_list[0].id), "%s", de->d_name);
         snprintf(s_list[s_n].name, sizeof(s_list[0].name), "%s", de->d_name);
         read_ini(de->d_name, s_list[s_n].name, sizeof(s_list[0].name), 0);
         s_n++;
      }
      closedir(d);
   }
   qsort(s_list, s_n, sizeof(s_list[0]), cmp_entry);
   memcpy(s_col, k_default, sizeof(s_col));
   s_card_alpha = 0.92f;
   s_radius = 1.0f;
   want = settings_get("theme", "default");
   s_active = 0;
   for (i = 0; i < s_n; i++) if (!strcmp(s_list[i].id, want)) s_active = i;
   if (s_active > 0) read_ini(s_list[s_active].id, NULL, 0, 1);
   else if (strcmp(want, "default")) logf_("theme %s not found, using the default", want);
   logf_("theme: %s (%d available)", s_list[s_active].id, s_n);
}

int         theme_count(void) { return s_n; }
const char *theme_id(int i) { return i >= 0 && i < s_n ? s_list[i].id : "default"; }
const char *theme_name(int i) { return i >= 0 && i < s_n ? s_list[i].name : "LPSX"; }
int         theme_active_index(void) { return s_active; }
const char *theme_active_id(void) { return s_list[s_active].id; }
float       theme_card_alpha(void) { return s_card_alpha; }
float       theme_radius(void) { return s_radius; }

void theme_color(int which, float rgb[3])
{
   if (which < 0 || which >= TC_COUNT) which = TC_TEXT;
   memcpy(rgb, s_col[which], sizeof(float) * 3);
}

int theme_asset(char *out, size_t n, const char *rel)
{
   char p[600];
   struct stat st;
   if (s_active <= 0 || !rel || strstr(rel, "..")) return 0;
   snprintf(p, sizeof(p), "%s/themes/%s/%s", s_home, s_list[s_active].id, rel);
   if (stat(p, &st) != 0) return 0;
   snprintf(out, n, "%s", p);
   return 1;
}
