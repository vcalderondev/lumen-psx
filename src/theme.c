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

typedef struct { char id[32], name[48]; float card[3], lit[3]; } theme_entry_t;   /* card, lit: the swatch of the settings */

static theme_entry_t s_list[THEME_MAX];
static int   s_n, s_active;
static char  s_home[256];
static float s_col[TC_COUNT][3];
static float s_card_alpha = 1.0f;    /* Lumen's own look: opaque black menus */
static float s_lit[3];
static int   s_has_lit;
static float s_radius = 1.0f;     /* corner rounding of every shape: 1 = Lumen's, 0 = square */
static float s_val[TV_COUNT];
static const char *const k_vkeys[TV_COUNT] = { "scrim", "scrim_settings", "selected", "card_light" };
/* Lumen's own look: menus on flat black (the dashboard just showing behind Settings), a quiet
 * focus fill, flat cards */
static const float k_vdefault[TV_COUNT] = { 1.0f, 0.90f, 0.16f, 0.0f };

static const char *const k_keys[TC_COUNT] = { "accent", "card", "text", "dim", "backdrop", "panel", "danger",
                                              "focus", "glass", "primary", "on_primary" };
/* Lumen's own look (lumen-design tokens) */
static const float k_default[TC_COUNT][3] = {
   { 0.36f, 0.60f, 1.00f },   /* accent   */
   { 0.016f, 0.024f, 0.031f },/* card: the welcome's black (= backdrop) */
   { 0.96f, 0.96f, 0.98f },   /* text     */
   { 0.62f, 0.63f, 0.70f },   /* dim      */
   { 0.016f, 0.024f, 0.031f },/* backdrop */
   { 0.043f, 0.047f, 0.062f },/* panel    */
   { 1.00f, 0.42f, 0.42f },   /* danger   */
   { 1.00f, 1.00f, 1.00f },   /* focus    */
   { 1.00f, 1.00f, 1.00f },   /* glass    */
   { 1.00f, 1.00f, 1.00f },   /* primary  */
   { 0.043f, 0.047f, 0.063f },/* on_primary */
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

/* the two colours a theme is recognised by in the settings: its card and its light (lit,
 * else its accent); Lumen's own for the keys it leaves out */
static void read_preview(theme_entry_t *e)
{
   char path[400], line[256];
   int have_lit = 0;
   FILE *f;
   memcpy(e->card, k_default[TC_CARD], sizeof(e->card));
   memcpy(e->lit, k_default[TC_ACCENT], sizeof(e->lit));
   snprintf(path, sizeof(path), "%s/themes/%s/theme.ini", s_home, e->id);
   if (!(f = fopen(path, "r"))) return;
   while (fgets(line, sizeof(line), f))
   {
      char *k = strip(line), *eq, *v;
      if (*k == '#' || *k == ';' || !(eq = strchr(k, '='))) continue;
      *eq = 0; v = strip(eq + 1); k = strip(k);
      if (!strcmp(k, "card")) parse_rgb(v, e->card);
      else if (!strcmp(k, "lit")) have_lit = parse_rgb(v, e->lit);
      else if (!strcmp(k, "accent") && !have_lit) parse_rgb(v, e->lit);
   }
   fclose(f);
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
      if (!strcmp(k, "lit")) { if (parse_rgb(v, s_lit)) s_has_lit = 1; else logf_("theme %s: bad colour lit = %s", id, v); continue; }
      if (!strcmp(k, "radius")) { float r = (float)atof(v); s_radius = r < 0.0f ? 0.0f : r > 1.0f ? 1.0f : r; continue; }
      for (i = 0; i < TV_COUNT; i++)
         if (!strcmp(k, k_vkeys[i])) { float a = (float)atof(v); s_val[i] = a < 0.0f ? 0.0f : a > 1.0f ? 1.0f : a; break; }
      if (i < TV_COUNT) continue;
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
   for (i = 0; i < s_n; i++) read_preview(&s_list[i]);
   memcpy(s_col, k_default, sizeof(s_col));
   s_card_alpha = 1.0f;
   memcpy(s_val, k_vdefault, sizeof(s_val));
   s_radius = 1.0f;
   s_has_lit = 0;
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
float       theme_value(int which) { return which >= 0 && which < TV_COUNT ? s_val[which] : 0.0f; }
void        theme_preview(int i, float card[3], float lit[3])
{
   if (i < 0 || i >= s_n) i = 0;
   memcpy(card, s_list[i].card, sizeof(s_list[0].card)); memcpy(lit, s_list[i].lit, sizeof(s_list[0].lit));
}
int         theme_lit(float rgb[3]) { if (s_has_lit) memcpy(rgb, s_lit, sizeof(s_lit)); return s_has_lit; }

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
