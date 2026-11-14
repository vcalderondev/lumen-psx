/*
 * Lumen — 3D game frontend for the PlayStation Classic.
 * SDL2 2.0.4 + OpenGL ES 3.0 on Weston/Wayland, Cortex-A35, 1 GB RAM.
 *
 * Prototype scope:
 *   - one library per declared system under /media/games/<system>/ (see docs/systems.md)
 *   - 3D jewel-case carousel with spring physics, idle float, lighting,
 *     floor reflections, ambient colour from the selected cover
 *   - platform pills (only platforms that have games)
 *   - "play": the case flips to show its back, zooms, fades to black, then the
 *     frontend exits with code 10 and writes the launch request to
 *     /tmp/lumen-launch (launch request v2, src/launch.c) for scripts/run.sh
 *   - Select+Start (or Escape) exits with code 0 (back to the Sony menu)
 *   - Triangle opens the settings panel (src/panel.c)
 *   - after screensaver_minutes idle: attract scene (src/attract.c); a demo exits
 *     with code 11 (mode=demo launch request)
 *
 * Usage: lumen [--root /media] [--home /media/lumen] [--log file]
 *              [--attract]       start in the attract scene (after a demo ended by itself)
 *              [--focus-demo]    select the game of /tmp/lumen-demo-game (a demo was interrupted)
 */
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>
#include <ctype.h>
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include "lumen.h"
#include "i18n.h"
#include "settings.h"
#include "panel.h"
#include "profiles.h"
#include "launch.h"
#include "attract.h"
#include "version.h"

#define TEX_WINDOW 6       /* covers kept on the GPU around the selection */
#define VISIBLE 4          /* boxes drawn on each side */
#define BG_OPACITY 0.30f   /* per-game background image opacity */

/* ------------------------------------------------------------------------ */
/* logging                                                                   */
static FILE *g_log;
void logf_(const char *fmt, ...)
{
   va_list ap;
   if (!g_log) g_log = stderr;
   va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap);
   fputc('\n', g_log); fflush(g_log);
}

double now_s(void) { return SDL_GetPerformanceCounter() / (double)SDL_GetPerformanceFrequency(); }

static long rss_kb(void)
{
   char line[256]; long kb = -1;
   FILE *f = fopen("/proc/self/status", "r");
   if (!f) return -1;
   while (fgets(line, sizeof(line), f))
      if (!strncmp(line, "VmRSS:", 6)) { kb = strtol(line + 6, NULL, 10); break; }
   fclose(f);
   return kb;
}

/* ------------------------------------------------------------------------ */
/* data (types in lumen.h)                                                   */
game_t     g_games[MAX_GAMES];
int        g_ngames;
platform_t g_plat[MAX_PLATFORMS];
int        g_nplat;

char g_root[256]     = "/media";
char g_home[256]     = "/media/lumen";
char g_core_dir[256] = "/media/lumen/cores";
static char g_state[256]    = "/media/lumen/state.txt";

static void trim(char *s)
{
   char *e;
   while (*s && isspace((unsigned char)*s)) memmove(s, s + 1, strlen(s));
   e = s + strlen(s);
   while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
}

static int file_exists(const char *p) { struct stat st; return stat(p, &st) == 0; }

/* Game.ini style: "\nKey=value" (no sections) */
static void ini_get(const char *ini, const char *key, char *out, size_t n)
{
   char pat[64]; const char *p, *e; size_t len;
   out[0] = 0;
   snprintf(pat, sizeof(pat), "\n%s=", key);
   p = strstr(ini, pat);
   if (!p) return;
   p += strlen(pat);
   e = strpbrk(p, "\r\n");
   len = e ? (size_t)(e - p) : strlen(p);
   if (len >= n) len = n - 1;
   memcpy(out, p, len); out[len] = 0;
   trim(out);
}

/* Declaration ini: [section] key = value ; comment */
static void decl_get(const char *ini, const char *section, const char *key, char *out, size_t n, const char *def)
{
   char line[600], cur[48] = "";
   const char *p = ini;
   snprintf(out, n, "%s", def ? def : "");
   while (*p)
   {
      const char *e = strchr(p, '\n');
      size_t len = e ? (size_t)(e - p) : strlen(p);
      char *eq, *sc;
      if (len >= sizeof(line)) len = sizeof(line) - 1;
      memcpy(line, p, len); line[len] = 0;
      p = e ? e + 1 : p + len;
      if ((sc = strchr(line, ';'))) *sc = 0;
      trim(line);
      if (line[0] == '[') { char *rb = strchr(line, ']'); if (rb) *rb = 0; snprintf(cur, sizeof(cur), "%s", line + 1); continue; }
      if (!(eq = strchr(line, '='))) continue;
      *eq = 0; trim(line); trim(eq + 1);
      if (!strcasecmp(cur, section) && !strcasecmp(line, key)) { snprintf(out, n, "%s", eq + 1); return; }
   }
}

static void parse_color(const char *s, float c[3])
{
   unsigned v;
   if (s[0] == '#' && sscanf(s + 1, "%x", &v) == 1)
   { c[0] = ((v >> 16) & 255) / 255.0f; c[1] = ((v >> 8) & 255) / 255.0f; c[2] = (v & 255) / 255.0f; }
}

static void platform_from_decl(platform_t *pl, const char *ini)
{
   char v[300];
   memset(pl, 0, sizeof(*pl));
   decl_get(ini, "system", "id", pl->id, sizeof(pl->id), "psx");
   decl_get(ini, "system", "name", pl->name, sizeof(pl->name), pl->id);
   decl_get(ini, "system", "logo", v, sizeof(v), "");
   if (v[0]) snprintf(pl->logo, sizeof(pl->logo), "%s/systems/%s/%s", g_home, pl->id, v);
   decl_get(ini, "system", "order", v, sizeof(v), "50");   pl->order = atoi(v);
   decl_get(ini, "system", "show_name", v, sizeof(v), "1"); pl->show_name = atoi(v);
   decl_get(ini, "library", "layout", pl->layout, sizeof(pl->layout), "files");
   decl_get(ini, "library", "paths", pl->paths, sizeof(pl->paths), "");
   if (!pl->paths[0]) snprintf(pl->paths, sizeof(pl->paths), "%s/games/%s", g_root, pl->id);  /* convention */
   decl_get(ini, "library", "extensions", pl->exts, sizeof(pl->exts), "");
   decl_get(ini, "launch", "core", pl->core, sizeof(pl->core), "");
   decl_get(ini, "media", "kind", v, sizeof(v), "disc");
   pl->kind = !strcmp(v, "cartridge") || !strcmp(v, "card") ? KIND_CART : (!strcmp(v, "arcade") ? KIND_ARCADE : KIND_DISC);
   decl_get(ini, "media", "aspect", v, sizeof(v), pl->kind == KIND_DISC ? "1.136" : "1.0");
   pl->aspect = (float)atof(v); if (pl->aspect <= 0.1f) pl->aspect = 1.0f;
   if (pl->aspect > 1.2f) { pl->w = 1.25f; pl->h = 1.25f / pl->aspect; } else { pl->h = 1.0f; pl->w = pl->aspect; }
   pl->color[0] = 0.08f; pl->color[1] = 0.07f; pl->color[2] = 0.09f;
   decl_get(ini, "media", "color", v, sizeof(v), ""); parse_color(v, pl->color);
   decl_get(ini, "media", "art", pl->art, sizeof(pl->art), "cover");
   decl_get(ini, "media", "media_art", pl->media_art, sizeof(pl->media_art), pl->kind == KIND_DISC ? "disc" : "cart");
   decl_get(ini, "animation", "launch", v, sizeof(v), "");
   pl->launch = !strcmp(v, "insert_cartridge") ? ANIM_INSERT_CART : (!strcmp(v, "insert_coin") ? ANIM_INSERT_COIN :
                (!strcmp(v, "open_case_disc") ? ANIM_OPEN_CASE_DISC :
                 (pl->kind == KIND_CART ? ANIM_INSERT_CART : (pl->kind == KIND_ARCADE ? ANIM_INSERT_COIN : ANIM_OPEN_CASE_DISC))));
   decl_get(ini, "animation", "idle", v, sizeof(v), "float"); pl->idle_float = strcmp(v, "none") != 0;
   decl_get(ini, "animation", "sound", v, sizeof(v), "");
   if (v[0]) snprintf(pl->sound, sizeof(pl->sound), "%s/systems/%s/%s", g_home, pl->id, v);
   decl_get(ini, "graphics", "core_options_name", pl->gfx_core, sizeof(pl->gfx_core), "");
   decl_get(ini, "graphics", "enhanced", pl->gfx_enh, sizeof(pl->gfx_enh), "");
   decl_get(ini, "graphics", "original", pl->gfx_orig, sizeof(pl->gfx_orig), "");
   decl_get(ini, "graphics", "retroarch", v, sizeof(v), "");   /* alias of retroarch_enhanced */
   decl_get(ini, "graphics", "retroarch_enhanced", pl->gfx_ra_enh, sizeof(pl->gfx_ra_enh), v);
   decl_get(ini, "graphics", "retroarch_original", pl->gfx_ra_orig, sizeof(pl->gfx_ra_orig), "");
   pl->has_gfx = pl->gfx_enh[0] || pl->gfx_ra_enh[0];
   decl_get(ini, "demo", "allowed", v, sizeof(v), "0");  pl->demo_allowed = atoi(v) != 0;
   decl_get(ini, "demo", "seconds", v, sizeof(v), "0");  pl->demo_seconds = atoi(v);
   if (pl->demo_seconds < 0) pl->demo_seconds = 0;
   if (pl->demo_seconds > 600) pl->demo_seconds = 600;
}

static int cmp_order(const void *a, const void *b)
{
   const platform_t *x = a, *y = b;
   return x->order != y->order ? x->order - y->order : strcmp(x->id, y->id);
}

static void load_systems(void)
{
   char dir[300], path[600];
   DIR *d; struct dirent *de;
   snprintf(dir, sizeof(dir), "%s/systems", g_home);
   if ((d = opendir(dir)))
   {
      while ((de = readdir(d)) && g_nplat < MAX_PLATFORMS)
      {
         size_t l = strlen(de->d_name); FILE *f; char *buf; long sz;
         if (de->d_name[0] == '.' || l < 5 || strcasecmp(de->d_name + l - 4, ".ini")) continue;
         snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
         if (!(f = fopen(path, "rb"))) continue;
         fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
         buf = calloc(1, sz + 1);
         if (buf && fread(buf, 1, sz, f) == (size_t)sz) { platform_from_decl(&g_plat[g_nplat], buf); g_nplat++; }
         free(buf); fclose(f);
      }
      closedir(d);
   }
   if (!g_nplat)  /* built-in default: PlayStation library in the PSC layout */
   {
      char def[400];
      snprintf(def, sizeof(def), "[system]\nid=psx\nname=PlayStation\n[library]\nlayout=psc\npaths=%s/games/psx\n"
               "extensions=cue,pbp,chd,m3u\n[launch]\ncore=pcsx_rearmed_libretro.so\n[media]\nkind=disc\n", g_root);
      platform_from_decl(&g_plat[0], def);
      g_nplat = 1;
   }
   qsort(g_plat, g_nplat, sizeof(platform_t), cmp_order);
   logf_("systems: %d", g_nplat);
}

static int ext_ok(const platform_t *pl, const char *name)
{
   const char *dot = strrchr(name, '.');
   char list[160], *tok, *save = NULL;
   if (!dot || !pl->exts[0]) return 0;
   snprintf(list, sizeof(list), "%s", pl->exts);
   for (tok = strtok_r(list, ",", &save); tok; tok = strtok_r(NULL, ",", &save))
   { trim(tok); if (!strcasecmp(tok, dot + 1)) return 1; }
   return 0;
}

static void find_first(char *out, size_t n, const char *base, const char *const *suffixes)
{
   int b;
   for (b = 0; suffixes[b]; b++)
   {
      snprintf(out, n, "%s%s", base, suffixes[b]);
      if (file_exists(out)) return;
   }
   out[0] = 0;
}

/* PSC/BleemSync layout: <path>/<n>/Game.ini + <disc>.cue|pbp|... + <disc>.png */
static void scan_psc(int pi, const char *dir)
{
   char path[700], buf[4096], discs[512], base[700];
   static const char *const media_sfx[] = { ".png", ".jpg", NULL };
   static const char *const bg_sfx[] = { "background.jpg", "background.png", "bg.jpg", "bg.png", NULL };
   DIR *d; struct dirent *de; platform_t *pl = &g_plat[pi];
   if (!(d = opendir(dir))) { logf_("[%s] no dir %s", pl->id, dir); return; }
   while ((de = readdir(d)) && g_ngames < MAX_GAMES)
   {
      FILE *f; size_t n; game_t *g; char *c; char exts[160], *tok, *save = NULL;
      if (de->d_name[0] == '.') continue;
      snprintf(path, sizeof(path), "%s/%s/Game.ini", dir, de->d_name);
      if (!(f = fopen(path, "rb"))) continue;
      buf[0] = '\n';
      n = fread(buf + 1, 1, sizeof(buf) - 2, f); fclose(f);
      buf[n + 1] = 0;
      g = &g_games[g_ngames];
      memset(g, 0, sizeof(*g));
      ini_get(buf, "Title", g->title, sizeof(g->title));
      ini_get(buf, "Publisher", g->publisher, sizeof(g->publisher));
      ini_get(buf, "Year", g->year, sizeof(g->year));
      ini_get(buf, "Players", g->players, sizeof(g->players));
      ini_get(buf, "Discs", discs, sizeof(discs));
      if ((c = strchr(discs, ','))) *c = 0;
      trim(discs);
      if (!discs[0]) continue;
      g->content[0] = 0;
      snprintf(exts, sizeof(exts), "%s", pl->exts[0] ? pl->exts : "cue,pbp");
      for (tok = strtok_r(exts, ",", &save); tok && !g->content[0]; tok = strtok_r(NULL, ",", &save))
      {
         trim(tok);
         snprintf(g->content, sizeof(g->content), "%s/%s/%s.%s", dir, de->d_name, discs, tok);
         if (!file_exists(g->content)) g->content[0] = 0;
      }
      if (!g->content[0]) continue;
      snprintf(g->cover, sizeof(g->cover), "%s/%s/%s.png", dir, de->d_name, discs);
      g->has_cover = file_exists(g->cover);
      snprintf(base, sizeof(base), "%s/%s/%s", dir, de->d_name, pl->media_art);
      find_first(g->disc, sizeof(g->disc), base, media_sfx);
      snprintf(base, sizeof(base), "%s/%s/", dir, de->d_name);
      find_first(g->bg, sizeof(g->bg), base, bg_sfx);
      if (!g->title[0]) snprintf(g->title, sizeof(g->title), "%s", discs);
      g->platform = pi;
      g_ngames++;
   }
   closedir(d);
}

/* Files layout: <path>/<name>.<ext>, art next to it: <name>.png|jpg,
 * <name>-<media_art>.png|jpg, <name>-background.jpg|png */
static void scan_files(int pi, const char *dir)
{
   DIR *d; struct dirent *de; platform_t *pl = &g_plat[pi];
   static const char *const img_sfx[] = { ".png", ".jpg", NULL };
   static const char *const bg_sfx[] = { "-background.jpg", "-background.png", "-bg.jpg", "-bg.png", NULL };
   if (!(d = opendir(dir))) { logf_("[%s] no dir %s", pl->id, dir); return; }
   while ((de = readdir(d)) && g_ngames < MAX_GAMES)
   {
      game_t *g; char base[700], stem[256], *dot, *p;
      if (de->d_name[0] == '.' || !ext_ok(pl, de->d_name)) continue;
      g = &g_games[g_ngames];
      memset(g, 0, sizeof(*g));
      snprintf(g->content, sizeof(g->content), "%s/%s", dir, de->d_name);
      snprintf(stem, sizeof(stem), "%s", de->d_name);
      if ((dot = strrchr(stem, '.'))) *dot = 0;
      snprintf(base, sizeof(base), "%s/%s", dir, stem);
      find_first(g->cover, sizeof(g->cover), base, img_sfx);
      g->has_cover = g->cover[0] != 0;
      snprintf(base, sizeof(base), "%s/%s-%s", dir, stem, pl->media_art);
      find_first(g->disc, sizeof(g->disc), base, img_sfx);
      snprintf(base, sizeof(base), "%s/%s", dir, stem);
      find_first(g->bg, sizeof(g->bg), base, bg_sfx);
      /* title: file name without "(...)" / "[...]" tags */
      snprintf(g->title, sizeof(g->title), "%s", stem);
      if ((p = strpbrk(g->title, "([")) && p > g->title) *p = 0;
      trim(g->title);
      g->platform = pi;
      g_ngames++;
   }
   closedir(d);
}

static void scan_all(void)
{
   int pi;
   for (pi = 0; pi < g_nplat; pi++)
   {
      char paths[512], *tok, *save = NULL;
      snprintf(paths, sizeof(paths), "%s", g_plat[pi].paths);
      for (tok = strtok_r(paths, ";", &save); tok; tok = strtok_r(NULL, ";", &save))
      {
         trim(tok);
         if (!strcmp(g_plat[pi].layout, "psc")) scan_psc(pi, tok); else scan_files(pi, tok);
      }
   }
}

static int cmp_title(const void *a, const void *b)
{
   const game_t *x = a, *y = b;
   if (x->platform != y->platform) return x->platform - y->platform;
   return strcasecmp(x->title, y->title);
}

static void build_platform_index(void)
{
   int i, p, kept = 0, remap[MAX_PLATFORMS];
   for (p = 0; p < g_nplat; p++) g_plat[p].count = 0;
   for (i = 0; i < g_ngames; i++) g_plat[g_games[i].platform].count++;
   /* drop empty platforms, keeping game -> platform indices consistent */
   for (p = 0; p < g_nplat; p++)
   {
      remap[p] = g_plat[p].count > 0 ? kept : -1;
      if (g_plat[p].count > 0) g_plat[kept++] = g_plat[p];
   }
   g_nplat = kept;
   for (i = 0; i < g_ngames; i++) g_games[i].platform = remap[g_games[i].platform];
   qsort(g_games, g_ngames, sizeof(game_t), cmp_title);
   for (p = 0; p < g_nplat; p++) { g_plat[p].first = -1; g_plat[p].count = 0; }
   for (i = 0; i < g_ngames; i++)
   {
      platform_t *pl = &g_plat[g_games[i].platform];
      if (pl->first < 0) pl->first = i;
      pl->count++;
   }
}

/* ------------------------------------------------------------------------ */
/* GL resources                                                              */
static GLuint g_prog_box, g_prog_2d, g_vbo_quad, g_vao_quad;
static GLint  u_mvp, u_model, u_tex, u_tint, u_alpha, u_mode, u_light, u_cam, u_phase, u_refl;
static GLint  u2_mvp, u2_tex, u2_color, u2_mode, u2_rect;
static GLuint g_white;
TTF_Font *g_font_title, *g_font_sub, *g_font_small, *g_font_cover;

static const char *VS_BOX =
   "#version 300 es\n"
   "layout(location=0) in vec3 pos; layout(location=1) in vec2 uv; layout(location=2) in vec3 nrm;\n"
   "uniform mat4 mvp, model;\n"
   "out vec2 v_uv; out vec3 v_n; out vec3 v_w;\n"
   "void main(){ v_uv=uv; v_n=mat3(model)*nrm; v_w=(model*vec4(pos,1.0)).xyz; gl_Position=mvp*vec4(pos,1.0); }\n";

/* modes: 0 lit texture (insert, back card), 1 lit flat colour (tray plastic),
 *        2 floor reflection, 3 black spine with printed title, 4 clear glass lid */
static const char *FS_BOX =
   "#version 300 es\n"
   "precision mediump float;\n"
   "in vec2 v_uv; in vec3 v_n; in vec3 v_w;\n"
   "uniform sampler2D tex; uniform vec3 tint; uniform float alpha; uniform int mode;\n"
   "uniform vec3 light; uniform vec3 cam; uniform float phase; uniform float refl_y;\n"
   "out vec4 frag;\n"
   "vec4 shade(){\n"
   "  vec3 n = normalize(v_n);\n"
   "  vec3 l = normalize(light);\n"
   "  vec3 v = normalize(cam - v_w);\n"
   "  float ndl = max(dot(n, l), 0.0);\n"
   "  if (mode==2) { vec3 c = texture(tex, v_uv).rgb * tint; float f = smoothstep(0.62, 1.0, v_uv.y);\n"
   "                 return vec4(c, alpha * f * f * 0.20); }\n"
   "  if (mode==4) {\n"
   "    float ndv = clamp(abs(dot(n, v)), 0.0, 1.0);\n"
   "    float fres = 0.04 + 0.96 * pow(1.0 - ndv, 4.0);\n"
   "    vec3 r = reflect(-v, n);\n"
   "    float band = smoothstep(0.45, 0.95, r.y) * 0.45;\n"
   "    float d = v_uv.x - (1.0 - v_uv.y) * 0.55 - 0.18 + r.x * 0.9;\n"
   "    float streak = exp(-d * d * 60.0) * 0.30 + exp(-(d - 0.16) * (d - 0.16) * 260.0) * 0.12;\n"
   "    float spec = pow(max(dot(reflect(-l, n), v), 0.0), 70.0) * 0.7;\n"
   "    vec2 e = min(v_uv, 1.0 - v_uv);\n"
   "    float rim = 1.0 - smoothstep(0.0, 0.016, min(e.x, e.y));\n"
   "    float hinge = 1.0 - step(0.088, v_uv.x);\n"
   "    float ridges = smoothstep(0.009, 0.0, abs(v_uv.x - 0.020)) + smoothstep(0.009, 0.0, abs(v_uv.x - 0.072));\n"
   "    float a = 0.035 + fres * 0.45 + band * 0.35 + streak + spec + rim * 0.40 + ridges * 0.40 + hinge * 0.08;\n"
   "    return vec4(vec3(0.93, 0.96, 1.0), clamp(a, 0.0, 0.92) * alpha); }\n"
   "  if (mode==5 || mode==6) {\n"
   "    vec2 p = v_uv * 2.0 - 1.0; float r = length(p);\n"
   "    if (r > 1.0 || r < 0.15) discard;\n"
   "    float ang = atan(p.y, p.x) + phase;\n"
   "    float ndv = abs(dot(n, v));\n"
   "    float spokes = pow(abs(cos(ang - 0.6)), 36.0) * 0.55 + pow(abs(cos(ang + 0.9)), 90.0) * 0.25;\n"
   "    vec3 rainbow = 0.5 + 0.5 * cos(6.2832 * (fract(ang * 0.318 + r * 1.7 + ndv * 0.8) + vec3(0.0, 0.33, 0.67)));\n"
   "    float spec = pow(max(dot(reflect(-l, n), v), 0.0), 50.0) * 0.6;\n"
   "    if (r < 0.34) { float ring = smoothstep(0.32, 0.335, r) * (1.0 - smoothstep(0.345, 0.36, r));\n"
   "      return vec4(vec3(0.86, 0.88, 0.92) + ring * 0.4 + spec, (0.30 + ring * 0.6) * alpha); }\n"
   "    vec3 c;\n"
   "    if (mode==6) { c = texture(tex, v_uv).rgb * (0.55 + 0.45 * ndl) + rainbow * spokes * 0.25; }\n"
   "    else { vec3 silver = vec3(0.74, 0.75, 0.79) * (0.6 + 0.4 * ndl);\n"
   "           c = mix(silver, rainbow, 0.28 + spokes * 0.5) + vec3(spokes * 0.35); }\n"
   "    float rim = smoothstep(0.965, 0.99, r);\n"
   "    c = mix(c, vec3(0.55, 0.56, 0.60), rim * 0.6);\n"
   "    return vec4(c + spec, alpha); }\n"
   "  if (mode==7) { vec3 c = texture(tex, v_uv).rgb * tint; return vec4(c + vec3(pow(max(dot(reflect(-l, n), v), 0.0), 60.0) * 0.25), alpha); }\n"
   "  if (mode==8) { float d = length(v_uv - 0.5) * 2.0; return vec4(tint, alpha * pow(clamp(1.0 - d, 0.0, 1.0), 2.0)); }\n"
   "  if (mode==9) {\n"
   "    vec2 p = v_uv * 2.0 - 1.0; float r = length(p); if (r > 1.0) discard;\n"
   "    float ang = atan(p.y, p.x);\n"
   "    vec3 gold = vec3(0.95, 0.74, 0.28);\n"
   "    float rim = smoothstep(0.80, 0.84, r);\n"
   "    float ridges = 0.5 + 0.5 * sin(ang * 90.0);\n"
   "    float emboss = smoothstep(0.56, 0.60, r) * (1.0 - smoothstep(0.62, 0.66, r));\n"
   "    float sh = 0.55 + 0.45 * ndl + p.y * 0.15;\n"
   "    vec3 c = gold * sh * (1.0 - rim * 0.25 * ridges) + vec3(emboss * 0.25);\n"
   "    float spec = pow(max(dot(reflect(-l, n), v), 0.0), 30.0) * 0.8;\n"
   "    return vec4(c + spec, alpha); }\n"
   "  vec3 base;\n"
   "  if (mode==1) base = tint;\n"
   "  else if (mode==3) { float t = texture(tex, vec2(1.0 - v_uv.y, v_uv.x)).a; base = mix(vec3(0.030, 0.030, 0.036), vec3(0.90), t); }\n"
   "  else base = texture(tex, v_uv).rgb * tint;\n"
   "  float diff = 0.50 + 0.50 * ndl;\n"
   "  float spec = pow(max(dot(reflect(-l, n), v), 0.0), (mode==0) ? 18.0 : 40.0) * ((mode==0) ? 0.10 : 0.35);\n"
   "  return vec4(base * diff + vec3(spec), alpha);\n"
   "}\n"
   "void main(){\n"
   "  vec4 c = shade();\n"
   "  if (refl_y < 50.0) { float k = clamp((v_w.y - (refl_y - 0.55)) / 0.55, 0.0, 1.0); c.a *= k * k * 0.26; }\n"
   "  frag = c;\n"
   "}\n";

static const char *VS_2D =
   "#version 300 es\n"
   "layout(location=0) in vec3 pos; layout(location=1) in vec2 uv;\n"
   "uniform mat4 mvp; out vec2 v_uv;\n"
   "void main(){ v_uv=uv; gl_Position=mvp*vec4(pos,1.0); }\n";

static const char *FS_2D =
   "#version 300 es\n"
   "precision mediump float;\n"
   "in highp vec2 v_uv; uniform sampler2D tex; uniform vec4 color; uniform int mode; uniform highp vec4 rect; out vec4 frag;\n"
   "void main(){\n"
   "  if (mode==5) { highp vec2 hs = rect.xy * 0.5; highp float r = min(rect.z, min(hs.x, hs.y));\n"
   "                 highp vec2 q = abs((v_uv - 0.5) * rect.xy) - (hs - vec2(r));\n"
   "                 highp float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;\n"
   "                 float edge = clamp(0.5 - d, 0.0, 1.0);\n"
   "                 float bw = clamp(d + 2.0, 0.0, 1.0) * rect.w;\n"
   "                 vec3 fill = color.rgb + vec3(mix(0.06, -0.04, v_uv.y));\n"
   "                 float a = bw + color.a * (1.0 - bw);\n"
   "                 vec3 c = (vec3(bw) + fill * color.a * (1.0 - bw)) / max(a, 0.001);\n"
   "                 frag = vec4(c, a * edge); return; }\n"
   "  vec4 t = texture(tex, v_uv);\n"
   "  if (mode==1) { vec2 d = abs(v_uv - 0.5) * 2.0; float r = length(max(d - vec2(0.82, 0.0), 0.0)) ;\n"
   "                 float a = 1.0 - smoothstep(0.96, 1.0, max(d.y, r)); frag = vec4(color.rgb, color.a * a); return; }\n"
   "  if (mode==2) { float d = length((v_uv - 0.5) * vec2(1.6, 1.0)); frag = vec4(0.0,0.0,0.0, color.a * smoothstep(0.35, 0.95, d)); return; }\n"
   "  if (mode==3) { float d = length((v_uv - 0.5) * 2.0); frag = vec4(color.rgb, color.a * pow(clamp(1.0 - d, 0.0, 1.0), 2.2)); return; }\n"
   "  if (mode==4) { float dv = length((v_uv - 0.5) * vec2(1.6, 1.0)); float av = 0.55 * smoothstep(0.35, 0.95, dv);\n"
   "                 vec2 q = (v_uv - vec2(0.5, 0.4167)) / vec2(0.328, 0.4167); float as = color.a * pow(clamp(1.0 - length(q), 0.0, 1.0), 2.2);\n"
   "                 float a = as + av * (1.0 - as); frag = vec4(color.rgb * as / max(a, 0.001), a); return; }\n"
   "  frag = t * color;\n"
   "}\n";

static GLuint compile(GLenum type, const char *src)
{
   GLint ok; char info[1024];
   GLuint s = glCreateShader(type);
   glShaderSource(s, 1, &src, NULL);
   glCompileShader(s);
   glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
   if (!ok) { glGetShaderInfoLog(s, sizeof(info), NULL, info); logf_("shader error: %s", info); }
   return s;
}

static GLuint link_prog(const char *vs, const char *fs)
{
   GLint ok; char info[1024];
   GLuint p = glCreateProgram();
   glAttachShader(p, compile(GL_VERTEX_SHADER, vs));
   glAttachShader(p, compile(GL_FRAGMENT_SHADER, fs));
   glLinkProgram(p);
   glGetProgramiv(p, GL_LINK_STATUS, &ok);
   if (!ok) { glGetProgramInfoLog(p, sizeof(info), NULL, info); logf_("link error: %s", info); }
   return p;
}

static void gl_setup(void)
{
   /* unit quad in XY, centred, facing +Z: pos(3) uv(2) normal(3) */
   static const float q[] = {
      -0.5f, -0.5f, 0, 0, 1, 0, 0, 1,
       0.5f, -0.5f, 0, 1, 1, 0, 0, 1,
       0.5f,  0.5f, 0, 1, 0, 0, 0, 1,
      -0.5f, -0.5f, 0, 0, 1, 0, 0, 1,
       0.5f,  0.5f, 0, 1, 0, 0, 0, 1,
      -0.5f,  0.5f, 0, 0, 0, 0, 0, 1,
   };
   static const unsigned char white[4] = { 255, 255, 255, 255 };
   g_prog_box = link_prog(VS_BOX, FS_BOX);
   u_mvp = glGetUniformLocation(g_prog_box, "mvp");
   u_model = glGetUniformLocation(g_prog_box, "model");
   u_tex = glGetUniformLocation(g_prog_box, "tex");
   u_tint = glGetUniformLocation(g_prog_box, "tint");
   u_alpha = glGetUniformLocation(g_prog_box, "alpha");
   u_mode = glGetUniformLocation(g_prog_box, "mode");
   u_light = glGetUniformLocation(g_prog_box, "light");
   u_cam = glGetUniformLocation(g_prog_box, "cam");
   u_phase = glGetUniformLocation(g_prog_box, "phase");
   u_refl = glGetUniformLocation(g_prog_box, "refl_y");
   g_prog_2d = link_prog(VS_2D, FS_2D);
   u2_mvp = glGetUniformLocation(g_prog_2d, "mvp");
   u2_tex = glGetUniformLocation(g_prog_2d, "tex");
   u2_color = glGetUniformLocation(g_prog_2d, "color");
   u2_mode = glGetUniformLocation(g_prog_2d, "mode");
   u2_rect = glGetUniformLocation(g_prog_2d, "rect");

   glGenVertexArrays(1, &g_vao_quad);
   glBindVertexArray(g_vao_quad);
   glGenBuffers(1, &g_vbo_quad);
   glBindBuffer(GL_ARRAY_BUFFER, g_vbo_quad);
   glBufferData(GL_ARRAY_BUFFER, sizeof(q), q, GL_STATIC_DRAW);
   glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 32, (void *)0);
   glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 32, (void *)12);
   glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, 32, (void *)20);
   glEnableVertexAttribArray(0);
   glEnableVertexAttribArray(1);
   glEnableVertexAttribArray(2);

   glGenTextures(1, &g_white);
   glBindTexture(GL_TEXTURE_2D, g_white);
   glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
}

static void surface_avg(SDL_Surface *c, float *avg)
{
   double r = 0, g = 0, b = 0; long n = 0; int x, y;
   for (y = 0; y < c->h; y += 4)
      for (x = 0; x < c->w; x += 4)
      {
         const unsigned char *p = (const unsigned char *)c->pixels + y * c->pitch + x * 4;
         r += p[0]; g += p[1]; b += p[2]; n++;
      }
   if (n) { avg[0] = r / n / 255.0; avg[1] = g / n / 255.0; avg[2] = b / n / 255.0; }
}

/* Upload an SDL surface as RGBA (ABGR8888 == R,G,B,A bytes on little endian). */
static GLuint surface_to_tex(SDL_Surface *s, int mip, float *avg)
{
   GLuint t; SDL_Surface *c = SDL_ConvertSurfaceFormat(s, SDL_PIXELFORMAT_ABGR8888, 0);
   if (!c) return 0;
   if (avg)
   {
      double r = 0, g = 0, b = 0; long n = 0; int x, y;
      for (y = 0; y < c->h; y += 4)
         for (x = 0; x < c->w; x += 4)
         {
            const unsigned char *p = (const unsigned char *)c->pixels + y * c->pitch + x * 4;
            r += p[0]; g += p[1]; b += p[2]; n++;
         }
      if (n) { avg[0] = r / n / 255.0; avg[1] = g / n / 255.0; avg[2] = b / n / 255.0; }
   }
   glGenTextures(1, &t);
   glBindTexture(GL_TEXTURE_2D, t);
   glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
   glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, c->w, c->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, c->pixels);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mip ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
   if (mip) glGenerateMipmap(GL_TEXTURE_2D);
   SDL_FreeSurface(c);
   return t;
}

GLuint text_tex(TTF_Font *f, const char *s, int *w, int *h)
{
   SDL_Color white = { 255, 255, 255, 255 };
   SDL_Surface *surf; GLuint t;
   if (!f || !s || !*s) { *w = *h = 0; return 0; }
   surf = TTF_RenderUTF8_Blended(f, s, white);
   if (!surf) { *w = *h = 0; return 0; }
   *w = surf->w; *h = surf->h;
   t = surface_to_tex(surf, 0, NULL);
   SDL_FreeSurface(surf);
   return t;
}

GLuint text_tex_wrapped(TTF_Font *f, const char *s, int wrap_px, int *w, int *h)
{
   SDL_Color white = { 255, 255, 255, 255 };
   SDL_Surface *surf; GLuint t;
   if (!f || !s || !*s) { *w = *h = 0; return 0; }
   surf = TTF_RenderUTF8_Blended_Wrapped(f, s, white, (Uint32)wrap_px);
   if (!surf) { *w = *h = 0; return 0; }
   *w = surf->w; *h = surf->h;
   t = surface_to_tex(surf, 0, NULL);
   SDL_FreeSurface(surf);
   return t;
}

static unsigned hash_str(const char *s) { unsigned h = 2166136261u; while (*s) h = (h ^ (unsigned char)*s++) * 16777619u; return h; }

/* Back of the case / placeholder front: coloured card with wrapped text. */
static GLuint card_tex(const game_t *g, int back)
{
   SDL_Surface *card = SDL_CreateRGBSurface(0, 256, 256, 32, 0x000000ff, 0x0000ff00, 0x00ff0000, 0xff000000);
   SDL_Color white = { 255, 255, 255, 255 };
   SDL_Rect r; GLuint t; char line[160]; int y = 18;
   Uint8 cr = (Uint8)(g->col[0] * 255), cg = (Uint8)(g->col[1] * 255), cb = (Uint8)(g->col[2] * 255);
   if (!card) return 0;
   if (back) { cr = cr / 3 + 12; cg = cg / 3 + 12; cb = cb / 3 + 18; }
   SDL_FillRect(card, NULL, SDL_MapRGBA(card->format, cr, cg, cb, 255));
#define LINE(font, txt) do { SDL_Surface *ts = TTF_RenderUTF8_Blended_Wrapped(font, txt, white, 224); \
      if (ts) { r.x = 16; r.y = y; r.w = ts->w; r.h = ts->h; SDL_BlitSurface(ts, NULL, card, &r); y += ts->h + 6; SDL_FreeSurface(ts); } } while (0)
   if (g_font_cover)
   {
      if (!back) y = 150;
      LINE(g_font_cover, g->title);
      if (back)
      {
         snprintf(line, sizeof(line), "%s%s%s", g->year, g->year[0] && g->publisher[0] ? "  ·  " : "", g->publisher);
         if (line[0]) LINE(g_font_small, line);
         if (g->players[0])
         {
            trf(line, sizeof(line), strcmp(g->players, "1") ? "card.players" : "card.players_one", "n", g->players, NULL);
            LINE(g_font_small, line);
         }
         trf(line, sizeof(line), "card.memory_card", "name", profiles_display_name(profiles_active()), NULL);
         y = 220; LINE(g_font_small, line);
      }
   }
#undef LINE
   t = surface_to_tex(card, 1, NULL);
   SDL_FreeSurface(card);
   return t;
}

/* Spine print: the title in white on a transparent strip (sampled rotated). */
static GLuint spine_tex(const game_t *g)
{
   SDL_Color white = { 255, 255, 255, 255 };
   SDL_Surface *strip, *ts;
   SDL_Rect r; GLuint t;
   if (!g_font_small) return 0;
   strip = SDL_CreateRGBSurface(0, 512, 48, 32, 0x000000ff, 0x0000ff00, 0x00ff0000, 0xff000000);
   if (!strip) return 0;
   SDL_FillRect(strip, NULL, SDL_MapRGBA(strip->format, 0, 0, 0, 0));
   ts = TTF_RenderUTF8_Blended(g_font_small, g->title, white);
   if (ts)
   {
      SDL_SetSurfaceBlendMode(ts, SDL_BLENDMODE_NONE);
      r.w = ts->w > 470 ? 470 : ts->w; r.h = ts->h > 46 ? 46 : ts->h;
      r.x = 24; r.y = (48 - r.h) / 2;
      SDL_BlitSurface(ts, NULL, strip, &r);
      SDL_FreeSurface(ts);
   }
   t = surface_to_tex(strip, 1, NULL);
   SDL_FreeSurface(strip);
   return t;
}

static void game_placeholder(game_t *g)
{
   unsigned h = hash_str(g->title);
   g->col[0] = 0.25f + (h & 255) / 512.0f;
   g->col[1] = 0.20f + ((h >> 8) & 255) / 640.0f;
   g->col[2] = 0.30f + ((h >> 16) & 255) / 512.0f;
   g->tex = card_tex(g, 0);
   g->tex_state = -1;
   g->spine_tex = spine_tex(g);
}

static void game_finish_cover(game_t *g, SDL_Surface *s, const float avg[3])
{
   if (s)
   {
      g->tex = surface_to_tex(s, 1, NULL);
      memcpy(g->col, avg, sizeof(g->col));
      g->tex_state = 1;
      g->spine_tex = spine_tex(g);
   }
   if (!g->tex) game_placeholder(g);
}

static void game_free_tex(game_t *g)
{
   if (g->tex) glDeleteTextures(1, &g->tex);
   if (g->back_tex) glDeleteTextures(1, &g->back_tex);
   if (g->spine_tex) glDeleteTextures(1, &g->spine_tex);
   g->tex = g->back_tex = g->spine_tex = 0;
   g->tex_state = 0;
}

/* ------------------------------------------------------------------------ */
/* drawing                                                                   */
static mat4 g_proj, g_view, g_vp, g_ortho;

static void draw_quad_box(mat4 model, GLuint tex, const float tint[3], float alpha, int mode)
{
   mat4 mvp = m4_mul(g_vp, model);
   glUniformMatrix4fv(u_mvp, 1, GL_FALSE, mvp.m);
   glUniformMatrix4fv(u_model, 1, GL_FALSE, model.m);
   glUniform3fv(u_tint, 1, tint);
   glUniform1f(u_alpha, alpha);
   glUniform1i(u_mode, mode);
   glBindTexture(GL_TEXTURE_2D, tex ? tex : g_white);
   glDrawArrays(GL_TRIANGLES, 0, 6);
}

/* PS1 jewel case, height 1.0: 142 x 125 x 10 mm -> width 1.136, depth 0.08.
 * Square paper insert on the right, hinge strip on the left, black tray, glass lid. */
#define CASE_W  1.136f
#define CASE_D  0.080f
#define HINGE   0.105f
#define INSET_Z 0.012f

static void case_part(mat4 base, float x, float y, float z, float ry, float rx, float sx, float sy,
                      GLuint tex, const float tint[3], float alpha, int mode)
{
   mat4 m = m4_mul(base, m4_mul(m4_translate(x, y, z), m4_mul(m4_rot_y(ry), m4_mul(m4_rot_x(rx), m4_scale(sx, sy, 1)))));
   draw_quad_box(m, tex, tint, alpha, mode);
}

static GLuint g_disc_tex;   /* art for the disc being launched (0 = generic CD) */
static mat4   g_disc_start; /* world matrix of the disc in the tray (launch)       */
static int    g_detail = 1;  /* 0: skip expensive effects (far cases)               */
/* effect switches (all on; the bench mode turns them off one at a time) */
static int    g_fx_vignette = 1, g_fx_refl = 1, g_fx_glass = 1, g_fx_sides = 1, g_fx_text = 1;
static GLuint g_vig_tex, g_spot_tex;   /* baked full-screen vignette / spotlight masks */

static float smooth01(float e0, float e1, float x) { float t = clampf((x - e0) / (e1 - e0), 0, 1); return t * t * (3 - 2 * t); }
static void make_fx_textures(void)
{
   enum { TW = 320, TH = 180 };
   static unsigned char vig[TW * TH * 4], spot[TW * TH * 4];
   int x, y;
   for (y = 0; y < TH; y++)
      for (x = 0; x < TW; x++)
      {
         float u = (x + 0.5f) / TW, v = (y + 0.5f) / TH, dx, dy, d, a;
         unsigned char *pv = vig + (y * TW + x) * 4, *ps = spot + (y * TW + x) * 4;
         dx = (u - 0.5f) * 1.6f; dy = v - 0.5f; d = sqrtf(dx * dx + dy * dy);
         pv[0] = pv[1] = pv[2] = 0; pv[3] = (unsigned char)(255 * 0.55f * smooth01(0.35f, 0.95f, d));
         dx = (u - 0.5f) / 0.328f; dy = (v - 0.4167f) / 0.4167f; d = sqrtf(dx * dx + dy * dy);
         a = powf(clampf(1.0f - d, 0, 1), 2.2f);
         ps[0] = ps[1] = ps[2] = 255; ps[3] = (unsigned char)(255 * a);
      }
   glGenTextures(1, &g_vig_tex);
   glBindTexture(GL_TEXTURE_2D, g_vig_tex);
   glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, TW, TH, 0, GL_RGBA, GL_UNSIGNED_BYTE, vig);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
   glGenTextures(1, &g_spot_tex);
   glBindTexture(GL_TEXTURE_2D, g_spot_tex);
   glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, TW, TH, 0, GL_RGBA, GL_UNSIGNED_BYTE, spot);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

/* open: 0 closed .. 1 lid fully open; lift/fly/spin drive the disc (launch only) */
static void draw_case(const game_t *g, mat4 base, float alpha, float open, float lift, float fly, float spin)
{
   static const float one[3] = { 1, 1, 1 };
   static const float tray[3] = { 0.045f, 0.045f, 0.052f };
   static const float edge[3] = { 0.16f, 0.17f, 0.19f };
   static const float paper[3] = { 0.86f, 0.86f, 0.84f };
   const float hw = CASE_W * 0.5f, hd = CASE_D * 0.5f, PI = (float)M_PI;
   const float ins_l = -hw + HINGE, ins_r = hw - 0.018f, ins_w = ins_r - ins_l, ins_c = (ins_l + ins_r) * 0.5f;
   mat4 lid;

   /* tray: back card, edges, spine, hinge strip, floor */
   if (g->back_tex) case_part(base, 0, 0, -hd, PI, 0, CASE_W, 1.0f, g->back_tex, one, alpha, 0);
   else             case_part(base, 0, 0, -hd, PI, 0, CASE_W, 1.0f, 0, tray, alpha, 1);
   case_part(base, hw, 0, 0, PI / 2, 0, CASE_D, 1.0f, 0, edge, alpha, 1);
   case_part(base, 0, 0.5f, 0, 0, -PI / 2, CASE_W, CASE_D, 0, edge, alpha, 1);
   case_part(base, 0, -0.5f, 0, 0, PI / 2, CASE_W, CASE_D, 0, edge, alpha, 1);
   case_part(base, -hw, 0, 0, -PI / 2, 0, CASE_D, 1.0f, g->spine_tex, one, alpha, 3);
   case_part(base, -hw + HINGE * 0.5f, 0, hd - INSET_Z - 0.002f, 0, 0, HINGE, 1.0f, 0, tray, alpha, 1);
   if (open > 0.0f)
      case_part(base, ins_c, 0, -hd + 0.004f, 0, 0, ins_w, 0.98f, 0, tray, alpha, 1);

   /* disc sitting in the tray and lifting; once it flies it is drawn in world space */
   if (open > 0.3f)
   {
      g_disc_start = m4_mul(base, m4_mul(m4_translate(ins_c, 0, -hd + 0.012f + lift * 0.16f), m4_scale(0.92f, 0.92f, 1)));
      if (fly <= 0.0f)
      {
         glUniform1f(u_phase, spin);
         draw_quad_box(m4_mul(g_disc_start, m4_rot_z(-spin)), g_disc_tex, one, alpha, g_disc_tex ? 6 : 5);
      }
   }

   /* lid = paper insert + glass, rotating about the hinge at the left front edge */
   lid = m4_mul(base, m4_mul(m4_translate(-hw, 0, hd), m4_mul(m4_rot_y(-open * 1.95f), m4_translate(hw, 0, -hd))));
   case_part(lid, ins_c, 0, hd - INSET_Z, 0, 0, ins_w, 0.972f, g->tex, one, alpha, 0);
   if (open > 0.0f)
      case_part(lid, ins_c, 0, hd - INSET_Z - 0.001f, PI, 0, ins_w, 0.972f, 0, paper, alpha, 1);
   if (g_detail && g_fx_glass)
   {
      glDepthMask(GL_FALSE);
      case_part(lid, 0, 0, hd, 0, 0, CASE_W, 1.0f, 0, one, alpha, 4);
      glDepthMask(GL_TRUE);
   }
}

/* The flying disc: from its place in the tray to the centre of the screen,
 * growing to ~80 % of the screen height, spinning. */
static void draw_flying_disc(float fly, float spin, float alpha)
{
   static const float one[3] = { 1, 1, 1 };
   const float *a = g_disc_start.m;
   float sx = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
   float k = fly * fly * (3 - 2 * fly);
   float x = lerpf(a[12], 0.0f, k), y = lerpf(a[13], -0.22f, k), z = lerpf(a[14], 2.55f, k);
   float d = lerpf(sx, 1.18f, k);
   mat4 m = m4_mul(m4_translate(x, y, z), m4_mul(m4_rot_z(-spin), m4_scale(d, d, 1)));
   glUniform1f(u_phase, spin);
   draw_quad_box(m, g_disc_tex, one, alpha, g_disc_tex ? 6 : 5);
}

/* Cartridge: plastic shell (declared colour/aspect), label with the game art,
 * grip ridges at the bottom. lift/drop/tilt drive the insert animation. */
static void draw_cart(const game_t *g, const platform_t *pl, mat4 base, float alpha)
{
   static const float one[3] = { 1, 1, 1 };
   const float w = pl->w, h = pl->h, d = 0.13f, hw = w * 0.5f, hh = h * 0.5f, hd = d * 0.5f, PI = (float)M_PI;
   float body[3], dark[3], light[3];
   int k;
   for (k = 0; k < 3; k++) { body[k] = pl->color[k]; dark[k] = pl->color[k] * 0.62f; light[k] = pl->color[k] * 0.85f + 0.05f; }
   case_part(base, 0, 0, hd, 0, 0, w, h, 0, body, alpha, 1);                       /* front */
   case_part(base, 0, 0, -hd, PI, 0, w, h, 0, dark, alpha, 1);                     /* back */
   case_part(base, hw, 0, 0, PI / 2, 0, d, h, 0, light, alpha, 1);                 /* right */
   case_part(base, -hw, 0, 0, -PI / 2, 0, d, h, 0, light, alpha, 1);               /* left */
   case_part(base, 0, hh, 0, 0, -PI / 2, w, d, 0, light, alpha, 1);                /* top */
   case_part(base, 0, -hh, 0, 0, PI / 2, w, d, 0, dark, alpha, 1);                 /* bottom */
   /* label: upper part of the front, inset */
   case_part(base, 0, hh - 0.08f * h - 0.62f * h * 0.5f, hd + 0.002f, 0, 0, w * 0.82f, h * 0.62f, g->tex, one, alpha, 0);
   /* grip ridges */
   for (k = 0; k < 4; k++)
      case_part(base, 0, -hh + 0.06f * h + k * 0.045f * h, hd + 0.003f, 0, 0, w * 0.55f, 0.012f * h, 0, dark, alpha, 1);
}

/* Arcade marquee: black frame, backlit art (emissive), coloured halo. lit: 0..1.25 */
static void draw_marquee(const game_t *g, const platform_t *pl, mat4 base, float alpha, float lit)
{
   static const float frame[3] = { 0.03f, 0.03f, 0.035f };
   const float w = pl->w, h = pl->h, d = 0.06f, hd = d * 0.5f, PI = (float)M_PI;
   float tint[3], glow[3];
   tint[0] = tint[1] = tint[2] = lit;
   glow[0] = g->col[0] * lit; glow[1] = g->col[1] * lit; glow[2] = g->col[2] * lit;
   glBlendFunc(GL_SRC_ALPHA, GL_ONE);
   glDepthMask(GL_FALSE);
   case_part(base, 0, 0, -hd - 0.02f, 0, 0, w * 1.6f, h * 2.4f, 0, glow, alpha * 0.55f, 8);
   glDepthMask(GL_TRUE);
   glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
   case_part(base, 0, 0, hd - 0.002f, 0, 0, w + 0.06f, h + 0.06f, 0, frame, alpha, 1);
   case_part(base, 0, 0, -hd, PI, 0, w + 0.06f, h + 0.06f, 0, frame, alpha, 1);
   case_part(base, 0, 0, hd, 0, 0, w, h, g->tex, tint, alpha, 7);
}

/* dispatcher: one renderer per declared media kind */
static void draw_media(const game_t *g, const platform_t *pl, mat4 base, float alpha,
                       float open, float lift, float fly, float spin, float lit)
{
   if (pl->kind == KIND_CART) draw_cart(g, pl, base, alpha);
   else if (pl->kind == KIND_ARCADE) draw_marquee(g, pl, base, alpha, lit);
   else draw_case(g, base, alpha, open, lift, fly, spin);
}

/* Mirror the insert about the floor plane under the case (y = -0.5*scale). */
static void draw_reflection(const game_t *g, const platform_t *pl, mat4 base, float alpha)
{
   static const float one[3] = { 1, 1, 1 };
   const float hw = CASE_W * 0.5f, ins_l = -hw + HINGE, ins_r = hw - 0.018f;
   float floor_y = -0.5f * pl->h * g->sc.x - 0.01f;
   mat4 m = m4_mul(m4_translate(0, 2.0f * floor_y, 0), m4_mul(m4_scale(1, -1, 1), base));
   if (pl->kind == KIND_DISC)
      m = m4_mul(m, m4_mul(m4_translate((ins_l + ins_r) * 0.5f, 0, CASE_D * 0.5f), m4_scale(ins_r - ins_l, 0.972f, 1)));
   else
      m = m4_mul(m, m4_mul(m4_translate(0, 0, 0.065f), m4_scale(pl->w, pl->h, 1)));
   draw_quad_box(m, g->tex, one, alpha, 2);
}

void draw_2d(GLuint tex, float x, float y, float w, float h, const float rgba[4], int mode)
{
   mat4 m = m4_mul(g_ortho, m4_mul(m4_translate(x + w * 0.5f, y + h * 0.5f, 0), m4_scale(w, -h, 1)));
   glUniformMatrix4fv(u2_mvp, 1, GL_FALSE, m.m);
   glUniform4fv(u2_color, 1, rgba);
   glUniform1i(u2_mode, mode);
   glBindTexture(GL_TEXTURE_2D, tex ? tex : g_white);
   glDrawArrays(GL_TRIANGLES, 0, 6);
}

/* glass rounded rectangle (mode 5): the shader needs the size in pixels for its distance field */
void draw_round_rect(float x, float y, float w, float h, float radius, const float rgba[4], float border_alpha)
{
   glUniform4f(u2_rect, w, h, radius, border_alpha);
   draw_2d(0, x, y, w, h, rgba, 5);
}

/* ------------------------------------------------------------------------ */
void label_set(label_t *l, TTF_Font *f, const char *s)
{
   if (!strcmp(l->key, s) && (l->tex || !*s)) return;
   if (l->tex) glDeleteTextures(1, &l->tex);
   snprintf(l->key, sizeof(l->key), "%s", s);
   l->tex = text_tex(f, s, &l->w, &l->h);
}

void label_free(label_t *l)
{
   if (l->tex) glDeleteTextures(1, &l->tex);
   memset(l, 0, sizeof(*l));
}

void gl_use_2d(void)
{
   glUseProgram(g_prog_2d);
   glUniform1i(u2_tex, 0);
}

static TTF_Font *open_font(const char **paths, int size)
{
   int i;
   for (i = 0; paths[i]; i++)
   {
      TTF_Font *f = TTF_OpenFont(paths[i], size);
      if (f) return f;
   }
   logf_("no font found for size %d", size);
   return NULL;
}

/* ------------------------------------------------------------------------ */
/* covers: decoded on a worker thread, uploaded on the main thread           */
static SDL_mutex   *cv_mx;
static SDL_cond    *cv_cv;
static int          cv_req = -1, cv_busy, cv_done = -1, cv_quit;
static char         cv_path[512];
static SDL_Surface *cv_surf;
static float        cv_avg[3];

static int cover_worker(void *unused)
{
   (void)unused;
   SDL_LockMutex(cv_mx);
   while (!cv_quit)
   {
      int idx; char path[512]; SDL_Surface *s, *c = NULL; float avg[3] = { 0.3f, 0.3f, 0.35f };
      while (cv_req < 0 && !cv_quit) SDL_CondWait(cv_cv, cv_mx);
      if (cv_quit) break;
      idx = cv_req; cv_req = -1;
      snprintf(path, sizeof(path), "%s", cv_path);
      SDL_UnlockMutex(cv_mx);
      s = IMG_Load(path);
      if (s) { c = SDL_ConvertSurfaceFormat(s, SDL_PIXELFORMAT_ABGR8888, 0); SDL_FreeSurface(s); }
      if (c) surface_avg(c, avg);
      SDL_LockMutex(cv_mx);
      cv_surf = c; cv_done = idx;
      memcpy(cv_avg, avg, sizeof(avg));
   }
   SDL_UnlockMutex(cv_mx);
   return 0;
}

static void cover_request(int idx, const char *path)
{
   SDL_LockMutex(cv_mx);
   snprintf(cv_path, sizeof(cv_path), "%s", path);
   cv_req = idx; cv_busy = 1;
   SDL_CondSignal(cv_cv);
   SDL_UnlockMutex(cv_mx);
}

/* ------------------------------------------------------------------------ */
/* per-game background: decoded on a worker thread, crossfaded on the GPU    */
static SDL_mutex   *bg_mx;
static SDL_cond    *bg_cv;
static char         bg_req[512];      /* path requested (guarded by bg_mx) */
static int          bg_req_new;
static SDL_Surface *bg_done;          /* decoded surface ready to upload   */
static char         bg_done_path[512];
static int          bg_quit;

static int bg_worker(void *unused)
{
   (void)unused;
   SDL_LockMutex(bg_mx);
   while (!bg_quit)
   {
      char path[512]; SDL_Surface *s, *c = NULL;
      while (!bg_req_new && !bg_quit) SDL_CondWait(bg_cv, bg_mx);
      if (bg_quit) break;
      snprintf(path, sizeof(path), "%s", bg_req);
      bg_req_new = 0;
      SDL_UnlockMutex(bg_mx);
      s = path[0] ? IMG_Load(path) : NULL;
      if (s) { c = SDL_ConvertSurfaceFormat(s, SDL_PIXELFORMAT_ABGR8888, 0); SDL_FreeSurface(s); }
      SDL_LockMutex(bg_mx);
      if (bg_done) SDL_FreeSurface(bg_done);
      bg_done = c;
      snprintf(bg_done_path, sizeof(bg_done_path), "%s", path);
      if (!c && path[0]) logf_("background failed: %s", path);
   }
   SDL_UnlockMutex(bg_mx);
   return 0;
}

static void bg_request(const char *path)
{
   SDL_LockMutex(bg_mx);
   snprintf(bg_req, sizeof(bg_req), "%s", path);
   bg_req_new = 1;
   SDL_CondSignal(bg_cv);
   SDL_UnlockMutex(bg_mx);
}

/* ------------------------------------------------------------------------ */
/* input: game controllers + raw joysticks mapped to logical actions         */
/* IN_SQUARE acts as IN_BACK everywhere except the keyboard, where it erases */
enum { IN_LEFT, IN_RIGHT, IN_UP, IN_DOWN, IN_L1, IN_R1, IN_PLAY, IN_BACK, IN_SQUARE, IN_START, IN_SELECT, IN_OPTIONS, IN_COUNT };
static int           in_down[IN_COUNT];     /* current state                  */
static int           in_pressed[IN_COUNT];  /* edge this frame                */
static SDL_JoystickID in_ctrl_ids[8];       /* joysticks opened as controllers */
static int           in_nctrl;
static double        in_last_activity;      /* last input of any kind (attract, step 5) */
int                  g_want_settings;   /* set when Select is tapped alone; consumed by the settings panel */

static void in_set(int b, int down)
{
   if (b < 0) return;
   if (down && !in_down[b]) { in_pressed[b] = 1; in_last_activity = now_s(); }
   in_down[b] = down;
}

static int in_is_ctrl(SDL_JoystickID id)
{
   int i;
   for (i = 0; i < in_nctrl; i++) if (in_ctrl_ids[i] == id) return 1;
   return 0;
}

static void in_open(int index)
{
   char guid[64];
   SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(index), guid, sizeof(guid));
   if (SDL_IsGameController(index))
   {
      SDL_GameController *c = SDL_GameControllerOpen(index);
      if (c && in_nctrl < 8)
      {
         in_ctrl_ids[in_nctrl++] = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(c));
         logf_("controller %d: %s (guid %s)", index, SDL_GameControllerName(c), guid);
         return;
      }
   }
   if (SDL_JoystickOpen(index))
      logf_("joystick %d (raw): %s (guid %s)", index, SDL_JoystickNameForIndex(index), guid);
   else
      logf_("joystick %d failed: %s", index, SDL_GetError());
}

/* Raw button layout of the PlayStation Classic pad (and most PS-style pads):
 * 0 triangle, 1 circle, 2 cross, 3 square, 4 L2, 5 R2, 6 L1, 7 R1, 8 select, 9 start */
static int in_raw_button(int b)
{
   switch (b)
   {
   case 0: return IN_OPTIONS;
   case 3: return IN_SQUARE;
   case 2: return IN_PLAY;
   case 1: return IN_BACK;
   case 6: return IN_L1;
   case 7: return IN_R1;
   case 8: return IN_SELECT;
   case 9: return IN_START;
   }
   return -1;
}

static void in_axis(int axis, int value)
{
   int neg = value < -16000, pos = value > 16000;
   if (neg || pos) in_last_activity = now_s();
   if (axis == 0) { in_set(IN_LEFT, neg); in_set(IN_RIGHT, pos); }
   if (axis == 1) { in_set(IN_UP, neg); in_set(IN_DOWN, pos); }
}

static void in_event(const SDL_Event *e)
{
   if (e->type == SDL_KEYDOWN || e->type == SDL_CONTROLLERBUTTONDOWN || e->type == SDL_JOYBUTTONDOWN ||
       (e->type == SDL_JOYHATMOTION && e->jhat.value != SDL_HAT_CENTERED))
      in_last_activity = now_s();
   switch (e->type)
   {
   case SDL_JOYDEVICEADDED:
      if (!SDL_IsGameController(e->jdevice.which)) in_open(e->jdevice.which);
      break;
   case SDL_CONTROLLERDEVICEADDED:
      in_open(e->cdevice.which);
      break;
   case SDL_CONTROLLERBUTTONDOWN: case SDL_CONTROLLERBUTTONUP:
   {
      int d = e->type == SDL_CONTROLLERBUTTONDOWN, b = -1;
      switch (e->cbutton.button)
      {
      case SDL_CONTROLLER_BUTTON_DPAD_LEFT: b = IN_LEFT; break;
      case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: b = IN_RIGHT; break;
      case SDL_CONTROLLER_BUTTON_DPAD_UP: b = IN_UP; break;
      case SDL_CONTROLLER_BUTTON_DPAD_DOWN: b = IN_DOWN; break;
      case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: b = IN_L1; break;
      case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: b = IN_R1; break;
      case SDL_CONTROLLER_BUTTON_A: b = IN_PLAY; break;
      case SDL_CONTROLLER_BUTTON_B: b = IN_BACK; break;
      case SDL_CONTROLLER_BUTTON_X: b = IN_SQUARE; break;
      case SDL_CONTROLLER_BUTTON_Y: b = IN_OPTIONS; break;
      case SDL_CONTROLLER_BUTTON_START: b = IN_START; break;
      case SDL_CONTROLLER_BUTTON_BACK: b = IN_SELECT; break;
      }
      in_set(b, d);
      break;
   }
   case SDL_CONTROLLERAXISMOTION:
      if (e->caxis.axis == SDL_CONTROLLER_AXIS_LEFTX) in_axis(0, e->caxis.value);
      if (e->caxis.axis == SDL_CONTROLLER_AXIS_LEFTY) in_axis(1, e->caxis.value);
      break;
   case SDL_JOYBUTTONDOWN: case SDL_JOYBUTTONUP:
      if (!in_is_ctrl(e->jbutton.which))
         in_set(in_raw_button(e->jbutton.button), e->type == SDL_JOYBUTTONDOWN);
      break;
   case SDL_JOYAXISMOTION:
      if (!in_is_ctrl(e->jaxis.which)) in_axis(e->jaxis.axis, e->jaxis.value);
      break;
   case SDL_JOYHATMOTION:
      if (!in_is_ctrl(e->jhat.which))
      {
         in_set(IN_LEFT, (e->jhat.value & SDL_HAT_LEFT) != 0);
         in_set(IN_RIGHT, (e->jhat.value & SDL_HAT_RIGHT) != 0);
         in_set(IN_UP, (e->jhat.value & SDL_HAT_UP) != 0);
         in_set(IN_DOWN, (e->jhat.value & SDL_HAT_DOWN) != 0);
      }
      break;
   case SDL_KEYDOWN: case SDL_KEYUP:
   {
      int d = e->type == SDL_KEYDOWN, b = -1;
      if (e->key.repeat) break;
      if (d) logf_("key: sym 0x%x scancode %d (%s)", (unsigned)e->key.keysym.sym,
                   (int)e->key.keysym.scancode, SDL_GetKeyName(e->key.keysym.sym));
      switch (e->key.keysym.sym)
      {
      case SDLK_LEFT: b = IN_LEFT; break;
      case SDLK_RIGHT: b = IN_RIGHT; break;
      case SDLK_UP: b = IN_UP; break;
      case SDLK_DOWN: b = IN_DOWN; break;
      case SDLK_PAGEUP: b = IN_L1; break;
      case SDLK_PAGEDOWN: b = IN_R1; break;
      case SDLK_RETURN: b = IN_PLAY; break;
      case SDLK_TAB: case SDLK_F1: b = IN_OPTIONS; break;
      case SDLK_BACKSPACE: b = IN_SQUARE; break;
      case SDLK_ESCAPE: b = IN_SELECT; in_set(IN_START, d); break;
      }
      in_set(b, d);
      break;
   }
   }
}

/* ------------------------------------------------------------------------ */
/* help bar: button icons ($home/icons/<name>.png, optional) + labels        */
/* navigation for UI modules: press edges, directions auto-repeat like the carousel */
static void nav_fill(ui_nav_t *nv, double t)
{
   static const int dirs[4] = { IN_UP, IN_DOWN, IN_LEFT, IN_RIGHT };
   static double next[4];
   int *out[4], k;
   memset(nv, 0, sizeof(*nv));
   out[0] = &nv->up; out[1] = &nv->down; out[2] = &nv->left; out[3] = &nv->right;
   for (k = 0; k < 4; k++)
   {
      if (in_pressed[dirs[k]]) { *out[k] = 1; next[k] = t + 0.32; }
      else if (in_down[dirs[k]] && t >= next[k]) { *out[k] = 1; next[k] = t + 0.085; }
   }
   nv->confirm = in_pressed[IN_PLAY];
   nv->back = in_pressed[IN_BACK] || in_pressed[IN_SQUARE];
   nv->erase = in_pressed[IN_SQUARE];
   nv->options = in_pressed[IN_OPTIONS];
   nv->start = in_pressed[IN_START];
   nv->l1 = in_pressed[IN_L1];
   nv->r1 = in_pressed[IN_R1];
}

/* ------------------------------------------------------------------------ */
/* help bar: one table per set (panel.h HINTS_*), button icons from
 * $home/icons/<name>.png (loaded once, shared), fallback text when one is missing */
typedef struct { const char *icons[3]; const char *fallback_key; const char *text_key; } hint_t;
#define SET_HINTS 5
static const hint_t g_hints[HINTS_COUNT][SET_HINTS] = {
   [HINTS_CAROUSEL] = {
      { { "cross" },    "btn.cross",        "hint.play" },
      { { "l1", "r1" }, "btn.l1_r1",        "hint.platform" },
      { { "select" },   "btn.select",       "hint.settings" } },
   [HINTS_PANEL_ROOT] = {
      { { "dpad" },     "btn.dpad",         "hint.move" },
      { { "cross" },    "btn.cross",        "hint.select" },
      { { "circle" },   "btn.circle",       "hint.close" } },
   [HINTS_PANEL_PAGE] = {   /* pages with choice rows */
      { { "dpad" },     "btn.dpad",         "hint.change" },
      { { "cross" },    "btn.cross",        "hint.select" },
      { { "circle" },   "btn.circle",       "hint.back" } },
   [HINTS_PANEL_LIST] = {   /* pages of plain actions (Language) */
      { { "dpad" },     "btn.dpad",         "hint.move" },
      { { "cross" },    "btn.cross",        "hint.select" },
      { { "circle" },   "btn.circle",       "hint.back" } },
   [HINTS_PANEL_INFO] = {   /* nothing selectable (About) */
      { { "circle" },   "btn.circle",       "hint.back" } },
   [HINTS_OSK] = {
      { { "cross" },    "btn.cross",        "hint.type" },
      { { "l1", "r1" }, "btn.l1_r1",        "hint.accent" },
      { { "square" },   "btn.square",       "hint.erase" },
      { { "start" },    "btn.start",        "hint.done" },
      { { "circle" },   "btn.circle",       "hint.cancel" } },
   [HINTS_CONFIRM] = {
      { { "dpad" },     "btn.dpad",         "hint.move" },
      { { "cross" },    "btn.cross",        "hint.select" },
      { { "circle" },   "btn.circle",       "hint.cancel" } },
   [HINTS_ATTRACT] = { { { NULL }, NULL, NULL } },   /* the attract scene draws its own PRESS START */
};
typedef struct { char name[16]; GLuint tex; int w, h; } icon_t;
static icon_t g_icons[16];
static int    g_nicons;
typedef struct { const icon_t *icon[3]; int have_icons; GLuint fb, txt; int fbw, fbh, tw, th; } hint_gl_t;
static hint_gl_t g_hint_gl[HINTS_COUNT][SET_HINTS];
static float     g_hint_cx = W * 0.5f;   /* horizontal centre of the help bar */

static const icon_t *icon_get(const char *home, const char *name)
{
   int i; char path[400]; SDL_Surface *sf; icon_t *ic;
   for (i = 0; i < g_nicons; i++) if (!strcmp(g_icons[i].name, name)) return &g_icons[i];
   if (g_nicons >= (int)(sizeof(g_icons) / sizeof(g_icons[0]))) return NULL;
   ic = &g_icons[g_nicons++];
   snprintf(ic->name, sizeof(ic->name), "%s", name);
   snprintf(path, sizeof(path), "%s/icons/%s.png", home, name);
   if ((sf = IMG_Load(path))) { ic->w = sf->w; ic->h = sf->h; ic->tex = surface_to_tex(sf, 1, NULL); SDL_FreeSurface(sf); }
   return ic;
}

/* (re)build the hint texts in the active language; icons stay */
static void hints_reload(void)
{
   int s, i;
   for (s = 0; s < HINTS_COUNT; s++)
      for (i = 0; i < SET_HINTS && g_hints[s][i].text_key; i++)
      {
         hint_gl_t *h = &g_hint_gl[s][i];
         if (h->fb) glDeleteTextures(1, &h->fb);
         if (h->txt) glDeleteTextures(1, &h->txt);
         h->fb = h->txt = 0; h->fbw = h->fbh = h->tw = h->th = 0;
         if (!h->have_icons)
            h->fb = text_tex(g_font_small, tr(g_hints[s][i].fallback_key), &h->fbw, &h->fbh);
         h->txt = text_tex(g_font_small, tr(g_hints[s][i].text_key), &h->tw, &h->th);
      }
}

/* icons once ($home/icons/<name>.png), then the texts */
static void hints_load(const char *home)
{
   int s, i, k;
   for (s = 0; s < HINTS_COUNT; s++)
      for (i = 0; i < SET_HINTS && g_hints[s][i].text_key; i++)
      {
         hint_gl_t *h = &g_hint_gl[s][i];
         h->have_icons = 1;
         for (k = 0; k < 3 && g_hints[s][i].icons[k]; k++)
         {
            h->icon[k] = icon_get(home, g_hints[s][i].icons[k]);
            if (!h->icon[k] || !h->icon[k]->tex || h->icon[k]->h <= 0) h->have_icons = 0;
         }
      }
   hints_reload();
}

/* section 5.3: text baked into textures follows the language; every other label is
 * set each frame through label_set(..., tr(...)) */
void ui_language_changed(void)
{
   hints_reload();
   ui_cards_changed();
}

/* the back of the case shows the active profile: regenerate it lazily (7.4) */
void ui_cards_changed(void)
{
   int i;
   for (i = 0; i < g_ngames; i++)
      if (g_games[i].back_tex) { glDeleteTextures(1, &g_games[i].back_tex); g_games[i].back_tex = 0; }
}

/* "Preview now" in the screensaver page */
void ui_start_attract(void)
{
   attract_start(now_s());
}

/* draw one hint set centred on (g_hint_cx, y) */
static void hints_draw(int set, float y, float alpha)
{
   /* icons are pixel art authored at 36 px (exported x4): draw them at exactly 36 px,
    * on whole pixels, so every art pixel lands on one screen pixel */
   /* icons are used only when every one of the hint loaded (have_icons, h > 0); else
    * the fallback text, and with neither (empty btn.* and no PNG) just the label */
   const float icon_h = 36.0f, gap_icon = 6.0f, gap_txt = 10.0f;
   const float ic[4] = { 1, 1, 1, alpha }, tc[4] = { 0.62f, 0.61f, 0.72f, alpha };
   float total = 0, x, gap_hint = 42.0f, avail;
   int i, k, n = 0;
   if (set < 0 || set >= HINTS_COUNT || alpha <= 0.003f) return;
   while (n < SET_HINTS && g_hints[set][n].text_key) n++;
   for (i = 0; i < n; i++)
   {
      hint_gl_t *h = &g_hint_gl[set][i];
      if (h->fb) total += h->fbw + gap_txt;
      else if (h->have_icons)
      {
         for (k = 0; k < 3 && h->icon[k]; k++) total += floorf(icon_h * h->icon[k]->w / (float)h->icon[k]->h + 0.5f) + gap_icon;
         total += gap_txt;
      }
      total += h->tw + (i < n - 1 ? gap_hint : 0);
   }
   /* the bar is centred in [0, 2 * g_hint_cx]: tighten the gaps when a long language
    * would not fit (5 hints of the keyboard in German left of the panel) */
   avail = 2.0f * g_hint_cx - 32.0f;
   if (total > avail && n > 1)
   {
      float g = gap_hint - (total - avail) / (n - 1);
      if (g < 16.0f) g = 16.0f;
      total -= (gap_hint - g) * (n - 1);
      gap_hint = g;
   }
   x = floorf(g_hint_cx - total * 0.5f);
   y = floorf(y);
   for (i = 0; i < n; i++)
   {
      hint_gl_t *h = &g_hint_gl[set][i];
      if (h->fb) { draw_2d(h->fb, x, y - h->fbh * 0.5f, h->fbw, h->fbh, tc, 0); x += h->fbw + gap_txt; }
      else if (h->have_icons)
      {
         for (k = 0; k < 3 && h->icon[k]; k++)
         {
            float w = floorf(icon_h * h->icon[k]->w / (float)h->icon[k]->h + 0.5f);
            draw_2d(h->icon[k]->tex, x, y - icon_h * 0.5f, w, icon_h, ic, 0);
            x += w + gap_icon;
         }
         x += gap_txt;
      }
      if (h->txt) draw_2d(h->txt, x, floorf(y - h->th * 0.5f), h->tw, h->th, tc, 0);
      x += h->tw + gap_hint;
   }
}

typedef struct { GLuint tex; int w, h; } bgtex_t;
static bgtex_t bg_cur, bg_next;
static float   bg_mix = 1.0f;          /* 0 -> showing cur, 1 -> next done */
static char    bg_shown[512];          /* path of the background we want   */

static void bg_poll_upload(void)
{
   SDL_Surface *s = NULL; char path[512] = "";
   SDL_LockMutex(bg_mx);
   s = bg_done; bg_done = NULL;
   snprintf(path, sizeof(path), "%s", bg_done_path);
   SDL_UnlockMutex(bg_mx);
   if (!s) return;
   if (strcmp(path, bg_shown)) { SDL_FreeSurface(s); return; } /* stale */
   if (bg_cur.tex && bg_mix >= 1.0f) { glDeleteTextures(1, &bg_cur.tex); bg_cur.tex = 0; }
   if (bg_next.tex) { if (bg_cur.tex) glDeleteTextures(1, &bg_cur.tex); bg_cur = bg_next; }
   bg_next.tex = surface_to_tex(s, 0, NULL);
   bg_next.w = s->w; bg_next.h = s->h;
   SDL_FreeSurface(s);
   bg_mix = 0.0f;
}

/* draw a background texture "cover"-fitted with a slow Ken Burns zoom */
static void bg_draw(const bgtex_t *b, float alpha, double t)
{
   float sx, sy, sc, w, h;
   const float c[4] = { 1, 1, 1, alpha };
   if (!b->tex || alpha <= 0.001f) return;
   sx = (float)W / b->w; sy = (float)H / b->h;
   sc = (sx > sy ? sx : sy) * (1.05f + 0.03f * sinf((float)t * 0.07f));
   w = b->w * sc; h = b->h * sc;
   draw_2d(b->tex, (W - w) * 0.5f + 12.0f * sinf((float)t * 0.05f), (H - h) * 0.5f, w, h, c, 0);
}

int main(int argc, char **argv)
{
   char font_b[300], font_r[300], padb[300];
   const char *fonts_bold[] = { font_b, "/usr/share/fonts/ttf/LiberationSans-Bold.ttf", NULL };
   const char *fonts_reg[]  = { font_r, "/usr/share/fonts/ttf/LiberationSans-Regular.ttf", NULL };
   const char *log_path = NULL;
   int start_attract = 0, focus_demo = 0, home_plat = 0, home_sel = 0, att_prev = 0;
   double last_activity = 0;
   attract_out_t att;
   SDL_Window *win; SDL_GLContext ctx;
   SDL_GameController *pad = NULL;
   double t0 = now_s(), last, fps_t, info_t = 0, play_t = -1;
   int i, sel_plat = 0, sel = 0, running = 1, exit_code = 0, frames = 0;
   int held_dir = 0; double held_next = 0, sel_t = 0;
   int start_down = 0, select_down = 0;
   spring bg[3], info_y, info_a, plat_x;
   label_t l_title = { 0 }, l_sub = { 0 }, l_count = { 0 }, l_prof = { 0 }, l_brand = { 0 }, l_start = { 0 }, l_coin1 = { 0 }, l_coin2 = { 0 };
   float fade = 1.0f; /* 1 = black, fades in at start */

   for (i = 1; i < argc; i++)
   {
      if (!strcmp(argv[i], "--root") && i + 1 < argc) snprintf(g_root, sizeof(g_root), "%s", argv[++i]);
      else if (!strcmp(argv[i], "--log") && i + 1 < argc) log_path = argv[++i];
      else if (!strcmp(argv[i], "--home") && i + 1 < argc) snprintf(g_home, sizeof(g_home), "%s", argv[++i]);
      else if (!strcmp(argv[i], "--attract")) start_attract = 1;
      else if (!strcmp(argv[i], "--focus-demo")) focus_demo = 1;
   }
   snprintf(g_core_dir, sizeof(g_core_dir), "%s/cores", g_home);
   snprintf(g_state, sizeof(g_state), "%s/state.txt", g_home);
   snprintf(font_b, sizeof(font_b), "%s/fonts/bold.ttf", g_home);
   snprintf(font_r, sizeof(font_r), "%s/fonts/regular.ttf", g_home);
   snprintf(padb, sizeof(padb), "%s/gamecontrollerdb.txt", g_home);
   if (log_path) g_log = fopen(log_path, "w");
   logf_("lumen %s start", LUMEN_VERSION);

   if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK) != 0)
   { logf_("SDL_Init: %s", SDL_GetError()); return 1; }
   logf_("controller mappings: %d from %s", SDL_GameControllerAddMappingsFromFile(padb), padb);
   SDL_JoystickEventState(SDL_ENABLE);
   SDL_GameControllerEventState(SDL_ENABLE);
   logf_("joysticks at start: %d", SDL_NumJoysticks());
   for (i = 0; i < SDL_NumJoysticks(); i++) in_open(i);
   IMG_Init(IMG_INIT_PNG | IMG_INIT_JPG);
   TTF_Init();
   settings_load(g_home);
   i18n_init(g_home);
   i18n_set_language(settings_get("language", "es"));
   profiles_load(g_home);

   SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
   SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
   SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
   SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
   SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 16);
   {
      int ms = settings_get_int("msaa", 4, 0, 4);
      SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, ms > 0);
      SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, ms);
   }
   win = SDL_CreateWindow("lumen", 0, 0, W, H, SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN_DESKTOP);
   ctx = win ? SDL_GL_CreateContext(win) : NULL;
   if (!ctx)
   {
      logf_("MSAA 4x unavailable (%s), retrying without", SDL_GetError());
      if (win) SDL_DestroyWindow(win);
      SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 0);
      SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 0);
      win = SDL_CreateWindow("lumen", 0, 0, W, H, SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN_DESKTOP);
      if (!win) { logf_("CreateWindow: %s", SDL_GetError()); return 1; }
      ctx = SDL_GL_CreateContext(win);
      if (!ctx) { logf_("GL context: %s", SDL_GetError()); return 1; }
   }
   {
      GLint samples = 0;
      glGetIntegerv(GL_SAMPLES, &samples);
      logf_("MSAA samples: %d", samples);
   }
   SDL_GL_SetSwapInterval(1);
   SDL_ShowCursor(SDL_DISABLE);
   logf_("GL %s | %s", glGetString(GL_VERSION), glGetString(GL_RENDERER));
   gl_setup();
   make_fx_textures();
   bg_mx = SDL_CreateMutex();
   bg_cv = SDL_CreateCond();
   SDL_CreateThread(bg_worker, "bg", NULL);
   cv_mx = SDL_CreateMutex();
   cv_cv = SDL_CreateCond();
   SDL_CreateThread(cover_worker, "covers", NULL);

   g_font_title = open_font(fonts_bold, 34);
   g_font_sub   = open_font(fonts_reg, 20);
   g_font_small = open_font(fonts_reg, 16);
   g_font_cover = open_font(fonts_bold, 22);

   load_systems();
   scan_all();
   build_platform_index();
   logf_("games: %d, platforms: %d, scan %.0f ms", g_ngames, g_nplat, (now_s() - t0) * 1000);
   for (i = 0; i < g_nplat; i++)
   {
      platform_t *p = &g_plat[i];
      if (p->logo[0])
      {
         SDL_Surface *ls = IMG_Load(p->logo);
         if (ls) { p->logo_w = ls->w; p->logo_h = ls->h; p->logo_tex = surface_to_tex(ls, 1, NULL); SDL_FreeSurface(ls); }
         else logf_("[%s] logo not found: %s", p->id, p->logo);
      }
      if (!p->logo_tex) p->show_name = 1;
      if (p->show_name) p->label = text_tex(g_font_small, p->name, &p->label_w, &p->label_h);
   }

   /* resume last selection */
   {
      FILE *f = fopen(g_state, "r");
      if (f) { if (fscanf(f, "%d %d", &sel_plat, &sel) != 2) sel_plat = sel = 0; fclose(f); }
      if (sel_plat < 0 || sel_plat >= g_nplat) sel_plat = 0;
      if (g_nplat && (sel < 0 || sel >= g_plat[sel_plat].count)) sel = 0;
   }
   if (focus_demo)   /* a demo was interrupted by a button: select the demoed game */
   {
      FILE *f = fopen("/tmp/lumen-demo-game", "r");
      char line[600], sys[64] = "", content[512] = "";
      int p, k;
      if (f)
      {
         while (fgets(line, sizeof(line), f))
         {
            line[strcspn(line, "\r\n")] = 0;
            if (!strncmp(line, "system=", 7)) snprintf(sys, sizeof(sys), "%s", line + 7);
            else if (!strncmp(line, "content=", 8)) snprintf(content, sizeof(content), "%s", line + 8);
         }
         fclose(f);
      }
      remove("/tmp/lumen-demo-game");
      for (p = 0; p < g_nplat; p++)
         if (!strcmp(g_plat[p].id, sys))
            for (k = 0; k < g_plat[p].count; k++)
               if (!strcmp(g_games[g_plat[p].first + k].content, content)) { sel_plat = p; sel = k; }
      logf_("focus demo game: %s %s", sys, content);
   }
   home_plat = sel_plat; home_sel = sel;

   g_proj = m4_perspective(38.0f * (float)M_PI / 180.0f, (float)W / H, 0.1f, 50.0f);
   g_view = m4_translate(0, 0.22f, -4.7f);
   g_vp = m4_mul(g_proj, g_view);
   g_ortho = m4_ortho(0, W, H, 0);

   memset(bg, 0, sizeof(bg));
   bg[0].x = 0.05f; bg[1].x = 0.05f; bg[2].x = 0.08f;
   memset(&info_y, 0, sizeof(info_y)); memset(&info_a, 0, sizeof(info_a)); memset(&plat_x, 0, sizeof(plat_x));
   label_set(&l_brand, g_font_small, "lumen");
   hints_load(g_home);
   panel_init();
   attract_init(start_attract, g_home);
   memset(&att, 0, sizeof(att));

   /* first frame positions: start off to the right so they fly in */
   for (i = 0; i < g_ngames; i++)
   {
      game_t *g = &g_games[i];
      g->x.x = 6.0f + (i % 8) * 0.6f; g->z.x = -3.0f; g->ry.x = -1.2f; g->sc.x = 1.0f; g->al.x = 0.0f;
   }

   last = fps_t = now_s();
   logf_("startup %.0f ms, rss %ld kB", (last - t0) * 1000, rss_kb());

   while (running)
   {
      SDL_Event e; double t = now_s(); float dt = (float)(t - last);
      platform_t *pl = g_nplat ? &g_plat[sel_plat] : NULL;
      int n = pl ? pl->count : 0, move = 0, plat_move = 0, play = 0, att_swallow, restore = -1;
      if (dt > 0.05f) dt = 0.05f;
      last = t;

      memset(in_pressed, 0, sizeof(in_pressed));
      while (SDL_PollEvent(&e))
      {
         if (e.type == SDL_QUIT) running = 0;
         else in_event(&e);
      }
      {  /* screensaver / attract scene: any input wakes it and is swallowed (10.2) */
         int was = attract_active();
         if (in_last_activity != last_activity) { last_activity = in_last_activity; attract_note_input(t); }
         attract_update(t, dt, !panel_active() && play_t < 0 && fade < 0.02f && g_ngames > 0, &att);
         att_swallow = was || attract_active() || att.amount > 0.02f;
         if (att_swallow) g_want_settings = 0;
      }
      /* logical actions -> UI */
      {  /* Select tapped alone (pressed and released without Start) opens the settings */
         static int sel_was_down, sel_combo;
         if (in_down[IN_SELECT] && (in_down[IN_START] || att_swallow)) sel_combo = 1;
         if (sel_was_down && !in_down[IN_SELECT])
         {
            if (!sel_combo && play_t < 0) { g_want_settings = 1; logf_("settings requested (Select)"); }
            sel_combo = 0;
         }
         sel_was_down = in_down[IN_SELECT];
      }
      if (!panel_active() && !att_swallow && in_down[IN_SELECT] && in_down[IN_START]) running = 0;
      else
      {
         if (in_pressed[IN_PLAY] || (in_pressed[IN_START] && !in_down[IN_SELECT])) play = 1;
         if (in_pressed[IN_L1] || in_pressed[IN_UP]) plat_move = -1;
         if (in_pressed[IN_R1] || in_pressed[IN_DOWN]) plat_move = 1;
         if (in_pressed[IN_LEFT]) { move = -1; held_next = t + 0.32; }
         if (in_pressed[IN_RIGHT]) { move = 1; held_next = t + 0.32; }
      }
      held_dir = in_down[IN_LEFT] ? -1 : (in_down[IN_RIGHT] ? 1 : 0);
      (void)start_down; (void)select_down; (void)pad;
      {  /* settings panel: owns the input while open or closing (6.3) */
         ui_nav_t nav;
         int was_open = panel_active();
         nav_fill(&nav, t);
         if (!was_open || att_swallow) memset(&nav, 0, sizeof(nav));
         if (!was_open && in_pressed[IN_OPTIONS] && play_t < 0 && !att_swallow) panel_open();
         if (g_want_settings)   /* Select tapped: opens the panel; while open it acts like Triangle */
         {
            g_want_settings = 0;
            if (!was_open && play_t < 0) panel_open();
            else if (was_open) nav.options = 1;
         }
         panel_update(&nav, t, dt);
         if (was_open || panel_active()) { move = plat_move = play = 0; held_dir = 0; }
      }
      if (play_t >= 0) { move = plat_move = play = 0; }
      if (att_swallow) { move = plat_move = play = 0; held_dir = 0; held_next = t + 0.32; }
      if (!move && held_dir && t >= held_next) { move = held_dir; held_next = t + 0.085; }
      {  /* attract: auto-rotation; the user's selection is kept and restored on leave */
         int now_att = attract_active();
         if (now_att) { move = att.move; plat_move = att.plat_move; }
         if (now_att && !att_prev) { home_plat = sel_plat; home_sel = sel; }
         if (!now_att && att_prev && g_nplat)
         {
            restore = home_plat < g_nplat ? home_plat : 0;
            if (restore == sel_plat)
            {
               if (n && sel != home_sel) { sel = home_sel < n ? home_sel : 0; info_y.x = 18.0f; info_a.x = 0.0f; sel_t = t; }
               restore = -1;
            }
            else plat_move = restore > sel_plat ? 1 : -1;
         }
         att_prev = now_att;
         if (att.launch_demo && play_t < 0) { exit_code = 11; running = 0; }
      }

      if (n && move) { sel = (sel + move + n) % n; info_y.x = 18.0f; info_a.x = 0.0f; sel_t = t; }
      if (g_nplat > 1 && plat_move)
      {
         int np = restore >= 0 ? restore : (sel_plat + plat_move + g_nplat) % g_nplat, k;
         /* new platform boxes fly in from the side of travel */
         for (k = 0; k < g_plat[np].count; k++)
         {
            game_t *g = &g_games[g_plat[np].first + k];
            g->x.x = plat_move * (5.0f + k * 0.4f); g->x.v = 0; g->z.x = -2.5f; g->ry.x = -plat_move * 1.2f; g->al.x = 0;
         }
         sel_plat = np; sel = 0; pl = &g_plat[sel_plat]; n = pl->count; sel_t = t;
         if (restore >= 0 && home_sel < n) sel = home_sel;
         info_y.x = 18.0f; info_a.x = 0.0f; plat_x.v = plat_move * 900.0f;
      }
      if (n && play && play_t < 0)
      {
         game_t *pg = &g_games[pl->first + sel];
         play_t = t;
         logf_("play: %s", pg->content);
         if (g_disc_tex) { glDeleteTextures(1, &g_disc_tex); g_disc_tex = 0; }
         if (pg->disc[0])
         {
            SDL_Surface *ds = IMG_Load(pg->disc);
            if (ds) { g_disc_tex = surface_to_tex(ds, 1, NULL); SDL_FreeSurface(ds); }
         }
      }

      /* background: ask the worker once the selection has settled */
      if (n && t - sel_t > 0.25)
      {
         const char *want = g_games[pl->first + sel].bg;
         if (strcmp(want, bg_shown))
         {
            snprintf(bg_shown, sizeof(bg_shown), "%s", want);
            if (want[0]) bg_request(want);
            else if (bg_next.tex) { if (bg_cur.tex) glDeleteTextures(1, &bg_cur.tex); bg_cur = bg_next; bg_next.tex = 0; bg_mix = 0.0f; }
         }
      }
      bg_poll_upload();
      bg_mix = bg_mix + dt * 1.4f > 1.0f ? 1.0f : bg_mix + dt * 1.4f;

      /* cover streaming: decoded on the worker, nearest to the selection first */
      {
         int done = -1, k;
         SDL_Surface *cs = NULL; float avg[3];
         SDL_LockMutex(cv_mx);
         if (cv_done >= 0) { done = cv_done; cs = cv_surf; memcpy(avg, cv_avg, sizeof(avg)); cv_done = -1; cv_surf = NULL; cv_busy = 0; }
         SDL_UnlockMutex(cv_mx);
         if (done >= 0)
         {
            game_finish_cover(&g_games[done], cs, avg);
            if (cs) SDL_FreeSurface(cs);
         }
         if (n)
         {
            int placed = 0;
            for (k = 0; k <= TEX_WINDOW && !placed; k++)
            {
               int cand[2], c;
               cand[0] = (sel + k) % n; cand[1] = (sel - k + n) % n;
               for (c = 0; c < 2 && !placed; c++)
               {
                  int gi = pl->first + cand[c];
                  game_t *g = &g_games[gi];
                  if (g->tex_state) continue;
                  if (g->has_cover) { if (!cv_busy) { g->tex_state = 2; cover_request(gi, g->cover); } placed = 1; }
                  else { game_placeholder(g); placed = 1; }
               }
            }
            for (k = 0; k < g_ngames; k++)
            {
               game_t *g = &g_games[k];
               int d = 9999;
               if (g->platform == g_games[pl->first].platform)
               {
                  int o = abs((k - pl->first) - sel);
                  d = o < n - o ? o : n - o;
               }
               if ((g->tex_state == 1 || g->tex_state == -1) && d > TEX_WINDOW + 3) game_free_tex(g);
            }
            /* back card only for the settled (or launching) selection; never in the attract
             * scene (it moves every few seconds and never shows the back) nor under the
             * panel: it is built once the user is back on the carousel */
            {
               game_t *cg = &g_games[pl->first + sel];
               if ((cg->tex_state == 1 || cg->tex_state == -1) && !cg->back_tex &&
                   ((t - sel_t > 0.2 && att.amount < 0.001f && !panel_active()) || play_t >= 0))
                  cg->back_tex = card_tex(cg, 1);
            }
         }
      }

      /* animation targets */
      {
         float zoom = 0.0f, open = 0.0f, lift = 0.0f, fly = 0.0f, spin = 0.0f, flash = 0.0f, launching = 0.0f;
         float drop = 0.0f, tilt = 0.0f, shake = 0.0f, led = 0.0f, slot = 0.0f, lit = 1.0f;
         float coin_x = 0.0f, coin_spin = 0.0f, coin_in = 0.0f, credit = 0.0f, insert = 0.0f;
         int anim = pl ? pl->launch : ANIM_OPEN_CASE_DISC;
         if (play_t >= 0)
         {
            float pt = (float)(t - play_t), u, end = 2.8f, fl_t = 2.35f;
#define SMOOTH(x) ((x) * (x) * (3 - 2 * (x)))
            launching = 1.0f;
            zoom = SMOOTH(clampf(pt / 0.5f, 0, 1));
            if (anim == ANIM_INSERT_CART)
            {
               u = clampf((pt - 0.45f) / 0.30f, 0, 1); lift = SMOOTH(u);
               u = clampf((pt - 0.75f) / 0.40f, 0, 1); drop = u * u;
               tilt = lift * 0.35f;
               slot = clampf((pt - 0.40f) / 0.35f, 0, 1);
               if (pt > 1.15f) shake = expf(-(pt - 1.15f) * 9.0f) * sinf((pt - 1.15f) * 70.0f);
               led = clampf((pt - 1.22f) / 0.12f, 0, 1);
               fl_t = 1.60f; end = 2.05f;
            }
            else if (anim == ANIM_INSERT_COIN)
            {
               insert = pt < 1.35f ? 1.0f : 0.0f;
               u = clampf((pt - 0.40f) / 0.75f, 0, 1); coin_x = 1.0f - SMOOTH(u);
               coin_spin = SMOOTH(u) * 9.0f;
               coin_in = SMOOTH(clampf((pt - 1.15f) / 0.20f, 0, 1));
               lit = 0.55f;
               if (pt > 1.35f)
               {
                  float k = pt - 1.35f;
                  lit = k < 0.45f ? (fmodf(k * 23.0f, 1.0f) > 0.45f ? 1.25f : 0.35f) : 1.25f;
                  credit = fmodf(k * 3.0f, 1.0f) > 0.35f ? 1.0f : 0.0f;
               }
               fl_t = 2.15f; end = 2.6f;
            }
            else
            {
               u = clampf((pt - 0.45f) / 0.65f, 0, 1); open = SMOOTH(u);
               u = clampf((pt - 1.05f) / 0.35f, 0, 1); lift = SMOOTH(u);
               fly = clampf((pt - 1.40f) / 0.75f, 0, 1);
               {  /* angular velocity: ramps up out of the tray, cruises while flying,
                   * eases to a calm spin once centred (fast spins strobe at 60 Hz) */
                  static float disc_spin;
                  float w;
                  if (pt < 1.05f) disc_spin = 0.0f;
                  w = pt < 1.05f ? 0.0f : pt < 1.45f ? 7.0f * (pt - 1.05f) / 0.40f :
                      pt < 1.95f ? 7.0f : lerpf(7.0f, 1.8f, clampf((pt - 1.95f) / 0.35f, 0, 1));
                  disc_spin += w * dt;
                  spin = disc_spin;
               }
            }
            flash = clampf(1.0f - fabsf(pt - fl_t) / 0.22f, 0, 1);
            fade = clampf((pt - fl_t) / 0.30f, 0, 1);
            if (pt > end) { exit_code = 10; running = 0; }
#undef SMOOTH
         }
         else
         {
            fade = fade > 0 ? fade - dt * 1.6f : 0;
            if (pl && pl->kind == KIND_ARCADE) lit = 1.0f + 0.04f * sinf((float)t * 3.0f);
         }
         g_vp = m4_mul(g_proj, m4_mul(m4_translate(shake * 0.03f, shake * 0.02f, 0), g_view));

         for (i = 0; i < n; i++)
         {
            game_t *g = &g_games[pl->first + i];
            int o = i - sel, a;
            float tx, tz, tr, ts, ta;
            if (o > n / 2) o -= n;
            if (o < -n / 2) o += n;
            a = abs(o);
            tx = o == 0 ? 0.0f : (o > 0 ? 1 : -1) * (1.08f + (a - 1) * 0.50f + launching * 2.2f);
            tz = o == 0 ? 0.55f + zoom * 0.55f : -0.35f - a * 0.22f - launching * 1.2f;
            tr = o == 0 ? 0.0f : -(o > 0 ? 1 : -1) * 1.0f;
            ts = o == 0 ? 1.10f : 0.92f;
            ta = a > VISIBLE ? 0.0f : 1.0f - a * 0.14f;
            if (launching && o != 0) ta = 0.0f;
            if (att.amount > 0.001f)
            {
               float adx, ady, adry;
               attract_case_offset(o, t, &adx, &ady, &adry);
               tx += adx; tr += adry;
            }
            spring_step(&g->x, tx, 140, 15, dt);
            spring_step(&g->z, tz, 140, 15, dt);
            spring_step(&g->ry, tr, 120, 14, dt);
            spring_step(&g->sc, ts, 160, 16, dt);
            spring_step(&g->al, ta, 90, 14, dt);
         }

         /* ambient colour from the selected cover */
         if (n)
         {
            game_t *cg = &g_games[pl->first + sel];
            float base[3] = { 0.050f, 0.050f, 0.078f }, want[3];
            for (i = 0; i < 3; i++) want[i] = base[i] + cg->col[i] * 0.16f;
            if (att.amount > 0.001f) attract_bg_target(want, t, want);
            for (i = 0; i < 3; i++) spring_step(&bg[i], want[i], 6, 5, dt);
         }
         spring_step(&info_y, 0, 170, 17, dt);
         spring_step(&info_a, 1, 60, 12, dt);
         spring_step(&plat_x, 0, 120, 16, dt);

         /* ---- render ---- */
         glViewport(0, 0, W, H);
         glClearColor(bg[0].x, bg[1].x, bg[2].x, 1);
         glClearDepthf(1.0f);
         glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
         glEnable(GL_BLEND);
         glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

         glUseProgram(g_prog_2d);
         glUniform1i(u2_tex, 0);
         {
            float ba = BG_OPACITY * (1.0f - fade), e = bg_mix * bg_mix * (3 - 2 * bg_mix);
            bg_draw(&bg_cur, ba * (1.0f - e), t);
            bg_draw(&bg_next, ba * e, t);
         }
         {
            const float vig[4] = { 1, 1, 1, 1 };
            float spot[4] = { 0.55f + bg[0].x, 0.55f + bg[1].x, 0.62f + bg[2].x, 0.10f * (1.0f - fade) };
            if (g_fx_vignette)
            {  /* baked masks: two texture fetches instead of per-pixel maths */
               draw_2d(g_vig_tex, 0, 0, W, H, vig, 0);
               draw_2d(g_spot_tex, 0, 0, W, H, spot, 0);
            }
         }

         glUseProgram(g_prog_box);
         glUniform1i(u_tex, 0);
         {
            const float light[3] = { -0.5f, 0.7f, 1.0f }, cam[3] = { 0.0f, -0.22f, 4.7f };
            glUniform3fv(u_light, 1, light);
            glUniform3fv(u_cam, 1, cam);
         }
         glUniform1f(u_refl, 100.0f);   /* reflections off */
         glEnable(GL_DEPTH_TEST);
         glDepthFunc(GL_LEQUAL);
         for (i = VISIBLE; i >= 0 && n; i--)
         {
            int s;
            for (s = -1; s <= 1; s += 2)
            {
               int idx, o = i * s;
               game_t *g;
               mat4 base;
               float bob = 0.0f, sway = 0.0f, op = 0.0f, ty = 0.0f, tx_ = 0.0f;
               const platform_t *gp;
               if (i == 0 && s == 1) continue;
               if (!g_fx_sides && o != 0) continue;
               idx = ((sel + o) % n + n) % n;
               g = &g_games[pl->first + idx];
               if (g->al.x < 0.01f || !g->tex) continue;
               gp = &g_plat[g->platform];
               {  /* skip cases whose centre projects outside the screen (with margin) */
                  const float *v = g_vp.m;
                  float cx = v[0] * g->x.x + v[8] * g->z.x + v[12], cw = v[3] * g->x.x + v[11] * g->z.x + v[15];
                  if (cw <= 0.0f || fabsf(cx / cw) > 1.25f) continue;
               }
               g_detail = abs(o) <= 1;
               if (att.amount > 0.001f)
               {
                  float adx, ady, adry;
                  attract_case_offset(o, t, &adx, &ady, &adry);
                  bob += ady;
               }
               if (o == 0)
               {
                  if (gp->idle_float)
                  {
                     bob += sinf((float)t * 1.9f) * 0.035f;
                     sway = -0.17f + sinf((float)t * 1.9f + 0.6f) * 0.06f;
                  }
                  if (play_t >= 0)
                  {
                     op = open; sway *= 1.0f - zoom; bob *= 1.0f - zoom;
                     ty = lift * 0.35f - drop * 3.2f - zoom * 0.22f;   /* zoom towards the camera axis */
                     tx_ = tilt;
                  }
               }
               if (o == 0 && g_fx_refl)
               {
                  /* true reflection: the whole case mirrored about the floor, same motion */
                  float floor_y = -0.5f * gp->h * g->sc.x - 0.01f;
                  mat4 rb = m4_mul(m4_translate(g->x.x, bob + ty, g->z.x),
                            m4_mul(m4_rot_y(g->ry.x + sway), m4_mul(m4_rot_x(-tx_), m4_scale(g->sc.x, g->sc.x, g->sc.x))));
                  mat4 mir = m4_mul(m4_translate(0, 2.0f * floor_y, 0), m4_mul(m4_scale(1, -1, 1), rb));
                  glDisable(GL_DEPTH_TEST);
                  glUniform1f(u_refl, floor_y);
                  draw_media(g, gp, mir, g->al.x * (1.0f - fade) * (1.0f - zoom), op, gp->kind == KIND_DISC ? lift : 0.0f, 0.0f, spin, lit);
                  glUniform1f(u_refl, 100.0f);
                  glEnable(GL_DEPTH_TEST);
               }
               base = m4_mul(m4_translate(g->x.x, bob + ty, g->z.x),
                      m4_mul(m4_rot_y(g->ry.x + sway), m4_mul(m4_rot_x(-tx_), m4_scale(g->sc.x, g->sc.x, g->sc.x))));
               draw_media(g, gp, base, g->al.x, op, o == 0 && gp->kind == KIND_DISC ? lift : 0.0f,
                          o == 0 ? fly : 0.0f, o == 0 ? spin : 0.0f, o == 0 ? lit : 1.0f);
               g_detail = 1;
            }
         }
         if (play_t >= 0 && anim == ANIM_OPEN_CASE_DISC && fly > 0.0f)
         {
            glDisable(GL_DEPTH_TEST);
            draw_flying_disc(fly, spin, 1.0f);
            glEnable(GL_DEPTH_TEST);
         }
         if (play_t >= 0 && anim == ANIM_INSERT_COIN && pl)
         {
            static const float one[3] = { 1, 1, 1 }, slotc[3] = { 0.02f, 0.02f, 0.025f }, plate[3] = { 0.30f, 0.30f, 0.34f };
            float sy = -0.5f * pl->h * 1.10f - 0.42f;
            mat4 m;
            m = m4_mul(m4_translate(0, sy, 1.15f), m4_scale(0.62f, 0.30f, 1));
            draw_quad_box(m, 0, plate, 1.0f - fade, 1);
            m = m4_mul(m4_translate(0, sy, 1.16f), m4_scale(0.30f, 0.035f, 1));
            draw_quad_box(m, 0, slotc, 1.0f - fade, 1);
            if (coin_in < 1.0f)
            {
               m = m4_mul(m4_translate(coin_x * 1.9f, sy + 0.16f - coin_in * 0.16f, 1.22f),
                   m4_mul(m4_rot_y(coin_in * 1.45f), m4_mul(m4_rot_z(-coin_spin), m4_scale(0.24f, 0.24f, 1))));
               draw_quad_box(m, 0, one, 1.0f - fade, 9);
            }
         }
         glDisable(GL_DEPTH_TEST);

         /* ---- 2D overlay ---- */
         glUseProgram(g_prog_2d);
         if (g_fx_text)
         {
            const float pa = panel_amount(), ba = (1 - fade) * (1 - 0.6f * pa);   /* dimmed under the panel */
            const float ua = ba * (1 - att.amount);                              /* and gone in the attract scene */
            const float brand[4] = { 0.93f, 0.92f, 0.97f, ba };
            const float txt[4] = { 0.93f, 0.92f, 0.97f, ua };
            const float dim[4] = { 0.55f, 0.54f, 0.64f, ua };
            const float pill_on[4] = { 0.93f, 0.93f, 0.99f, ua };
            const float pill_off[4] = { 0.25f, 0.25f, 0.32f, 0.6f * ua };
            const float txt_dark[4] = { 0.15f, 0.13f, 0.36f, ua };
            float px, total = 0;
            char s[256];
            game_t *cg = n ? &g_games[pl->first + sel] : NULL;

            if (att.dim > 0.003f)
            {
               const float dimq[4] = { 0, 0, 0, att.dim };
               draw_2d(0, 0, 0, W, H, dimq, 0);
            }
            draw_2d(l_brand.tex, 36, 26, l_brand.w, l_brand.h, brand, 0);
            if (pl)
            {
               char cnt[16];
               snprintf(cnt, sizeof(cnt), "%d", pl->count);
               label_set(&l_count, g_font_small,
                         trf(s, sizeof(s), pl->count == 1 ? "top.games_one" : "top.games", "n", cnt, "system", pl->name, NULL));
            }
            {
               /* profile chip (swatch + name) at the right edge, game count to its left */
               const profile_t *ap = profiles_active();
               float sw[4], chip_x, cnt_x;
               label_set(&l_prof, g_font_small, profiles_display_name(ap));
               chip_x = W - 36 - l_prof.w;
               draw_2d(l_prof.tex, chip_x, 26, l_prof.w, l_prof.h, txt, 0);
               profiles_color(ap->color, sw); sw[3] = ua;
               draw_round_rect(chip_x - 8 - 14, 26 + floorf((l_prof.h - 14) * 0.5f), 14, 14, 7, sw, 0.0f);
               cnt_x = chip_x - 8 - 14 - 16 - l_count.w;
               draw_2d(l_count.tex, cnt_x, 26, l_count.w, l_count.h, dim, 0);
            }

#define LOGO_H 22.0f
#define PILL_CONTENT(p) (((p)->logo_tex ? LOGO_H * (p)->logo_w / (float)(p)->logo_h : 0.0f) + \
                         ((p)->logo_tex && (p)->show_name ? 8.0f : 0.0f) + ((p)->show_name ? (p)->label_w : 0.0f))
            for (i = 0; i < g_nplat; i++) total += PILL_CONTENT(&g_plat[i]) + 36 + 10;
            px = (W - total) * 0.5f + plat_x.x * 0.02f;
            for (i = 0; i < g_nplat; i++)
            {
               platform_t *p = &g_plat[i];
               float pw = PILL_CONTENT(p) + 36, ph = 32, cx = px + 18;
               const float *fg = i == sel_plat ? txt_dark : txt;
               draw_2d(0, px, 64, pw, ph, i == sel_plat ? pill_on : pill_off, 1);
               if (p->logo_tex)
               {
                  float lw = LOGO_H * p->logo_w / (float)p->logo_h;
                  draw_2d(p->logo_tex, cx, 64 + (ph - LOGO_H) * 0.5f, lw, LOGO_H, fg, 0);
                  cx += lw + 8.0f;
               }
               if (p->show_name)
                  draw_2d(p->label, cx, 64 + (ph - p->label_h) * 0.5f, p->label_w, p->label_h, fg, 0);
               px += pw + 10;
            }
#undef PILL_CONTENT

            if (cg)
            {
               float a = info_a.x * ua;
               float c1[4] = { 0.93f, 0.92f, 0.97f, a }, c2[4] = { 0.55f, 0.54f, 0.64f, a };
               label_set(&l_title, g_font_title, cg->title);
               snprintf(s, sizeof(s), "%s%s%s", cg->year, cg->year[0] && cg->publisher[0] ? "  \xC2\xB7  " : "", cg->publisher);
               label_set(&l_sub, g_font_sub, s);
               draw_2d(l_title.tex, (W - l_title.w) * 0.5f, 588 + info_y.x, l_title.w, l_title.h, c1, 0);
               draw_2d(l_sub.tex, (W - l_sub.w) * 0.5f, 630 + info_y.x * 0.7f, l_sub.w, l_sub.h, c2, 0);
            }
            else
            {
               label_set(&l_title, g_font_title, tr("empty.library"));
               draw_2d(l_title.tex, (W - l_title.w) * 0.5f, 330, l_title.w, l_title.h, txt, 0);
            }
            if (pa > 0.003f)
            {
               const float back[4] = { 0, 0, 0, 0.45f * pa };
               draw_2d(0, 0, 0, W, H, back, 0);
            }
            {  /* help bar: the set follows the panel, crossfading over 0.15 s; while the
                * panel is open it is centred in the free space left of it */
               static int h_cur = HINTS_CAROUSEL, h_prev = -1;
               static float h_mix = 1.0f;
               int want = panel_active() ? panel_hint_set() : (attract_active() ? HINTS_ATTRACT : HINTS_CAROUSEL);
               const float ha = (1 - fade) * (1 - att.amount);
               if (want != h_cur) { h_prev = h_cur; h_cur = want; h_mix = 0.0f; }
               h_mix = clampf(h_mix + dt / 0.15f, 0.0f, 1.0f);
               g_hint_cx = lerpf(W * 0.5f, (W - 24.0f - 520.0f) * 0.5f, pa);
               if (h_prev >= 0 && h_mix < 1.0f) hints_draw(h_prev, 686, ha * (1.0f - h_mix));
               hints_draw(h_cur, 686, ha * h_mix);
            }
            if (play_t >= 0 && anim == ANIM_INSERT_CART && slot > 0.0f)
            {
               const float sc[4] = { 0.02f, 0.02f, 0.03f, 0.92f * slot * (1 - fade) };
               const float rim[4] = { 0.35f, 0.34f, 0.42f, 0.8f * slot * (1 - fade) };
               const float ledc[4] = { 1.0f, 0.18f, 0.12f, led * (1 - fade) };
               const float ledg[4] = { 1.0f, 0.25f, 0.15f, 0.55f * led * (1 - fade) };
               draw_2d(0, W * 0.5f - 232, 598, 464, 34, rim, 1);
               draw_2d(0, W * 0.5f - 220, 604, 440, 22, sc, 1);
               draw_2d(0, W * 0.5f + 262, 606, 18, 18, ledc, 1);
               draw_2d(0, W * 0.5f + 271 - 60, 615 - 60, 120, 120, ledg, 3);
            }
            if (play_t >= 0 && anim == ANIM_INSERT_COIN)
            {
               const float yc[4] = { 1.0f, 0.82f, 0.30f, (1 - fade) * (insert ? (fmodf((float)t * 2.5f, 1.0f) > 0.4f ? 1.0f : 0.25f) : credit) };
               label_t *lb = insert ? &l_coin1 : &l_coin2;
               label_set(lb, g_font_sub, tr(insert ? "coin.insert" : "coin.credit"));
               draw_2d(lb->tex, (W - lb->w) * 0.5f, 640, lb->w, lb->h, yc, 0);
            }
            panel_draw(t);
            panel_toast_draw(t);
            glUseProgram(g_prog_2d);
            attract_draw(t);
            if (flash > 0.001f)
            {
               const float white[4] = { 1, 1, 1, flash * 0.85f };
               draw_2d(0, 0, 0, W, H, white, 0);
            }
            if (fade > 0.001f)
            {
               const float black[4] = { 0, 0, 0, fade };
               draw_2d(0, 0, 0, W, H, black, 0);
               if (play_t >= 0)
               {
                  const float st[4] = { 0.93f, 0.92f, 0.97f, fade };
                  label_set(&l_start, g_font_sub, tr("launch.starting"));
                  draw_2d(l_start.tex, (W - l_start.w) * 0.5f, (H - l_start.h) * 0.5f, l_start.w, l_start.h, st, 0);
               }
            }
         }
      }

      SDL_GL_SwapWindow(win);
      frames++;
      {  /* bench: <home>/bench present -> measure with one effect off at a time */
         static int bench = -1, step; static double bt, bframes;
         static const char *names[] = { "all", "no-vignette", "no-reflection", "no-glass", "no-side-cases", "no-text", "bare" };
         if (bench < 0)
         {
            char bp[300]; snprintf(bp, sizeof(bp), "%s/bench", g_home);
            bench = file_exists(bp) ? 1 : 0;
            if (bench) { remove(bp); bt = t + 3.0; step = -1; logf_("bench: start (MSAA %d)", settings_get_int("msaa", 4, 0, 4)); }
         }
         if (bench == 1 && t >= bt)
         {
            if (step >= 0) logf_("bench %-14s %.1f fps", names[step], bframes / 2.5);
            step++;
            if (step >= (int)(sizeof(names) / sizeof(names[0]))) { bench = 2; g_fx_vignette = g_fx_refl = g_fx_glass = g_fx_sides = g_fx_text = 1; logf_("bench: done"); }
            else
            {
               g_fx_vignette = step != 1 && step != 6; g_fx_refl = step != 2 && step != 6; g_fx_glass = step != 3 && step != 6;
               g_fx_sides = step != 4 && step != 6;    g_fx_text = step != 5 && step != 6;
               bt = t + 2.5; bframes = 0;
            }
         }
         if (bench == 1 && step >= 0) bframes += 1;
      }
      if (t - fps_t >= 5.0)
      {
         logf_("fps %.1f, rss %ld kB", frames / (t - fps_t), rss_kb());
         frames = 0; fps_t = t;
      }
      (void)info_t;
   }

   /* persist settings and selection; hand the launch request to the launcher script.
    * In the attract scene the selection saved is the user's, from before it started (10.5). */
   panel_shutdown();   /* a colour picked or settings changed with the panel still open */
   if (settings_dirty()) settings_save();
   {
      FILE *f = fopen(g_state, "w");
      if (f) { fprintf(f, "%d %d\n", att_prev ? home_plat : sel_plat, att_prev ? home_sel : sel); fclose(f); }
   }
   if (exit_code == 11 && attract_demo_game() < 0) exit_code = 1;
   if ((exit_code == 10 || exit_code == 11) && g_nplat)
   {
      const int demo = exit_code == 11;
      game_t *g = demo ? &g_games[attract_demo_game()] : &g_games[g_plat[sel_plat].first + sel];
      const platform_t *lp = &g_plat[g->platform];
      char core[400];
      launch_req_t req;
      memset(&req, 0, sizeof(req));
      snprintf(core, sizeof(core), "%s/%s", g_core_dir, lp->core);
      req.core = core; req.content = g->content; req.system = lp->id; req.title = g->title;
      req.profile = profiles_active()->id;
      req.demo = demo;
      req.demo_seconds = demo && lp->demo_seconds > 0 ? lp->demo_seconds : settings_get_int("demo_seconds", 60, 20, 300);
      if (lp->has_gfx)
      {
         char gkey[48];
         int enh;
         snprintf(gkey, sizeof(gkey), "graphics.%s", lp->id);
         enh = !strcmp(settings_get(gkey, "original"), "enhanced");
         req.graphics = enh ? "enhanced" : "original";
         req.opt_core_name = lp->gfx_core;
         req.opt_list = enh ? lp->gfx_enh : lp->gfx_orig;
         req.ra_list = enh ? lp->gfx_ra_enh : lp->gfx_ra_orig;
      }
      if (demo)
      {  /* --focus-demo reads it back when a button interrupts the demo */
         FILE *f = fopen("/tmp/lumen-demo-game", "w");
         if (f) { fprintf(f, "system=%s\ncontent=%s\n", lp->id, g->content); fclose(f); }
      }
      if (launch_write("/tmp/lumen-launch", &req) == 0)
         logf_("launch request: %s (%s %ds, profile %s, graphics %s)", g->content, demo ? "demo" : "normal",
               req.demo_seconds, req.profile, req.graphics ? req.graphics : "-");
      else { logf_("launch request refused, not launching"); exit_code = 1; }
   }
   SDL_LockMutex(bg_mx); bg_quit = 1; SDL_CondSignal(bg_cv); SDL_UnlockMutex(bg_mx);
   SDL_LockMutex(cv_mx); cv_quit = 1; SDL_CondSignal(cv_cv); SDL_UnlockMutex(cv_mx);
   logf_("exit %d", exit_code);
   SDL_GL_DeleteContext(ctx);
   SDL_DestroyWindow(win);
   TTF_Quit(); IMG_Quit(); SDL_Quit();
   return exit_code;
}
