/*
 * Lumen — game frontend for the PlayStation Classic.
 * SDL2 2.0.4 + OpenGL ES 3.0 on Weston/Wayland, Cortex-A35, 1 GB RAM.
 *
 * Scope:
 *   - one library per declared system under /media/games/<system>/ (see docs/systems.md)
 *   - flat PS5-style dashboard (docs/dashboard-design.md): full-screen game art (or a
 *     background generated from the cover colours), system tabs, a row of rounded cover
 *     tiles, title + play statistics, profile / controllers / network / clock status,
 *     per-game music (src/music.c)
 *   - "play": the 3D case of the game opens, its disc flies out, fades to black, then the
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
#include <unistd.h>
#include "lumen.h"
#include "i18n.h"
#include "settings.h"
#include "panel.h"
#include "profiles.h"
#include "launch.h"
#include "attract.h"
#include "music.h"
#include "welcome.h"
#include "status.h"
#include "stats.h"
#include "version.h"

#define TEX_WINDOW 6       /* covers kept on the GPU around the selection */
#define VISIBLE 4          /* boxes drawn on each side */

/* ------------------------------------------------------------------------ */
/* logging                                                                   */
/* The log lives on the USB stick (exFAT through FUSE): a file's size is only committed
 * on close/fsync, so a power-off would leave the log of the running frontend empty.
 * Every line is flushed; fsync runs at most once per second (log_tick() catches up on
 * pending lines), immediately for lines that report an error, and at exit. Lines can
 * come from the worker threads, so the stream lock guards the bookkeeping. */
static FILE  *g_log;
static double g_log_synced = -10.0;   /* monotonic time of the last fsync */
static int    g_log_pending;          /* lines written since that fsync */

static double log_clock(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* call with the stream locked */
static void log_sync_locked(int force)
{
   double t;
   if (!g_log || g_log == stderr || !g_log_pending) return;
   t = log_clock();
   if (!force && t - g_log_synced < 1.0) return;
   fsync(fileno(g_log));
   g_log_synced = t; g_log_pending = 0;
}

static int log_is_error(const char *s)
{
   for (; *s; s++)
      if (!strncasecmp(s, "error", 5) || !strncasecmp(s, "fail", 4)) return 1;
   return 0;
}

void logf_(const char *fmt, ...)
{
   va_list ap;
   char line[1024];
   if (!g_log) g_log = stderr;
   va_start(ap, fmt); vsnprintf(line, sizeof(line), fmt, ap); va_end(ap);
   flockfile(g_log);
   fputs(line, g_log); fputc('\n', g_log); fflush(g_log);
   g_log_pending = 1;
   log_sync_locked(log_is_error(line));
   funlockfile(g_log);
}

/* once per frame: fsync lines left pending by the 1 s limit */
static void log_tick(void)
{
   if (!g_log_pending || !g_log) return;
   flockfile(g_log); log_sync_locked(0); funlockfile(g_log);
}

static void log_sync_atexit(void)
{
   if (!g_log) return;
   flockfile(g_log); fflush(g_log); log_sync_locked(1); funlockfile(g_log);
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
/* The UI is laid out in a 1280x720 logical space (W x H); the window may be larger
 * (display=1080p: 1920x1080). g_ui_scale = device px per logical px: fonts and text
 * textures are rasterised at device size, so text stays sharp at any output. */
float g_ui_scale = 1.0f;
static int g_dw = W, g_dh = H;      /* drawable size in device pixels */
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

/* Runtime assets live under <home>/assets/ (assets/README.md); installs made before that
 * layout keep them directly under <home>/. out = the first that exists, else the new path. */
static void asset_path(char *out, size_t n, const char *rel_new, const char *rel_old)
{
   snprintf(out, n, "%s/assets/%s", g_home, rel_new);
   if (file_exists(out)) return;
   snprintf(out, n, "%s/%s", g_home, rel_old);
   if (file_exists(out)) return;
   snprintf(out, n, "%s/assets/%s", g_home, rel_new);
}

/* per-system art named by a declaration: "<file>" is the system's own
 * (assets/systems/<id>/<file>), "<other id>/<file>" shares another system's */
static void system_asset(char *out, size_t n, const char *id, const char *file)
{
   char rn[200], ro[200];
   if (strchr(file, '/')) { snprintf(rn, sizeof(rn), "systems/%s", file); snprintf(ro, sizeof(ro), "systems/%s", file); }
   else { snprintf(rn, sizeof(rn), "systems/%s/%s", id, file); snprintf(ro, sizeof(ro), "systems/%s/%s", id, file); }
   asset_path(out, n, rn, ro);
}

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
   if (v[0]) system_asset(pl->logo, sizeof(pl->logo), pl->id, v);
   decl_get(ini, "system", "order", v, sizeof(v), "50");   pl->order = atoi(v);
   decl_get(ini, "system", "show_name", v, sizeof(v), "1"); pl->show_name = atoi(v);
   decl_get(ini, "library", "layout", pl->layout, sizeof(pl->layout), "files");
   decl_get(ini, "library", "paths", pl->paths, sizeof(pl->paths), "");
   if (!pl->paths[0] && !strcmp(pl->layout, "stock")) snprintf(pl->paths, sizeof(pl->paths), "/gaadata");
   if (!pl->paths[0]) snprintf(pl->paths, sizeof(pl->paths), "%s/games/%s", g_root, pl->id);  /* convention */
   decl_get(ini, "library", "extensions", pl->exts, sizeof(pl->exts), !strcmp(pl->layout, "stock") ? "cue,bin" : "");
   decl_get(ini, "library", "metadata", v, sizeof(v), "");
   if (!v[0]) snprintf(v, sizeof(v), "%s.games", pl->id);
   snprintf(pl->meta, sizeof(pl->meta), "%s/systems/%s", g_home, v);   /* next to the declaration */
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
   if (v[0]) system_asset(pl->sound, sizeof(pl->sound), pl->id, v);
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
   int ndecl = 0;
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
         if (buf && fread(buf, 1, sz, f) == (size_t)sz)
         {
            platform_t *pl = &g_plat[g_nplat];
            platform_from_decl(pl, buf);
            ndecl++;
            /* the console's built-in games are listed only when the settings say so */
            if (!strcmp(pl->layout, "stock") && !settings_get_int("show_stock_games", 0, 0, 1))
               logf_("[%s] built-in games hidden (show_stock_games=0)", pl->id);
            else g_nplat++;
         }
         free(buf); fclose(f);
      }
      closedir(d);
   }
   if (!ndecl)  /* built-in default: PlayStation library in the PSC layout */
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
   static const char *const music_sfx[] = { "music.ogg", "music.wav", NULL };
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
      find_first(g->music, sizeof(g->music), base, music_sfx);
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
   static const char *const music_sfx[] = { "-music.ogg", "-music.wav", NULL };
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
      find_first(g->music, sizeof(g->music), base, music_sfx);
      /* title: file name without "(...)" / "[...]" tags */
      snprintf(g->title, sizeof(g->title), "%s", stem);
      if ((p = strpbrk(g->title, "([")) && p > g->title) *p = 0;
      trim(g->title);
      g->platform = pi;
      g_ngames++;
   }
   closedir(d);
}

/* Stock layout: the console's built-in games, read-only (never written).
 * <path>/<id>/<BASENAME>.cue|bin per game; the metadata cannot be read from the
 * console's own database (no sqlite on the console), so it comes from a shipped file
 * next to the declaration: "id|title|publisher|year|players|basename1,basename2" lines,
 * '#' comments. Disc 1 = the first basename. Cover: <BASENAME>.png, else the first
 * *.png of the folder. The folder listings are logged on the first scan only. */
/* first existing <folder>/<base>.<ext> for the declared extensions, "" if none */
static void stock_find_disc(const char *exts_decl, const char *folder, const char *base, char *out, size_t n)
{
   char exts[160], *tok, *save = NULL;
   out[0] = 0;
   snprintf(exts, sizeof(exts), "%s", exts_decl);
   for (tok = strtok_r(exts, ",", &save); tok; tok = strtok_r(NULL, ",", &save))
   {
      trim(tok);
      snprintf(out, n, "%s/%s.%s", folder, base, tok);
      if (file_exists(out)) return;
   }
   out[0] = 0;
}

/* Multi-disc built-in game: the partition is read-only, so the playlist goes to
 * /tmp/lumen-stock/<basename1>.m3u (named after disc 1 so saves keep that name).
 * Replaces content with the playlist when at least two discs exist. */
static void stock_playlist(const platform_t *pl, const char *folder, char *discs, char *content, size_t n)
{
   char m3u[600], path[600], *tok, *save = NULL, *b1 = NULL;
   int count = 0;
   FILE *f = NULL;
   for (tok = strtok_r(discs, ",", &save); tok; tok = strtok_r(NULL, ",", &save))
   {
      trim(tok);
      if (!tok[0] || strchr(tok, '/')) continue;
      stock_find_disc(pl->exts, folder, tok, path, sizeof(path));
      if (!path[0]) { logf_("[%s] %s: disc %s missing", pl->id, folder, tok); continue; }
      if (!f)
      {
         b1 = tok;
         mkdir("/tmp/lumen-stock", 0755);
         snprintf(m3u, sizeof(m3u), "/tmp/lumen-stock/%s.m3u", b1);
         if (!(f = fopen(m3u, "w"))) { logf_("[%s] cannot write %s", pl->id, m3u); return; }
      }
      fprintf(f, "%s\n", path);
      count++;
   }
   if (!f) return;
   if (fclose(f) != 0) { logf_("[%s] cannot write %s", pl->id, m3u); return; }
   if (count >= 2) snprintf(content, n, "%s", m3u);
   else unlink(m3u);
}

static void scan_stock(int pi, const char *dir)
{
   static int listed;
   platform_t *pl = &g_plat[pi];
   char line[700];
   FILE *f = fopen(pl->meta, "r");
   int found = 0, missing = 0;
   if (!f) { logf_("[%s] stock metadata missing: %s", pl->id, pl->meta); return; }
   while (fgets(line, sizeof(line), f) && g_ngames < MAX_GAMES)
   {
      char *fld[6], *p = line, *c, folder[600], listing[400], discs[300];
      game_t *g;
      int k;
      DIR *d; struct dirent *de;
      line[strcspn(line, "\r\n")] = 0;
      if (!strncmp(p, "\xEF\xBB\xBF", 3)) p += 3;
      if (!*p || *p == '#') continue;
      for (k = 0; k < 6; k++) { fld[k] = p; if ((c = strchr(p, '|'))) { *c = 0; p = c + 1; } else p += strlen(p); }
      for (k = 0; k < 6; k++) trim(fld[k]);
      snprintf(discs, sizeof(discs), "%s", fld[5]);
      if ((c = strchr(fld[5], ','))) *c = 0;
      trim(fld[5]);
      if (!fld[0][0] || !fld[5][0] || strchr(fld[0], '/') || strchr(fld[5], '/') || strstr(fld[0], "..")) continue;
      snprintf(folder, sizeof(folder), "%s/%s", dir, fld[0]);
      g = &g_games[g_ngames];
      memset(g, 0, sizeof(*g));
      stock_find_disc(pl->exts, folder, fld[5], g->content, sizeof(g->content));
      snprintf(g->cover, sizeof(g->cover), "%s/%s.png", folder, fld[5]);
      g->has_cover = file_exists(g->cover);
      listing[0] = 0;
      if ((!g->has_cover || !listed) && (d = opendir(folder)))
      {
         while ((de = readdir(d)))
         {
            size_t l = strlen(de->d_name);
            if (de->d_name[0] == '.') continue;
            if (!listed && strlen(listing) + l + 2 < sizeof(listing))
            { if (listing[0]) strcat(listing, " "); strcat(listing, de->d_name); }
            if (!g->has_cover && l > 4 && !strcasecmp(de->d_name + l - 4, ".png"))
            { snprintf(g->cover, sizeof(g->cover), "%s/%s", folder, de->d_name); g->has_cover = 1; }
         }
         closedir(d);
      }
      if (!listed) logf_("[%s] %s: %s", pl->id, folder, listing[0] ? listing : "(empty or missing)");
      if (!g->has_cover) g->cover[0] = 0;
      if (!g->content[0]) { missing++; continue; }
      if (strchr(discs, ',')) stock_playlist(pl, folder, discs, g->content, sizeof(g->content));
      snprintf(g->title, sizeof(g->title), "%s", fld[1][0] ? fld[1] : fld[5]);
      snprintf(g->publisher, sizeof(g->publisher), "%s", fld[2]);
      snprintf(g->year, sizeof(g->year), "%s", fld[3]);
      snprintf(g->players, sizeof(g->players), "%s", fld[4]);
      g->platform = pi;
      g_ngames++; found++;
   }
   fclose(f);
   listed = 1;
   logf_("[%s] stock: %d games from %s (%d without a disc image)", pl->id, found, dir, missing);
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
         if (!strcmp(g_plat[pi].layout, "psc")) scan_psc(pi, tok);
         else if (!strcmp(g_plat[pi].layout, "stock")) scan_stock(pi, tok);
         else scan_files(pi, tok);
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
static GLint  u2_mvp, u2_tex, u2_color, u2_mode, u2_rect, u2_aux0, u2_aux1, u2_aux2;
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
   "uniform highp vec4 aux0, aux1, aux2;\n"
   /* signed distance to a rounded rectangle of rect.xy pixels, corner radius rect.z */
   "highp float rr_dist(){ highp vec2 hs = rect.xy * 0.5; highp float r = min(rect.z, min(hs.x, hs.y));\n"
   "  highp vec2 q = abs((v_uv - 0.5) * rect.xy) - (hs - vec2(r)); return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r; }\n"
   "void main(){\n"
   /* 6: dashboard tile: texture (uv window aux0) in a rounded mask; aux1.x sheen position
    *    (< -1 none), aux1.y top-light amount */
   "  if (mode==6) { float m = clamp(0.5 - rr_dist(), 0.0, 1.0);\n"
   "                 vec3 c = texture(tex, aux0.xy + v_uv * aux0.zw).rgb * color.rgb;\n"
   "                 if (aux1.x > -1.0) { float s = v_uv.x + v_uv.y * 0.55 - aux1.x; c += vec3(0.22 * exp(-s * s * 55.0)); }\n"
   "                 c += vec3(aux1.y * 0.10 * (1.0 - v_uv.y));\n"
   "                 frag = vec4(c, color.a * m); return; }\n"
   /* 7: generated background: diagonal gradient base(aux0) -> colour A(aux1), two soft
    *    light blobs (accent aux2, A) centred at rect.xy / rect.zw (moved by the CPU),
    *    dithered against banding */
   "  if (mode==7) { highp vec2 p = v_uv;\n"
   "                 float g = clamp(p.x * 0.55 + (1.0 - p.y) * 0.45, 0.0, 1.0);\n"
   "                 vec3 c = mix(aux0.rgb, aux1.rgb * 0.70, g * g);\n"
   "                 highp vec2 d1 = (p - rect.xy) * vec2(1.7778, 1.0), d2 = (p - rect.zw) * vec2(1.7778, 1.0);\n"
   "                 c += aux2.rgb * 0.55 * exp(-dot(d1, d1) * 2.6) + aux1.rgb * 0.35 * exp(-dot(d2, d2) * 3.5);\n"
   "                 highp float n = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453);\n"
   "                 frag = vec4(c + (n - 0.5) * (1.5 / 255.0), color.a); return; }\n"
   /* 8: legibility overlay: a uniform dark layer (aux0.x opacity), plus the left 58 %,
    *    top and bottom bands darkened further */
   "  if (mode==8) { float l = 1.0 - smoothstep(0.0, 0.64, v_uv.x); l = 0.70 * l * (0.55 + 0.45 * l);\n"
   "                 float b = 0.60 * smoothstep(0.62, 1.0, v_uv.y), t = 0.45 * (1.0 - smoothstep(0.0, 0.24, v_uv.y));\n"
   "                 float a = 1.0 - (1.0 - l) * (1.0 - b) * (1.0 - t) * (1.0 - aux0.x);\n"
   "                 frag = vec4(0.0, 0.0, 0.0, a * color.a); return; }\n"
   /* 9: flat rounded rectangle, filled (aux0.x <= 0) or stroked inside with width aux0.x */
   "  if (mode==9) { highp float d = rr_dist(); float m;\n"
   "                 if (aux0.x > 0.0) m = clamp(0.5 + aux0.x * 0.5 - abs(d + aux0.x * 0.5), 0.0, 1.0); else m = clamp(0.5 - d, 0.0, 1.0);\n"
   "                 frag = vec4(color.rgb, color.a * m); return; }\n"
   /* 10: soft drop shadow of a rounded rectangle; the quad is the shape grown by the blur
    *     radius aux0.x on every side (rect.xy = quad size, rect.z = corner radius), moved
    *     down by aux0.z px; aux0.y > 0 cuts the shadow out under the shape itself (for
    *     translucent shapes, like CSS box-shadow) */
   "  if (mode==10) { highp vec2 hs = rect.xy * 0.5 - vec2(aux0.x); highp float r = min(rect.z, min(hs.x, hs.y));\n"
   "                  highp vec2 pp = (v_uv - 0.5) * rect.xy;\n"
   "                  highp vec2 q = abs(pp) - (hs - vec2(r));\n"
   "                  highp float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;\n"
   "                  float k = 1.0 - smoothstep(-aux0.x * 0.35, aux0.x, d); k *= k;\n"
   "                  if (aux0.y > 0.0) { highp vec2 q2 = abs(pp + vec2(0.0, aux0.z)) - (hs - vec2(r));\n"
   "                    highp float d2 = length(max(q2, 0.0)) + min(max(q2.x, q2.y), 0.0) - r; k *= clamp(d2 + 0.5, 0.0, 1.0); }\n"
   "                  frag = vec4(0.0, 0.0, 0.0, color.a * k); return; }\n"
   "  if (mode==5) { highp vec2 hs = rect.xy * 0.5; highp float r = min(rect.z, min(hs.x, hs.y));\n"
   "                 highp vec2 q = abs((v_uv - 0.5) * rect.xy) - (hs - vec2(r));\n"
   "                 highp float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;\n"
   "                 float edge = clamp(0.5 - d, 0.0, 1.0);\n"
   "                 float bw = clamp(d + aux0.x, 0.0, 1.0) * rect.w;\n"
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

/* plain textured quad (mode 0: backgrounds, text, glyphs): its own tiny program, so the
 * most-drawn pixels do not run through FS_2D's chain of mode branches */
static const char *FS_TEX =
   "#version 300 es\n"
   "precision mediump float;\n"
   "in highp vec2 v_uv; uniform sampler2D tex; uniform vec4 color; out vec4 frag;\n"
   "void main(){ frag = texture(tex, v_uv) * color; }\n";
static GLuint g_prog_tex;
static GLint  ut_mvp, ut_color;
static int    g_fx_bgflat;   /* bench: draw the background as a flat colour (fill without the fetch) */

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
   u2_aux0 = glGetUniformLocation(g_prog_2d, "aux0");
   u2_aux1 = glGetUniformLocation(g_prog_2d, "aux1");
   u2_aux2 = glGetUniformLocation(g_prog_2d, "aux2");
   g_prog_tex = link_prog(VS_2D, FS_TEX);
   ut_mvp = glGetUniformLocation(g_prog_tex, "mvp");
   ut_color = glGetUniformLocation(g_prog_tex, "color");
   glUseProgram(g_prog_tex);
   glUniform1i(glGetUniformLocation(g_prog_tex, "tex"), 0);

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

/* Average colour and accent of an ABGR8888 surface (cover palette for the generated
 * background): the accent is the mean of the hue bin (12 bins) with the largest
 * saturation x value weight, so a small vivid logo beats a large grey area. */
static void surface_avg(SDL_Surface *c, float *avg, float *acc)
{
   double r = 0, g = 0, b = 0, bw[12] = { 0 }, br[12] = { 0 }, bg_[12] = { 0 }, bb[12] = { 0 }; long n = 0; int x, y, k, best = 0;
   for (y = 0; y < c->h; y += 4)
      for (x = 0; x < c->w; x += 4)
      {
         const unsigned char *p = (const unsigned char *)c->pixels + y * c->pitch + x * 4;
         int mx = p[0] > p[1] ? (p[0] > p[2] ? p[0] : p[2]) : (p[1] > p[2] ? p[1] : p[2]);
         int mn = p[0] < p[1] ? (p[0] < p[2] ? p[0] : p[2]) : (p[1] < p[2] ? p[1] : p[2]);
         r += p[0]; g += p[1]; b += p[2]; n++;
         if (acc && mx > 40 && mx - mn > 30)
         {
            float d = (float)(mx - mn), h, wgt = d / 255.0f * mx / 255.0f;
            if (mx == p[0]) h = fmodf((p[1] - p[2]) / d + 6.0f, 6.0f);
            else if (mx == p[1]) h = (p[2] - p[0]) / d + 2.0f;
            else h = (p[0] - p[1]) / d + 4.0f;
            k = (int)(h * 2.0f) % 12;
            bw[k] += wgt; br[k] += p[0] * wgt; bg_[k] += p[1] * wgt; bb[k] += p[2] * wgt;
         }
      }
   if (n) { avg[0] = r / n / 255.0; avg[1] = g / n / 255.0; avg[2] = b / n / 255.0; }
   if (!acc) return;
   for (k = 1; k < 12; k++) if (bw[k] > bw[best]) best = k;
   if (bw[best] > 0.0) { acc[0] = br[best] / bw[best] / 255.0; acc[1] = bg_[best] / bw[best] / 255.0; acc[2] = bb[best] / bw[best] / 255.0; }
   else { acc[0] = avg[0]; acc[1] = avg[1]; acc[2] = avg[2]; }
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

/* smallest device size >= n whose logical size (n / scale) is a whole number, so a
 * text texture drawn at its logical size maps 1:1 onto device pixels */
static int dev_pad(int n)
{
   int k;
   if (g_ui_scale == 1.0f) return n;
   for (k = n; k < n + 8; k++)
   {
      float l = k / g_ui_scale;
      if (fabsf(l - floorf(l + 0.5f)) < 0.001f) return k;
   }
   return n;
}

/* upload rendered text: padded to dev_pad() sizes; w / h returned in logical px */
static GLuint text_upload(SDL_Surface *surf, int *w, int *h)
{
   int dw = dev_pad(surf->w), dh = dev_pad(surf->h);
   GLuint t;
   if (dw != surf->w || dh != surf->h)
   {
      SDL_Surface *pad = SDL_CreateRGBSurface(0, dw, dh, 32, 0x000000ff, 0x0000ff00, 0x00ff0000, 0xff000000);
      if (pad)
      {
         SDL_FillRect(pad, NULL, SDL_MapRGBA(pad->format, 255, 255, 255, 0));
         SDL_SetSurfaceBlendMode(surf, SDL_BLENDMODE_NONE);
         SDL_BlitSurface(surf, NULL, pad, NULL);
         t = surface_to_tex(pad, 0, NULL);
         SDL_FreeSurface(pad);
         *w = (int)floorf(dw / g_ui_scale + 0.5f); *h = (int)floorf(dh / g_ui_scale + 0.5f);
         return t;
      }
   }
   *w = (int)floorf(surf->w / g_ui_scale + 0.5f); *h = (int)floorf(surf->h / g_ui_scale + 0.5f);
   return surface_to_tex(surf, 0, NULL);
}

GLuint text_tex(TTF_Font *f, const char *s, int *w, int *h)
{
   SDL_Color white = { 255, 255, 255, 255 };
   SDL_Surface *surf; GLuint t;
   if (!f || !s || !*s) { *w = *h = 0; return 0; }
   surf = TTF_RenderUTF8_Blended(f, s, white);
   if (!surf) { *w = *h = 0; return 0; }
   t = text_upload(surf, w, h);
   SDL_FreeSurface(surf);
   return t;
}

/* wrap_px in logical px */
GLuint text_tex_wrapped(TTF_Font *f, const char *s, int wrap_px, int *w, int *h)
{
   SDL_Color white = { 255, 255, 255, 255 };
   SDL_Surface *surf; GLuint t;
   if (!f || !s || !*s) { *w = *h = 0; return 0; }
   surf = TTF_RenderUTF8_Blended_Wrapped(f, s, white, (Uint32)(wrap_px * g_ui_scale));
   if (!surf) { *w = *h = 0; return 0; }
   t = text_upload(surf, w, h);
   SDL_FreeSurface(surf);
   return t;
}

/* scissor rectangle in logical coordinates (top-left origin) */
void gl_scissor(float x, float y, float w, float h)
{
   int x0 = (int)floorf(x * g_ui_scale), y0 = (int)floorf((H - y - h) * g_ui_scale);
   int sw = (int)ceilf(w * g_ui_scale), sh = (int)ceilf(h * g_ui_scale);
   glScissor(x0 < 0 ? 0 : x0, y0 < 0 ? 0 : y0, sw > 0 ? sw : 0, sh > 0 ? sh : 0);
}

static unsigned hash_str(const char *s) { unsigned h = 2166136261u; while (*s) h = (h ^ (unsigned char)*s++) * 16777619u; return h; }

/* Back of the case / placeholder front: coloured card with wrapped text. */
#define SP(v) ((int)((v) * g_ui_scale + 0.5f))   /* logical px -> device px (fonts are scaled) */
static GLuint card_tex(const game_t *g, int back)
{
   SDL_Surface *card = SDL_CreateRGBSurface(0, SP(256), SP(256), 32, 0x000000ff, 0x0000ff00, 0x00ff0000, 0xff000000);
   SDL_Color white = { 255, 255, 255, 255 };
   SDL_Rect r; GLuint t; char line[160]; int y = SP(18);
   Uint8 cr = (Uint8)(g->col[0] * 255), cg = (Uint8)(g->col[1] * 255), cb = (Uint8)(g->col[2] * 255);
   if (!card) return 0;
   if (back) { cr = cr / 3 + 12; cg = cg / 3 + 12; cb = cb / 3 + 18; }
   SDL_FillRect(card, NULL, SDL_MapRGBA(card->format, cr, cg, cb, 255));
#define LINE(font, txt) do { SDL_Surface *ts = TTF_RenderUTF8_Blended_Wrapped(font, txt, white, SP(224)); \
      if (ts) { r.x = SP(16); r.y = y; r.w = ts->w; r.h = ts->h; SDL_BlitSurface(ts, NULL, card, &r); y += ts->h + SP(6); SDL_FreeSurface(ts); } } while (0)
   if (g_font_cover)
   {
      if (!back) y = SP(150);
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
         y = SP(220); LINE(g_font_small, line);
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
   strip = SDL_CreateRGBSurface(0, SP(512), SP(48), 32, 0x000000ff, 0x0000ff00, 0x00ff0000, 0xff000000);
   if (!strip) return 0;
   SDL_FillRect(strip, NULL, SDL_MapRGBA(strip->format, 0, 0, 0, 0));
   ts = TTF_RenderUTF8_Blended(g_font_small, g->title, white);
   if (ts)
   {
      SDL_SetSurfaceBlendMode(ts, SDL_BLENDMODE_NONE);
      r.w = ts->w > SP(470) ? SP(470) : ts->w; r.h = ts->h > SP(46) ? SP(46) : ts->h;
      r.x = SP(24); r.y = (SP(48) - r.h) / 2;
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
   g->acc[0] = g->col[2]; g->acc[1] = g->col[0]; g->acc[2] = g->col[1];
   g->tex_w = g->tex_h = 256;
   g->tex = card_tex(g, 0);
   g->tex_state = -1;
   g->spine_tex = spine_tex(g);
}

static void game_finish_cover(game_t *g, SDL_Surface *s, const float avg[3], const float acc[3])
{
   if (s)
   {
      g->tex = surface_to_tex(s, 1, NULL);
      g->tex_w = s->w; g->tex_h = s->h;
      memcpy(g->col, avg, sizeof(g->col));
      memcpy(g->acc, acc, sizeof(g->acc));
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

static void cost_add_3d(const mat4 *mvp, int mode);
static void draw_quad_box(mat4 model, GLuint tex, const float tint[3], float alpha, int mode)
{
   mat4 mvp = m4_mul(g_vp, model);
   cost_add_3d(&mvp, mode);
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
static int    g_fx_refl = 1, g_fx_glass = 1, g_fx_sides = 1, g_fx_text = 1;
static int    g_fx_shadows = 1, g_fx_bg = 1;   /* bench switches: dashboard shadows, background layers */
static float  g_bg_dim = 0.45f;                /* scrim opacity baked into backgrounds (setting bg_dim) */
static GLuint g_vig_tex, g_spot_tex;   /* baked full-screen vignette / spotlight masks */
static int    g_dash_profile_dirty = 1; /* dashboard: avatar + play statistics to reload */

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

/* cost meter (LUMEN_COST=1): device pixels covered per frame, split by blending and by
 * shader weight, plus draw calls; logged once per second. A proxy for GPU load that a
 * software renderer can measure too (tools/host). */
static int    g_cost_on;
static double g_cost_px_opaque, g_cost_px_blend, g_cost_px_heavy, g_cost_draws;
static void cost_add(float w, float h, int mode)
{
   double px = fabs((double)w * h) * g_ui_scale * g_ui_scale;
   GLint vp[4];
   glGetIntegerv(GL_VIEWPORT, vp);
   if (vp[2] != g_dw) px *= (double)vp[2] * vp[3] / ((double)g_dw * g_dh);   /* offscreen FBO */
   if (glIsEnabled(GL_BLEND)) g_cost_px_blend += px; else g_cost_px_opaque += px;
   if (mode == 7 || mode == 8 || mode == 10 || mode == 5) g_cost_px_heavy += px;
   g_cost_draws += 1;
}

/* 3D quads (unit square in model space): the projected area in device pixels. The box
 * shader's heavy modes are the glass lid (4), the discs (5, 6), the marquee art and
 * halo (7, 8) and the coin (9). */
static void cost_add_3d(const mat4 *mvp, int mode)
{
   static const float cx[4] = { -0.5f, 0.5f, 0.5f, -0.5f }, cy[4] = { -0.5f, -0.5f, 0.5f, 0.5f };
   const float *m = mvp->m;
   float sx[4], sy[4], area = 0.0f;
   double px;
   int k;
   if (!g_cost_on) return;
   for (k = 0; k < 4; k++)
   {
      float x = m[0] * cx[k] + m[4] * cy[k] + m[12], y = m[1] * cx[k] + m[5] * cy[k] + m[13];
      float w = m[3] * cx[k] + m[7] * cy[k] + m[15];
      if (w < 0.01f) w = 0.01f;
      sx[k] = clampf(x / w, -1.0f, 1.0f) * 0.5f * g_dw; sy[k] = clampf(y / w, -1.0f, 1.0f) * 0.5f * g_dh;
   }
   for (k = 0; k < 4; k++) area += sx[k] * sy[(k + 1) & 3] - sx[(k + 1) & 3] * sy[k];
   px = fabs(area) * 0.5;
   if (glIsEnabled(GL_BLEND)) g_cost_px_blend += px; else g_cost_px_opaque += px;
   if (mode >= 4 && mode <= 9) g_cost_px_heavy += px;
   g_cost_draws += 1;
}

void draw_2d(GLuint tex, float x, float y, float w, float h, const float rgba[4], int mode)
{
   if (g_cost_on) cost_add(w, h, mode);
   if (mode == 0 && tex && fabsf(w * g_ui_scale - floorf(w * g_ui_scale + 0.5f)) < 0.01f &&
       fabsf(h * g_ui_scale - floorf(h * g_ui_scale + 0.5f)) < 0.01f)
   {  /* textures drawn 1:1 (text, glyphs) start on a device pixel: no half-pixel
       * resampling; scaled or animated images (backgrounds) keep sub-pixel motion */
      x = floorf(x * g_ui_scale + 0.5f) / g_ui_scale;
      y = floorf(y * g_ui_scale + 0.5f) / g_ui_scale;
   }
   mat4 m = m4_mul(g_ortho, m4_mul(m4_translate(x + w * 0.5f, y + h * 0.5f, 0), m4_scale(w, -h, 1)));
   if (mode == 0)
   {  /* callers work with g_prog_2d bound: switch for this draw only */
      glUseProgram(g_prog_tex);
      glUniformMatrix4fv(ut_mvp, 1, GL_FALSE, m.m);
      glUniform4fv(ut_color, 1, rgba);
      glBindTexture(GL_TEXTURE_2D, tex ? tex : g_white);
      glDrawArrays(GL_TRIANGLES, 0, 6);
      glUseProgram(g_prog_2d);
      return;
   }
   glUniformMatrix4fv(u2_mvp, 1, GL_FALSE, m.m);
   glUniform4fv(u2_color, 1, rgba);
   glUniform1i(u2_mode, mode);
   glBindTexture(GL_TEXTURE_2D, tex ? tex : g_white);
   glDrawArrays(GL_TRIANGLES, 0, 6);
}

/* The box shader and its states are only used by the launch animation, so the driver
 * compiled them on the first launch frame: the sim stalled 560 ms on Play, and again
 * 210 ms when the disc first showed (the animation jumped 0.6 s and 0.26 s ahead).
 * PowerVR also builds a variant per blend / depth state. So every launch draw is issued
 * once at startup, behind the boot splash, with the same programs, states and kinds of
 * texture (mipmapped RGBA, like covers, spines, backs and disc art), invisible (alpha 0,
 * a few pixels) before the first frame clears the screen. */
static void launch_warmup(void)
{
   static const float one[3] = { 1, 1, 1 };
   static game_t wg;
   double t0 = now_s();
   GLuint wtexs[2] = { 0, 0 }, wtex, keep_disc = g_disc_tex;
   const float light[3] = { -0.5f, 0.7f, 1.0f }, cam[3] = { 0.0f, -0.22f, 4.7f };
   mat4 base = m4_mul(m4_translate(0, 0, 0), m4_scale(0.004f, 0.004f, 0.004f));
   int p, k, x, done_kind[3] = { 0, 0, 0 };
   for (x = 0; x < 2; x++)
   {  /* power-of-two and not (covers are any size): some drivers specialise on it */
      SDL_Surface *ws = SDL_CreateRGBSurface(0, x ? 10 : 8, x ? 6 : 8, 32, 0x000000ff, 0x0000ff00, 0x00ff0000, 0xff000000);
      if (ws) { SDL_FillRect(ws, NULL, SDL_MapRGBA(ws->format, 128, 128, 128, 255)); wtexs[x] = surface_to_tex(ws, 1, NULL); SDL_FreeSurface(ws); }
   }
   memset(&wg, 0, sizeof(wg));
   wg.sc.x = 1.0f; wg.al.x = 1.0f;
   glViewport(0, 0, g_dw, g_dh);
   g_vp = m4_mul(g_proj, g_view);
   glEnable(GL_BLEND);
   glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
   glUseProgram(g_prog_box);
   glUniform1i(u_tex, 0);
   glUniform3fv(u_light, 1, light);
   glUniform3fv(u_cam, 1, cam);
   glEnable(GL_DEPTH_TEST);
   glDepthFunc(GL_LEQUAL);
   for (p = 0; p < g_nplat * 2; p++)
   {
      const platform_t *gp = &g_plat[p % g_nplat];
      if (done_kind[gp->kind] & (1 << (p / g_nplat))) continue;
      done_kind[gp->kind] |= 1 << (p / g_nplat);
      wtex = wtexs[p / g_nplat];
      wg.tex = wg.spine_tex = wtex;
      for (k = 0; k < 4; k++)
      {  /* back card or not, disc art or the generic CD */
         wg.back_tex = (k & 1) ? wtex : 0;
         g_disc_tex = (k & 2) ? wtex : 0;
         g_detail = 1;
         glDisable(GL_DEPTH_TEST);   /* reflection pass */
         glUniform1f(u_refl, -0.5f);
         draw_media(&wg, gp, base, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f);
         glUniform1f(u_refl, 100.0f);
         glEnable(GL_DEPTH_TEST);
         draw_media(&wg, gp, base, 0.0f, 1.0f, gp->kind == KIND_DISC ? 1.0f : 0.0f, 0.0f, 0.0f, 1.0f);
         if (gp->kind == KIND_DISC)
         {
            glDisable(GL_DEPTH_TEST);
            draw_flying_disc(0.0f, 0.0f, 0.0f);
            glEnable(GL_DEPTH_TEST);
         }
      }
      if (gp->kind == KIND_ARCADE)
      {  /* coin plate, slot and coin */
         draw_quad_box(base, 0, one, 0.0f, 1);
         draw_quad_box(base, 0, one, 0.0f, 9);
      }
   }
   glDisable(GL_DEPTH_TEST);
   /* 2D: the flash + fade quad (premultiplied blend) */
   glUseProgram(g_prog_2d);
   glUniform1i(u2_tex, 0);
   {
      const float clear_[4] = { 0, 0, 0, 0 };
      glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
      draw_2d(0, 0, 0, 1, 1, clear_, 0);
      glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
   }
   glFinish();
   g_disc_tex = keep_disc;
   g_disc_start = m4_identity();
   glDeleteTextures(2, wtexs);
   logf_("launch warm-up: %.0f ms", (now_s() - t0) * 1000.0);
}

/* glass rounded rectangle (mode 5): the shader needs the size in pixels for its distance field */
void draw_round_rect(float x, float y, float w, float h, float radius, const float rgba[4], float border_alpha)
{
   const float k = g_ui_scale;   /* shape maths in device px: 1-px antialiased edges */
   glUniform4f(u2_rect, w * k, h * k, radius * k, border_alpha);
   glUniform4f(u2_aux0, 2.0f * k, 0.0f, 0.0f, 0.0f);   /* border band: 2 logical px */
   draw_2d(0, x, y, w, h, rgba, 5);
}

/* flat rounded rectangle (mode 9): filled, or stroked inside with width `stroke` */
static void draw_shape(float x, float y, float w, float h, float radius, float stroke, const float rgba[4])
{
   const float k = g_ui_scale;
   glUniform4f(u2_rect, w * k, h * k, radius * k, 0.0f);
   glUniform4f(u2_aux0, stroke * k, 0.0f, 0.0f, 0.0f);
   draw_2d(0, x, y, w, h, rgba, 9);
}

/* soft shadow under a rounded rectangle x,y,w,h: blur radius, offset dy, opacity a;
 * cut = 1 leaves the area under the shape itself clear (translucent shapes) */
static void draw_shadow_ex(float x, float y, float w, float h, float radius, float blur, float dy, float a, int cut)
{
   const float c[4] = { 0, 0, 0, a };
   if (!g_fx_shadows) return;
   const float k = g_ui_scale;
   glUniform4f(u2_rect, (w + 2 * blur) * k, (h + 2 * blur) * k, radius * k, 0.0f);
   glUniform4f(u2_aux0, blur * k, cut ? 1.0f : 0.0f, dy * k, 0.0f);
   draw_2d(0, x - blur, y - blur + dy, w + 2 * blur, h + 2 * blur, c, 10);
}
static void draw_shadow(float x, float y, float w, float h, float radius, float blur, float dy, float a)
{
   draw_shadow_ex(x, y, w, h, radius, blur, dy, a, 0);
}

/* label with a soft 1-2 px shadow, for text over bright art */
static void draw_text(const label_t *l, float x, float y, const float c[4])
{
   const float sh[4] = { 0, 0, 0, 0.45f * c[3] };
   if (!l->tex) return;
   draw_2d(l->tex, floorf(x) + 1, floorf(y) + 2, l->w, l->h, sh, 0);
   draw_2d(l->tex, floorf(x), floorf(y), l->w, l->h, c, 0);
}

/* The focused tile's shadow, baked once: same falloff as FS_2D mode 10 for a 220 px
 * tile (radius 14, blur 22), drawn as a plain texture (a blurred shape needs no
 * resolution, and the SDF shader cost ~2 ms a frame at 1080p). */
static GLuint g_tile_shadow;
static void make_tile_shadow(void)
{
   enum { N = 132 };
   static unsigned char px[N * N * 4];
   const float full = 264.0f, hs = 110.0f, r = 14.0f, blur = 22.0f;
   int x, y;
   for (y = 0; y < N; y++)
      for (x = 0; x < N; x++)
      {
         float qx = fabsf((x + 0.5f) / N * full - full * 0.5f) - (hs - r), qy = fabsf((y + 0.5f) / N * full - full * 0.5f) - (hs - r);
         float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0, in = qx > qy ? qx : qy;
         float d = sqrtf(ox * ox + oy * oy) + (in < 0 ? in : 0) - r;
         float k = 1.0f - smooth01(-blur * 0.35f, blur, d);
         unsigned char *p = px + (y * N + x) * 4;
         p[0] = p[1] = p[2] = 255; p[3] = (unsigned char)(255.0f * k * k + 0.5f);
      }
   glGenTextures(1, &g_tile_shadow);
   glBindTexture(GL_TEXTURE_2D, g_tile_shadow);
   glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, N, N, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

/* textured rounded tile (mode 6), the texture (tw x th) cover-fitted into w x h;
 * sheen < -1 = none */
static void draw_tile(GLuint tex, int tw, int th, float x, float y, float w, float h, float radius,
                      float sheen, float light, const float rgba[4])
{
   float u0 = 0, v0 = 0, us = 1, vs = 1;
   if (tw > 0 && th > 0)
   {
      float ar = (float)tw / th, want = w / h;
      if (ar > want) { us = want / ar; u0 = (1.0f - us) * 0.5f; }
      else { vs = ar / want; v0 = (1.0f - vs) * 0.5f; }
   }
   glUniform4f(u2_rect, w * g_ui_scale, h * g_ui_scale, radius * g_ui_scale, 0.0f);
   glUniform4f(u2_aux0, u0, v0, us, vs);
   glUniform4f(u2_aux1, sheen, light, 0.0f, 0.0f);
   draw_2d(tex, x, y, w, h, rgba, 6);
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
      TTF_Font *f = TTF_OpenFont(paths[i], (int)(size * g_ui_scale + 0.5f));
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
static unsigned     cv_gen, cv_req_gen, cv_done_gen;   /* library generation: a rescan drops stale results */
static char         cv_path[512];
static SDL_Surface *cv_surf;
static float        cv_avg[3], cv_acc[3];

static int cover_worker(void *unused)
{
   (void)unused;
   SDL_LockMutex(cv_mx);
   while (!cv_quit)
   {
      int idx; unsigned gen; char path[512]; SDL_Surface *s, *c = NULL;
      float avg[3] = { 0.3f, 0.3f, 0.35f }, acc[3] = { 0.3f, 0.3f, 0.35f };
      while (cv_req < 0 && !cv_quit) SDL_CondWait(cv_cv, cv_mx);
      if (cv_quit) break;
      idx = cv_req; cv_req = -1;
      gen = cv_req_gen;
      snprintf(path, sizeof(path), "%s", cv_path);
      SDL_UnlockMutex(cv_mx);
      s = IMG_Load(path);
      if (s) { c = SDL_ConvertSurfaceFormat(s, SDL_PIXELFORMAT_ABGR8888, 0); SDL_FreeSurface(s); }
      if (c) surface_avg(c, avg, acc);
      SDL_LockMutex(cv_mx);
      cv_surf = c; cv_done = idx; cv_done_gen = gen;
      memcpy(cv_avg, avg, sizeof(avg)); memcpy(cv_acc, acc, sizeof(acc));
   }
   SDL_UnlockMutex(cv_mx);
   return 0;
}

static void cover_request(int idx, const char *path)
{
   SDL_LockMutex(cv_mx);
   snprintf(cv_path, sizeof(cv_path), "%s", path);
   cv_req = idx; cv_req_gen = cv_gen; cv_busy = 1;
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
static int          bg_done_failed;   /* the request bg_done_path could not be decoded */
static int          bg_quit;

/* The legibility scrim (FS_2D mode 8) baked into a decoded background, so drawing it
 * costs one opaque pass instead of two blended ones. The scrim is separable:
 * kept = (1 - left(u)) * (1 - bottom(v)) * (1 - top(v)) * (1 - dim). Backgrounds are
 * 16:9 and drawn cover-fit, so image space ~ screen space. */
static float sstep(float e0, float e1, float x) { float t = (x - e0) / (e1 - e0); t = t < 0 ? 0 : (t > 1 ? 1 : t); return t * t * (3 - 2 * t); }
static void bake_scrim(SDL_Surface *c, float dim)
{
   int x, y;
   unsigned short *fx = malloc(sizeof(unsigned short) * c->w);
   if (!fx) return;
   for (x = 0; x < c->w; x++)
   {
      float l = 1.0f - sstep(0.0f, 0.64f, (x + 0.5f) / c->w);
      l = 0.70f * l * (0.55f + 0.45f * l);
      fx[x] = (unsigned short)((1.0f - l) * 256.0f);
   }
   for (y = 0; y < c->h; y++)
   {
      float v = (y + 0.5f) / c->h;
      float fy = (1.0f - 0.60f * sstep(0.62f, 1.0f, v)) * (1.0f - 0.45f * (1.0f - sstep(0.0f, 0.24f, v))) * (1.0f - dim);
      unsigned k = (unsigned)(fy * 256.0f);
      unsigned char *p = (unsigned char *)c->pixels + y * c->pitch;
      for (x = 0; x < c->w; x++, p += 4)
      {
         unsigned m = (fx[x] * k) >> 8;
         p[0] = (unsigned char)((p[0] * m) >> 8); p[1] = (unsigned char)((p[1] * m) >> 8); p[2] = (unsigned char)((p[2] * m) >> 8);
      }
   }
   free(fx);
}

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
      if (c) bake_scrim(c, g_bg_dim);
      SDL_LockMutex(bg_mx);
      if (bg_done) SDL_FreeSurface(bg_done);
      bg_done = c;
      bg_done_failed = !c && path[0];
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
/* disc art for the launch: decoded on a worker once the selection settles,   */
/* uploaded on the main thread (decoding the 512 px PNG at Play stalled the    */
/* first launch frame)                                                         */
static SDL_mutex   *dc_mx;
static SDL_cond    *dc_cv;
static char         dc_req[512];       /* path requested (guarded by dc_mx)  */
static int          dc_req_new, dc_quit;
static SDL_Surface *dc_done;           /* decoded surface ready to upload    */
static char         dc_done_path[512];
static int          dc_done_new;
static char         g_disc_want[512];  /* main thread: art asked for          */
static char         g_disc_path[512];  /* main thread: art held in g_disc_tex */

static int disc_worker(void *unused)
{
   (void)unused;
   SDL_LockMutex(dc_mx);
   while (!dc_quit)
   {
      char path[512]; SDL_Surface *s, *c = NULL;
      while (!dc_req_new && !dc_quit) SDL_CondWait(dc_cv, dc_mx);
      if (dc_quit) break;
      snprintf(path, sizeof(path), "%s", dc_req);
      dc_req_new = 0;
      SDL_UnlockMutex(dc_mx);
      s = IMG_Load(path);
      if (s) { c = SDL_ConvertSurfaceFormat(s, SDL_PIXELFORMAT_ABGR8888, 0); SDL_FreeSurface(s); }
      if (!c) logf_("disc art failed: %s", path);
      SDL_LockMutex(dc_mx);
      if (dc_done) SDL_FreeSurface(dc_done);
      dc_done = c; dc_done_new = 1;
      snprintf(dc_done_path, sizeof(dc_done_path), "%s", path);
   }
   SDL_UnlockMutex(dc_mx);
   return 0;
}

/* ask for the disc art of a game ("" = none: the generic CD) */
static void disc_want(const char *path)
{
   if (!strcmp(path, g_disc_want)) return;
   snprintf(g_disc_want, sizeof(g_disc_want), "%s", path);
   if (strcmp(g_disc_path, g_disc_want) && g_disc_tex)
   {  /* never show another game's disc while the new one decodes */
      glDeleteTextures(1, &g_disc_tex); g_disc_tex = 0; g_disc_path[0] = 0;
   }
   if (!path[0] || !strcmp(g_disc_path, path)) return;
   SDL_LockMutex(dc_mx);
   snprintf(dc_req, sizeof(dc_req), "%s", path);
   dc_req_new = 1;
   SDL_CondSignal(dc_cv);
   SDL_UnlockMutex(dc_mx);
}

static void disc_poll_upload(void)
{
   SDL_Surface *s = NULL; char path[512] = "";
   SDL_LockMutex(dc_mx);
   if (dc_done_new) { s = dc_done; dc_done = NULL; dc_done_new = 0; snprintf(path, sizeof(path), "%s", dc_done_path); }
   SDL_UnlockMutex(dc_mx);
   if (s && !strcmp(path, g_disc_want))
   {
      if (g_disc_tex) glDeleteTextures(1, &g_disc_tex);
      g_disc_tex = surface_to_tex(s, 1, NULL);
      snprintf(g_disc_path, sizeof(g_disc_path), "%s", path);
   }
   if (s) SDL_FreeSurface(s);
}

/* ------------------------------------------------------------------------ */
/* input: game controllers + raw joysticks mapped to logical actions         */
/* IN_SQUARE acts as IN_BACK everywhere except the keyboard, where it erases */
enum { IN_LEFT, IN_RIGHT, IN_UP, IN_DOWN, IN_L1, IN_R1, IN_L2, IN_R2, IN_PLAY, IN_BACK, IN_SQUARE, IN_START, IN_SELECT, IN_OPTIONS, IN_COUNT };
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
   case 4: return IN_L2;
   case 5: return IN_R2;
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
      /* analog triggers (DualShock 4 and others): L2 / R2 past half travel */
      if (e->caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT) in_set(IN_L2, e->caxis.value > 16000);
      if (e->caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT) in_set(IN_R2, e->caxis.value > 16000);
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
      case SDLK_HOME: b = IN_L2; break;
      case SDLK_END: b = IN_R2; break;
      case SDLK_RETURN: b = IN_PLAY; break;
      case SDLK_TAB: case SDLK_F1: b = IN_OPTIONS; break;
      case SDLK_BACKSPACE: b = IN_SQUARE; break;
      case SDLK_b: b = IN_BACK; break;        /* keyboards / psc-sim: Circle */
      case SDLK_s: b = IN_SELECT; break;      /* Select alone: settings */
      case SDLK_ESCAPE: b = IN_SELECT; in_set(IN_START, d); break;
      }
      in_set(b, d);
      break;
   }
   }
}

/* ------------------------------------------------------------------------ */
/* help bar: button icons ($home/assets/icons/<name>.png, optional) + labels */
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
 * $home/assets/icons/<name>.png (old layout: $home/icons/; loaded once, shared),
 * fallback text when one is missing */
typedef struct { const char *icons[3]; const char *fallback_key; const char *text_key; } hint_t;
#define SET_HINTS 5
static const hint_t g_hints[HINTS_COUNT][SET_HINTS] = {
   [HINTS_CAROUSEL] = {
      { { "cross" },    "btn.cross",        "hint.play" },
      { { "l1", "r1" }, "btn.l1_r1",        "hint.page" },
      { { "l2", "r2" }, "btn.l2_r2",        "hint.platform" },
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
   [HINTS_WELCOME] = {
      { { "cross" },    "btn.cross",        "hint.next" },
      { { "circle" },   "btn.circle",       "hint.back" },
      { { "triangle" }, "btn.triangle",     "hint.dont_show" } },
   [HINTS_WELCOME_LAST] = {
      { { "cross" },    "btn.cross",        "hint.start" },
      { { "circle" },   "btn.circle",       "hint.back" },
      { { "triangle" }, "btn.triangle",     "hint.dont_show" } },
};
typedef struct { char name[16]; GLuint tex; int w, h; } icon_t;
static icon_t g_icons[24];
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
   /* flat glyphs first (dashboard style), then the pixel-art icons, then the old layout */
   snprintf(path, sizeof(path), "%s/assets/glyphs/%s.png", home, name);
   if (!file_exists(path)) snprintf(path, sizeof(path), "%s/assets/icons/%s.png", home, name);
   if (!file_exists(path)) snprintf(path, sizeof(path), "%s/icons/%s.png", home, name);   /* old layout */
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

/* icons once ($home/assets/icons/<name>.png), then the texts */
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
   g_dash_profile_dirty = 1;   /* also the dashboard's avatar and statistics */
   for (i = 0; i < g_ngames; i++)
      if (g_games[i].back_tex) { glDeleteTextures(1, &g_games[i].back_tex); g_games[i].back_tex = 0; }
}

/* Display page: leave with exit code 12; run.sh / boot.sh restart the console so the
 * new output and compositor are set up from scratch */
static int g_want_reboot;
void ui_request_reboot(void) { g_want_reboot = 1; logf_("restart requested (display settings)"); }

/* the Sound page changed music / music_volume */
void ui_sound_changed(void)
{
   int on = settings_get_int("music", 1, 0, 1);
   music_set_volume(on ? settings_get_int("music_volume", 60, 0, 100) / 100.0f : 0.0f);
}

/* "Preview now" in the screensaver page */
void ui_start_attract(void)
{
   attract_start(now_s());
}

/* draw one hint set centred on (g_hint_cx, y) */
static void hints_draw(int set, float y, float alpha)
{
   /* glyphs are flat art at 2x (64 px high): drawn at 30 px with linear filtering
    * (the pixel-art icons, when no glyph exists, are scaled the same way) */
   /* icons are used only when every one of the hint loaded (have_icons, h > 0); else
    * the fallback text, and with neither (empty btn.* and no PNG) just the label */
   const float icon_h = 30.0f, gap_icon = 6.0f, gap_txt = 10.0f;
   const float ic[4] = { 1, 1, 1, alpha }, tc[4] = { 0.86f, 0.86f, 0.90f, alpha };
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

/* Background layers: an image (tex) or a background generated from the colours of a
 * game's cover (gen, palette copied when the layer is made). The next layer crossfades
 * over the current one in 0.6 s; a layer pushed meanwhile waits (only the newest), so a
 * crossfade is never cut short. */
typedef struct { GLuint tex; int w, h; int gen; float a[3], acc[3]; GLuint gfbo, gtex; } bgtex_t;
#define GEN_W 640    /* generated backgrounds render at 640x360 and are scaled up: */
#define GEN_H 360    /* smooth gradients, a tenth of the pixels of a 1080p pass */
static bgtex_t bg_cur, bg_next, bg_pending;
static float   bg_mix = 1.0f;          /* 0 -> showing cur, 1 -> next done */
static char    bg_shown[512];          /* key of the background we want: path or "gen:<i>" */
static int     bg_game = -1;           /* game of the last request (fallback when its image fails) */

static int  bg_empty(const bgtex_t *l) { return !l->tex && !l->gen; }
static void bg_clear(bgtex_t *l)
{
   if (l->tex) glDeleteTextures(1, &l->tex);
   if (l->gtex) glDeleteTextures(1, &l->gtex);
   if (l->gfbo) glDeleteFramebuffers(1, &l->gfbo);
   memset(l, 0, sizeof(*l));
}

static void bg_push(bgtex_t l)
{
   if (bg_empty(&bg_next)) { bg_next = l; bg_mix = 0.0f; }
   else { bg_clear(&bg_pending); bg_pending = l; }
}

/* once a crossfade ends: next becomes current, the pending layer starts its fade */
static void bg_advance(float dt)
{
   bg_mix = bg_mix + dt / 0.6f > 1.0f ? 1.0f : bg_mix + dt / 0.6f;
   if (bg_mix < 1.0f || bg_empty(&bg_next)) return;
   bg_clear(&bg_cur);
   bg_cur = bg_next; memset(&bg_next, 0, sizeof(bg_next));
   if (!bg_empty(&bg_pending)) { bg_next = bg_pending; memset(&bg_pending, 0, sizeof(bg_pending)); bg_mix = 0.0f; }
}

/* generated layer from a game's palette (defaults when the game or its cover is unknown) */
static bgtex_t bg_gen_layer(int gi)
{
   bgtex_t l;
   memset(&l, 0, sizeof(l));
   l.gen = 1;
   l.a[0] = 0.22f; l.a[1] = 0.20f; l.a[2] = 0.30f; l.acc[0] = 0.30f; l.acc[1] = 0.25f; l.acc[2] = 0.45f;
   if (gi >= 0 && gi < g_ngames && (g_games[gi].tex_state == 1 || g_games[gi].tex_state == -1))
   { memcpy(l.a, g_games[gi].col, sizeof(l.a)); memcpy(l.acc, g_games[gi].acc, sizeof(l.acc)); }
   return l;
}

static void bg_poll_upload(void)
{
   SDL_Surface *s = NULL; char path[512] = "";
   int failed;
   bgtex_t l;
   SDL_LockMutex(bg_mx);
   s = bg_done; bg_done = NULL;
   failed = bg_done_failed; bg_done_failed = 0;
   snprintf(path, sizeof(path), "%s", bg_done_path);
   SDL_UnlockMutex(bg_mx);
   if (!s)
   {  /* unreadable image: the generated background of that game instead */
      if (failed && !strcmp(path, bg_shown)) bg_push(bg_gen_layer(bg_game));
      return;
   }
   if (strcmp(path, bg_shown)) { SDL_FreeSurface(s); return; } /* stale */
   memset(&l, 0, sizeof(l));
   l.tex = surface_to_tex(s, 0, NULL);
   l.w = s->w; l.h = s->h;
   SDL_FreeSurface(s);
   bg_push(l);
}

/* draw a layer: images "cover"-fitted with a slow drift (scrim already baked in);
 * generated ones from their palette, rendered with the scrim into a small FBO */
static void bg_draw(bgtex_t *b, float alpha, double t)
{
   const float c[4] = { 1, 1, 1, alpha };
   int opaque = alpha >= 0.999f;
   if (alpha <= 0.001f || !g_fx_bg) return;
   if (opaque) glDisable(GL_BLEND);
   if (b->tex)
   {
      /* cover-fitted and still: a 16:9 image at the output size maps 1:1 (sharp) */
      float sx = (float)W / b->w, sy = (float)H / b->h, sc = sx > sy ? sx : sy, w, h;
      w = b->w * sc; h = b->h * sc;
      draw_2d(g_fx_bgflat ? 0 : b->tex, (W - w) * 0.5f, (H - h) * 0.5f, w, h, c, 0);
   }
   else if (b->gen)
   {
      const float one[4] = { 1, 1, 1, 1 };
      float a[3], acc[3], base[3];
      int k;
      if (!b->gfbo)
      {
         glGenTextures(1, &b->gtex);
         glBindTexture(GL_TEXTURE_2D, b->gtex);
         glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, GEN_W, GEN_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
         glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
         glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
         glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
         glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
         glGenFramebuffers(1, &b->gfbo);
         glBindFramebuffer(GL_FRAMEBUFFER, b->gfbo);
         glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, b->gtex, 0);
         if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) logf_("bg: generated FBO incomplete");
      }
      memcpy(a, b->a, sizeof(a)); memcpy(acc, b->acc, sizeof(acc));
      for (k = 0; k < 3; k++)
      {  /* A a bit more saturated around its mean; base = A darkened */
         float m = (a[0] + a[1] + a[2]) / 3.0f;
         a[k] = clampf(m + (a[k] - m) * 1.35f, 0.0f, 1.0f);
         base[k] = a[k] * 0.22f + 0.015f;
      }
      for (k = 0; k < 3; k++) { float m = (acc[0] + acc[1] + acc[2]) / 3.0f; acc[k] = clampf(m + (acc[k] - m) * 1.2f, 0.0f, 1.0f); }
      glBindFramebuffer(GL_FRAMEBUFFER, b->gfbo);
      glViewport(0, 0, GEN_W, GEN_H);
      glDisable(GL_BLEND);
      glUniform4f(u2_rect, (float)(0.80 + 0.06 * sin(t * 0.314)), (float)(0.20 + 0.06 * cos(t * 0.271)),
                           (float)(0.22 + 0.06 * cos(t * 0.231)), (float)(0.88 + 0.05 * sin(t * 0.293)));
      glUniform4f(u2_aux0, base[0], base[1], base[2], 0);
      glUniform4f(u2_aux1, a[0], a[1], a[2], 0);
      glUniform4f(u2_aux2, acc[0], acc[1], acc[2], 0);
      draw_2d(0, 0, 0, W, H, one, 7);
      glEnable(GL_BLEND);
      glUniform4f(u2_aux0, g_bg_dim, 0, 0, 0);
      draw_2d(0, 0, 0, W, H, one, 8);        /* same scrim as the baked images */
      glBindFramebuffer(GL_FRAMEBUFFER, 0);
      glViewport(0, 0, g_dw, g_dh);
      if (opaque) glDisable(GL_BLEND);
      draw_2d(b->gtex, 0, H, W, -H, c, 0);   /* FBO rows are bottom-up */
   }
   if (opaque) glEnable(GL_BLEND);
}

/* ------------------------------------------------------------------------ */
/* library: tab logos / names and the live rescan                            */
static TTF_Font *g_font_hero, *g_font_tab, *g_font_name;   /* dashboard title, system tabs, profile name */
static void platforms_load_art(void)
{
   int i;
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
      if (p->show_name) p->label = text_tex(g_font_tab ? g_font_tab : g_font_small, p->name, &p->label_w, &p->label_h);
   }
}

static int g_lib_rescan;   /* set by ui_library_changed(), handled at the top of the next frame */

/* a setting that changes which systems are listed (show_stock_games) */
void ui_library_changed(void) { g_lib_rescan = 1; }

/* Re-read the declarations and every library without restarting: GPU resources of
 * the old library are freed, the cover worker's in-flight result is dropped (cv_gen),
 * and the selection follows the same game (system id + content path), else the same
 * system, else the first one. Returns the new (platform, game-in-platform). */
static void library_rescan(int *sel_plat, int *sel)
{
   char sys[32] = "", content[512] = "";
   double t0 = now_s();
   int i, p, k;
   if (*sel_plat >= 0 && *sel_plat < g_nplat)
   {
      snprintf(sys, sizeof(sys), "%s", g_plat[*sel_plat].id);
      if (*sel >= 0 && *sel < g_plat[*sel_plat].count)
         snprintf(content, sizeof(content), "%s", g_games[g_plat[*sel_plat].first + *sel].content);
   }
   for (i = 0; i < g_ngames; i++) game_free_tex(&g_games[i]);
   for (p = 0; p < g_nplat; p++)
   {
      if (g_plat[p].label) glDeleteTextures(1, &g_plat[p].label);
      if (g_plat[p].logo_tex) glDeleteTextures(1, &g_plat[p].logo_tex);
   }
   SDL_LockMutex(cv_mx);
   cv_gen++;
   /* a queued (not started) request or a finished result is for the old indices; one
    * being decoded comes back with the old generation and is dropped on arrival */
   if (cv_req >= 0) { cv_req = -1; cv_busy = 0; }
   if (cv_done >= 0) { if (cv_surf) SDL_FreeSurface(cv_surf); cv_surf = NULL; cv_done = -1; cv_busy = 0; }
   SDL_UnlockMutex(cv_mx);
   g_ngames = 0; g_nplat = 0;
   load_systems();
   scan_all();
   build_platform_index();
   platforms_load_art();
   *sel_plat = 0; *sel = 0;
   for (p = 0; p < g_nplat; p++)
      if (!strcmp(g_plat[p].id, sys))
      {
         *sel_plat = p;
         for (k = 0; k < g_plat[p].count; k++)
            if (!strcmp(g_games[g_plat[p].first + k].content, content)) { *sel = k; break; }
         break;
      }
   for (i = 0; i < g_ngames; i++)   /* the new boxes fly in like at start */
   {
      game_t *g = &g_games[i];
      g->x.x = 6.0f + (i % 8) * 0.6f; g->z.x = -3.0f; g->ry.x = -1.2f; g->sc.x = 1.0f; g->al.x = 0.0f;
   }
   attract_library_changed();
   bg_shown[0] = 0;   /* request the selection again (layers keep their own palette) */
   bg_game = -1;
   logf_("library rescan: games %d, platforms %d, %.0f ms", g_ngames, g_nplat, (now_s() - t0) * 1000);
}


/* ------------------------------------------------------------------------ */
/* dashboard (docs/dashboard-design.md): profile + status bar, system tabs, a row of
 * rounded cover tiles anchored on the selection, title and play statistics          */
/* blocks keep a TV-safe margin: many TVs overscan ~3 % of each edge */
#define DASH_X    84.0f    /* left / right margin of every block */
#define TOP_Y     40.0f    /* profile + status row */
#define TILE      168.0f
#define TILE_SEL  220.0f
#define TILE_GAP  18.0f
#define TILE_Y    206.0f
#define TAB_Y     118.0f
#define INFO_Y    452.0f
#define DASH_PAGE 6        /* games per L1 / R1 page: the tiles visible right of the selection */
static status_t  g_status;
static char      g_menu_music[512];          /* <home>/assets/music/menu.ogg or "" */
static struct { char id[16]; GLuint tex; int w, h; label_t initials; } g_avatar;

/* first letter of up to two words of the name (UTF-8 sequences kept whole) */
static void initials_of(const char *name, char *out, size_t n)
{
   size_t o = 0; int words = 0, start = 1;
   const unsigned char *p = (const unsigned char *)name;
   while (*p && words < 2 && o + 5 < n)
   {
      if (*p == ' ') { start = 1; p++; continue; }
      if (start)
      {
         int len = *p < 0x80 ? 1 : (*p >> 5) == 6 ? 2 : (*p >> 4) == 14 ? 3 : (*p >> 3) == 30 ? 4 : 1, k;
         for (k = 0; k < len && p[k]; k++) out[o++] = (char)(len == 1 ? toupper(p[k]) : p[k]);
         p += k; words++; start = 0;
         continue;
      }
      p++;
   }
   out[o] = 0;
}

/* <home>/profiles/<id>/avatar.png, else initials drawn on the profile colour */
static void avatar_load(const profile_t *p)
{
   char path[400], ini[16];
   SDL_Surface *sf;
   if (g_avatar.tex) glDeleteTextures(1, &g_avatar.tex);
   g_avatar.tex = 0;
   snprintf(g_avatar.id, sizeof(g_avatar.id), "%s", p->id);
   snprintf(path, sizeof(path), "%s/profiles/%s/avatar.png", g_home, p->id);
   if (file_exists(path) && (sf = IMG_Load(path)))
   {
      g_avatar.w = sf->w; g_avatar.h = sf->h;
      g_avatar.tex = surface_to_tex(sf, 1, NULL);
      SDL_FreeSurface(sf);
   }
   initials_of(profiles_display_name(p), ini, sizeof(ini));
   label_set(&g_avatar.initials, g_font_tab, ini);
}

/* horizontal position and size of a tile at offset d from the selection (continuous,
 * so the row slides and the selected tile grows while the selection spring moves) */
static float tile_x(float d)
{
   if (d >= 1.0f) return DASH_X + TILE_SEL + TILE_GAP + (d - 1.0f) * (TILE + TILE_GAP);
   if (d >= 0.0f) return DASH_X + d * (TILE_SEL + TILE_GAP);
   return DASH_X + d * (TILE + TILE_GAP);
}
static float tile_size(float d) { float a = fabsf(d); return a >= 1.0f ? TILE : lerpf(TILE_SEL, TILE, a); }

static void glyph_draw(const char *name, float x, float y, float h, const float c[4])
{
   const icon_t *ic = icon_get(g_home, name);
   if (ic && ic->tex && ic->h > 0) draw_2d(ic->tex, x, y, h * ic->w / (float)ic->h, h, c, 0);
}
static float glyph_w(const char *name, float h)
{
   const icon_t *ic = icon_get(g_home, name);
   return ic && ic->tex && ic->h > 0 ? h * ic->w / (float)ic->h : 0.0f;
}

/* ua: alpha of the whole dashboard UI; pos: selection spring (tile index space);
 * info_a / info_y: title block fade and slide after a selection change */
static void dash_draw(const platform_t *pl, int sel_plat, int n, int sel, float pos, double sel_t, double t,
                      float ua, float info_a, float info_y)
{
   static label_t l_name, l_psub, l_title, l_meta, l_play, l_clock, l_empty;
   const float white[4] = { 0.98f, 0.98f, 0.99f, ua };
   const float dim[4] = { 0.84f, 0.84f, 0.88f, 0.80f * ua };
   char s[320], a1[96], a2[96], num[24];
   int i, k;
   if (ua <= 0.003f) return;
   if (g_dash_profile_dirty)
   {
      g_dash_profile_dirty = 0;
      stats_load(g_home, profiles_active()->id);
      avatar_load(profiles_active());
   }

   /* profile: avatar (or initials on the profile colour) with a thin ring, name, games played */
   {
      const profile_t *ap = profiles_active();
      const float ring[4] = { 1, 1, 1, 0.35f * ua };
      const float ax = DASH_X, ay = TOP_Y, ad = 48;
      float sw[4];
      int played = stats_games_played();
      if (strcmp(g_avatar.id, ap->id)) avatar_load(ap);
      draw_shadow(ax, ay, ad, ad, ad * 0.5f, 10, 3, 0.45f * ua);
      if (g_avatar.tex)
      {
         const float c[4] = { 1, 1, 1, ua };
         draw_tile(g_avatar.tex, g_avatar.w, g_avatar.h, ax, ay, ad, ad, ad * 0.5f, -2.0f, 0.0f, c);
      }
      else
      {
         profiles_color(ap->color, sw); sw[3] = ua;
         draw_shape(ax, ay, ad, ad, ad * 0.5f, 0, sw);
         if (g_avatar.initials.tex)
            draw_2d(g_avatar.initials.tex, floorf(ax + ad * 0.5f - g_avatar.initials.w * 0.5f),
                    floorf(ay + ad * 0.5f - g_avatar.initials.h * 0.5f), g_avatar.initials.w, g_avatar.initials.h, white, 0);
      }
      draw_shape(ax - 3, ay - 3, ad + 6, ad + 6, ad * 0.5f + 3, 1.5f, ring);
      label_set(&l_name, g_font_name ? g_font_name : g_font_sub, profiles_display_name(ap));
      draw_text(&l_name, ax + ad + 16, ay - 1, white);
      snprintf(num, sizeof(num), "%d", played);
      label_set(&l_psub, g_font_small, trf(s, sizeof(s), played == 1 ? "dash.profile_games_one" : "dash.profile_games", "n", num, NULL));
      draw_text(&l_psub, ax + ad + 16, ay + 26, dim);
   }

   /* status, right to left: clock (only when the time is known), network, controllers */
   {
      const float on[4] = { 1, 1, 1, 0.92f * ua }, off[4] = { 1, 1, 1, 0.35f * ua };
      const float cy = TOP_Y + 24;
      float x = W - DASH_X;
      int np = SDL_NumJoysticks();
      if (g_status.clock_ok)
      {
         label_set(&l_clock, g_font_name ? g_font_name : g_font_sub, g_status.clock);
         x -= l_clock.w;
         draw_text(&l_clock, x, cy - l_clock.h * 0.5f, white);
         x -= 26;
      }
      if (g_status.net_up)
      {
         x -= glyph_w("network", 26);
         glyph_draw("network", x, cy - 13, 26, on);
         x -= 22;
      }
      if (np > 4) np = 4;
      for (k = (np > 0 ? np : 1) - 1; k >= 0; k--)   /* no pad: one dim glyph */
      {
         x -= glyph_w("pad", 28);
         glyph_draw("pad", x, cy - 14, 28, np > 0 ? on : off);
         x -= 8;
      }
   }

   /* system tabs: L1 [pills] R1. Each pill is translucent glass with a soft shadow and a
    * hairline border; the selected one is brighter */
   if (g_nplat)
   {  /* a row wider than the safe area scrolls (spring) to keep the selected pill inside,
       * with the L1 / R1 glyphs pinned at the margins */
      static spring off;
      const float ph = 42.0f, lh = 22.0f, padx = 20.0f;
      const float gl[4] = { 1, 1, 1, 0.60f * ua };
      float pws[MAX_PLATFORMS], x0, x, total = 0, sx = 0, sw = 0, l1w = 0, r1w = 0, want = 0, left, right;
      for (i = 0; i < g_nplat; i++)
      {
         platform_t *p = &g_plat[i];
         float cw = 0;
         if (p->logo_tex) cw += lh * p->logo_w / (float)p->logo_h + (p->show_name && p->label ? 9.0f : 0.0f);
         if (p->show_name && p->label) cw += p->label_w;
         pws[i] = cw + 2 * padx;
         if (i == sel_plat) { sx = total; sw = pws[i]; }
         total += pws[i] + (i + 1 < g_nplat ? 12.0f : 0.0f);
      }
      if (g_nplat > 1) { l1w = glyph_w("l2", 24) + 14; r1w = glyph_w("r2", 24) + 14; }
      left = DASH_X + l1w; right = W - DASH_X - r1w;
      if (total > right - left)
      {
         want = off.x;
         if (sx - want < 0) want = sx;
         if (sx + sw - want > right - left) want = sx + sw - (right - left);
         want = clampf(want, 0.0f, total - (right - left));
      }
      spring_step(&off, want, 200, 22, 1.0f / 60.0f);
      x0 = left - off.x;
      if (g_nplat > 1) glyph_draw("l2", DASH_X, TAB_Y + (ph - 24) * 0.5f, 24, gl);
      if (total > right - left)
      {
         glEnable(GL_SCISSOR_TEST);
         gl_scissor(left - 16, TAB_Y - 16, right - left + 32, ph + 40);
      }
      x = x0;
      for (i = 0; i < g_nplat; i++)
      {
         platform_t *p = &g_plat[i];
         const int on_ = i == sel_plat;
         float cx, pw = pws[i];
         float fill[4] = { 1, 1, 1, (on_ ? 0.24f : 0.08f) * ua }, edge[4] = { 1, 1, 1, (on_ ? 0.45f : 0.16f) * ua };
         const float c[4] = { 1, 1, 1, (on_ ? 1.0f : 0.70f) * ua };
         draw_shadow_ex(x, TAB_Y, pw, ph, ph * 0.5f, 12, 4, (on_ ? 0.45f : 0.30f) * ua, 1);
         draw_shape(x, TAB_Y, pw, ph, ph * 0.5f, 0, fill);
         draw_shape(x, TAB_Y, pw, ph, ph * 0.5f, 1.0f, edge);
         cx = x + padx;
         if (p->logo_tex)
         {
            float lw = lh * p->logo_w / (float)p->logo_h;
            draw_2d(p->logo_tex, floorf(cx), floorf(TAB_Y + (ph - lh) * 0.5f), lw, lh, c, 0);
            cx += lw + 9.0f;
         }
         if (p->show_name && p->label)
            draw_2d(p->label, floorf(cx), floorf(TAB_Y + (ph - p->label_h) * 0.5f), p->label_w, p->label_h, c, 0);
         x += pw + 12;
      }
      glDisable(GL_SCISSOR_TEST);
      if (g_nplat > 1)
         glyph_draw("r2", total > right - left ? W - DASH_X - glyph_w("r2", 24) : x + 2, TAB_Y + (ph - 24) * 0.5f, 24, gl);
   }

   /* tiles: soft shadows first, then the covers; tiles left of the selection fade away */
   if (n)
   {
      int base = (int)floorf(pos);
      float st = (float)(t - sel_t), sheen = -2.0f;
      if (st > 0.35f)
      {  /* a light sweep over the selected tile shortly after it settles, then every 6 s */
         float u = fmodf(st - 0.35f, 6.0f);
         if (u < 1.0f) sheen = -0.6f + u * 2.4f;
      }
      for (k = -2; k <= 8; k++)
      {
         int idx = base + k;
         float d, x, sz, ta;
         game_t *g;
         if (idx < 0 || idx >= n) continue;
         if (!g_fx_sides && idx != sel) continue;
         d = idx - pos; x = tile_x(d); sz = tile_size(d);
         ta = ua * (d < 0.0f ? clampf(1.0f + d * 1.8f, 0.0f, 1.0f) : 1.0f);
         if (ta <= 0.003f || x > W || x + sz < 0) continue;
         g = &g_games[pl->first + idx];
         /* only the focused tile casts a shadow: the others sit on dimmed art where it
          * barely shows, and seven blurred quads cost ~5 ms a frame on the console */
         if (idx == sel && g_fx_shadows)
         {
            const float sc_[4] = { 0, 0, 0, 0.60f * ta };
            float b = 22.0f * sz / 220.0f;
            draw_2d(g_tile_shadow, x - b, TILE_Y - b + 10, sz + 2 * b, sz + 2 * b, sc_, 0);
         }
         if (g->tex)
         {
            const float c[4] = { 1, 1, 1, ta };
            draw_tile(g->tex, g->tex_w, g->tex_h, x, TILE_Y, sz, sz, 14, idx == sel ? sheen : -2.0f, idx == sel ? 1.0f : 0.0f, c);
         }
         else
         {
            const float c[4] = { 0.16f, 0.16f, 0.20f, 0.7f * ta };
            draw_shape(x, TILE_Y, sz, sz, 14, 0, c);   /* (its shadow above is not cut: barely visible) */
         }
         if (idx == sel)
         {  /* focus ring (fades in with the title) and the media badge */
            const float ring[4] = { 1, 1, 1, 0.95f * ta * info_a }, badge[4] = { 0.04f, 0.04f, 0.06f, 0.70f * ta };
            const float bedge[4] = { 1, 1, 1, 0.22f * ta };
            const char *mg = pl->kind == KIND_CART ? "cartridge" : (pl->kind == KIND_ARCADE ? "coin" : "disc");
            const float gc[4] = { 1, 1, 1, 0.95f * ta };
            draw_shape(x - 7, TILE_Y - 7, sz + 14, sz + 14, 20, 3.0f, ring);
            draw_shape(x + sz - 50, TILE_Y + sz - 50, 40, 40, 20, 0, badge);
            draw_shape(x + sz - 50, TILE_Y + sz - 50, 40, 40, 20, 1.0f, bedge);
            glyph_draw(mg, x + sz - 43, TILE_Y + sz - 43, 26, gc);
         }
      }
   }

   /* title, metadata, play statistics of the selection */
   if (n)
   {
      const game_t *cg = &g_games[pl->first + sel];
      const game_stats_t *gs = stats_get(cg->content);
      const float c1[4] = { 0.99f, 0.99f, 1.0f, info_a * ua }, c2[4] = { 0.90f, 0.90f, 0.93f, 0.92f * info_a * ua };
      const float c3[4] = { 0.84f, 0.84f, 0.88f, 0.78f * info_a * ua };
      float sc, y = INFO_Y + info_y;
      label_set(&l_title, g_font_hero, cg->title);
      sc = l_title.w > W - 2 * DASH_X ? (W - 2 * DASH_X) / (float)l_title.w : 1.0f;
      if (sc < 1.0f)
      {
         const float sh[4] = { 0, 0, 0, 0.45f * c1[3] };
         draw_2d(l_title.tex, DASH_X + 1, y + 2, l_title.w * sc, l_title.h * sc, sh, 0);
         draw_2d(l_title.tex, DASH_X, y, l_title.w * sc, l_title.h * sc, c1, 0);
      }
      else draw_text(&l_title, DASH_X, y, c1);
      y += l_title.h * sc + 4 + info_y * 0.4f;
      s[0] = 0;
      if (cg->year[0]) snprintf(s, sizeof(s), "%s", cg->year);
      if (cg->publisher[0]) { size_t l = strlen(s); snprintf(s + l, sizeof(s) - l, "%s%s", l ? "   \xC2\xB7   " : "", cg->publisher); }
      if (cg->players[0])
      {
         size_t l = strlen(s);
         trf(a1, sizeof(a1), strcmp(cg->players, "1") ? "card.players" : "card.players_one", "n", cg->players, NULL);
         snprintf(s + l, sizeof(s) - l, "%s%s", l ? "   \xC2\xB7   " : "", a1);
      }
      label_set(&l_meta, g_font_sub, s);
      draw_text(&l_meta, DASH_X, y, c2);
      y += (l_meta.h ? l_meta.h : 24) + 6;
      if (gs && gs->seconds > 0)
      {
         trf(s, sizeof(s), "dash.played", "time", stats_format_duration(a1, sizeof(a1), gs->seconds), NULL);
         if (gs->last_epoch > 0)
         {
            size_t l = strlen(s);
            snprintf(s + l, sizeof(s) - l, "   \xC2\xB7   %s",
                     trf(a2, sizeof(a2), "dash.last", "date", stats_format_date(a1, sizeof(a1), gs->last_epoch), NULL));
         }
      }
      else snprintf(s, sizeof(s), "%s", tr("dash.never"));
      label_set(&l_play, g_font_small, s);
      draw_text(&l_play, DASH_X, y, c3);
   }
   else
   {
      label_set(&l_empty, g_font_hero, tr("empty.library"));
      draw_text(&l_empty, DASH_X, INFO_Y, white);
   }
}

/* ------------------------------------------------------------------------ */
/* test hooks (tools/host): scripted input and screenshots, driven by environment
 * variables, inert when unset.
 *   LUMEN_INPUT="2:right;2.5:right;4:select;6:back;9:quit"  logical buttons at seconds
 *     (left right up down l1 r1 play back square options start select quit)
 *   LUMEN_SHOT="/out/a.ppm@3;/out/b.ppm@7"                   framebuffer dumps (PPM) */
typedef struct { double at; int btn; char path[200]; int done; } test_ev_t;
static test_ev_t g_tin[64], g_tshot[16];
static int       g_ntin, g_ntshot;

static int test_btn(const char *n)
{
   static const char *names[] = { "left", "right", "up", "down", "l1", "r1", "l2", "r2", "play", "back", "square", "start", "select", "options" };
   static const int ids[] = { IN_LEFT, IN_RIGHT, IN_UP, IN_DOWN, IN_L1, IN_R1, IN_L2, IN_R2, IN_PLAY, IN_BACK, IN_SQUARE, IN_START, IN_SELECT, IN_OPTIONS };
   int i;
   if (!strcmp(n, "quit")) return -2;
   for (i = 0; i < (int)(sizeof(ids) / sizeof(ids[0])); i++) if (!strcmp(n, names[i])) return ids[i];
   return -1;
}

static void test_parse(void)
{
   char buf[2048], *tok, *save = NULL;
   const char *e = getenv("LUMEN_INPUT");
   if (e)
   {
      snprintf(buf, sizeof(buf), "%s", e);
      for (tok = strtok_r(buf, ";", &save); tok && g_ntin < 64; tok = strtok_r(NULL, ";", &save))
      {
         char *c = strchr(tok, ':');
         if (!c) continue;
         *c = 0;
         g_tin[g_ntin].at = atof(tok); g_tin[g_ntin].btn = test_btn(c + 1);
         if (g_tin[g_ntin].btn != -1) g_ntin++;
      }
   }
   save = NULL;
   if ((e = getenv("LUMEN_SHOT")))
   {
      snprintf(buf, sizeof(buf), "%s", e);
      for (tok = strtok_r(buf, ";", &save); tok && g_ntshot < 16; tok = strtok_r(NULL, ";", &save))
      {
         char *c = strrchr(tok, '@');
         if (!c) continue;
         *c = 0;
         snprintf(g_tshot[g_ntshot].path, sizeof(g_tshot[g_ntshot].path), "%s", tok);
         g_tshot[g_ntshot++].at = atof(c + 1);
      }
   }
   g_cost_on = getenv("LUMEN_COST") != NULL;
   if (g_ntin || g_ntshot) logf_("test: %d inputs, %d screenshots", g_ntin, g_ntshot);
}

/* press scripted buttons for one frame (released on the next); returns 0 on "quit" */
static int test_input(double ts)
{
   static int held[16], nheld;
   int i, run = 1;
   for (i = 0; i < nheld; i++) in_set(held[i], 0);
   nheld = 0;
   for (i = 0; i < g_ntin; i++)
      if (!g_tin[i].done && ts >= g_tin[i].at)
      {
         g_tin[i].done = 1;
         if (g_tin[i].btn == -2) run = 0;
         else { in_set(g_tin[i].btn, 1); if (nheld < 16) held[nheld++] = g_tin[i].btn; }
      }
   return run;
}

/* dump the back buffer (before the swap) as a binary PPM */
static void test_shots(double ts)
{
   int i, y;
   for (i = 0; i < g_ntshot; i++)
      if (!g_tshot[i].done && ts >= g_tshot[i].at)
      {
         unsigned char *px = malloc((size_t)g_dw * g_dh * 4), *row;
         FILE *f;
         g_tshot[i].done = 1;
         if (!px) continue;
         glReadPixels(0, 0, g_dw, g_dh, GL_RGBA, GL_UNSIGNED_BYTE, px);
         if ((f = fopen(g_tshot[i].path, "wb")))
         {
            fprintf(f, "P6\n%d %d\n255\n", g_dw, g_dh);
            row = malloc((size_t)g_dw * 3);
            for (y = g_dh - 1; row && y >= 0; y--)
            {
               int x;
               for (x = 0; x < g_dw; x++) memcpy(row + x * 3, px + ((size_t)y * g_dw + x) * 4, 3);
               fwrite(row, 3, g_dw, f);
            }
            free(row);
            fclose(f);
            logf_("test: screenshot %s", g_tshot[i].path);
         }
         free(px);
      }
}

/* ---- in-game dialog (lumen --dialog game | discs:<count>:<current>, run by lumen-buttons
 * on OPEN while RetroArch is paused): the game's cover as a dimmed backdrop, a card with
 * the cover, the title and the choices, the dashboard's help bar. Exit code = the answer:
 * 0 continue, 2 exit the game, 10 + k switch to disc k (1-based). Circle = continue. */
static void dialog_launch_info(char *title, size_t tn, char *cover, size_t cn)
{
   char line[700], content[600] = "";
   FILE *f = fopen("/tmp/lumen-launch", "r");
   title[0] = cover[0] = 0;
   if (!f) return;
   while (fgets(line, sizeof(line), f))
   {
      line[strcspn(line, "\r\n")] = 0;
      if (!strncmp(line, "title=", 6)) snprintf(title, tn, "%s", line + 6);
      else if (!strncmp(line, "content=", 8)) snprintf(content, sizeof(content), "%s", line + 8);
   }
   fclose(f);
   if (content[0])
   {
      char *dot = strrchr(content, '.'), *sl = strrchr(content, '/');
      if (dot && (!sl || dot > sl)) *dot = 0;
      snprintf(cover, cn, "%s.png", content);
      if (!file_exists(cover) && sl) { *sl = 0; snprintf(cover, cn, "%s/disc.png", content); }
      if (!file_exists(cover)) cover[0] = 0;
   }
}

static int run_dialog(SDL_Window *win, const char *kind)
{
   enum { MAXROW = 12 };
   label_t kicker = { 0 }, title = { 0 }, sect = { 0 }, row_l[MAXROW], tag = { 0 }, h_ok = { 0 }, h_back = { 0 };
   int ans[MAXROW], sect_before = -1, n = 0, sel = 0, answer = -1, i, discs = 1, cur = 1;
   char gtitle[256], cover_path[700], buf[64];
   GLuint cover = 0; int cw_ = 0, chh = 0;
   const icon_t *ic_ok = icon_get(g_home, "cross"), *ic_back = icon_get(g_home, "circle");
   spring pill = { 0 }, fade = { 0 };
   double t0 = now_s(), tp = t0;

   if (!strncmp(kind, "discs:", 6) && sscanf(kind + 6, "%d:%d", &discs, &cur) >= 1)
   { if (discs < 1) discs = 1; if (discs > MAXROW - 2) discs = MAXROW - 2; if (cur < 1 || cur > discs) cur = 1; }
   memset(row_l, 0, sizeof(row_l));
   /* rows: Continue, [discs], Exit */
   label_set(&row_l[n], g_font_name, tr("dialog.continue")); ans[n++] = 0;
   if (discs > 1)
   {
      sect_before = n;
      for (i = 1; i <= discs; i++)
      {
         snprintf(buf, sizeof(buf), "%s %d", tr("dialog.disc"), i);
         label_set(&row_l[n], g_font_name, buf); ans[n++] = i == cur ? 0 : 10 + i;
      }
   }
   label_set(&row_l[n], g_font_name, tr("dialog.exit")); ans[n++] = 2;

   dialog_launch_info(gtitle, sizeof(gtitle), cover_path, sizeof(cover_path));
   if (cover_path[0])
   {
      SDL_Surface *sf = IMG_Load(cover_path);
      if (sf) { cw_ = sf->w; chh = sf->h; cover = surface_to_tex(sf, 1, NULL); SDL_FreeSurface(sf); }
   }
   label_set(&kicker, g_font_small, tr("dialog.title"));
   label_set(&title, g_font_title, gtitle[0] ? gtitle : tr("dialog.title"));
   label_set(&sect, g_font_small, tr("dialog.change_disc"));
   label_set(&tag, g_font_small, tr("dialog.in_use"));
   label_set(&h_ok, g_font_small, tr("dialog.choose"));
   label_set(&h_back, g_font_small, tr("dialog.back"));
   logf_("dialog %s (discs %d, current %d, cover %s)", kind, discs, cur, cover ? "yes" : "no");

   while (answer < 0)
   {
      SDL_Event e;
      ui_nav_t nv;
      double t = now_s();
      float dt = (float)(t - tp); tp = t;
      memset(in_pressed, 0, sizeof(in_pressed));
      while (SDL_PollEvent(&e)) { if (e.type == SDL_QUIT) answer = 0; else in_event(&e); }
      nav_fill(&nv, t);
      if (t - t0 > 0.25)   /* ignore the press still held from the game */
      {
         if (nv.up && sel > 0) sel--;
         if (nv.down && sel < n - 1) sel++;
         if (nv.confirm) answer = ans[sel];
         else if (nv.back || nv.options) answer = 0;
      }
      spring_step(&fade, 1.0f, 60, 14, dt);
      {
         /* geometry: header 132, rows 56, a section label before the discs */
         const float RW = 56, cwid = 640, head = 140, pad = 28;
         float chgt = head + n * RW + (sect_before >= 0 ? 34 : 0) + pad, cx = (W - cwid) * 0.5f, cy = (H - chgt) * 0.5f - 18;
         float a = clampf(fade.x, 0, 1), rowy[MAXROW], y = cy + head;
         const float white[4] = { 1, 1, 1, a }, dim[4] = { 0.70f, 0.73f, 0.80f, a }, acc[4] = { 0.36f, 0.60f, 1.0f, a };
         const float card[4] = { 0.10f, 0.12f, 0.16f, 0.92f * a }, pillc[4] = { 0.23f, 0.51f, 0.96f, a };
         const float scrim[4] = { 0.016f, 0.024f, 0.031f, 0.86f }, red[4] = { 1.0f, 0.55f, 0.55f, a };
         for (i = 0; i < n; i++) { if (i == sect_before) y += 34; rowy[i] = y; y += RW; }
         if (t - t0 < 0.02) pill.x = rowy[sel];
         spring_step(&pill, rowy[sel], 260, 26, dt);

         glViewport(0, 0, g_dw, g_dh);
         glClearColor(0.016f, 0.024f, 0.031f, 1.0f);
         glClear(GL_COLOR_BUFFER_BIT);
         glDisable(GL_DEPTH_TEST);
         glEnable(GL_BLEND);
         glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
         gl_use_2d();
         if (cover)   /* backdrop: the cover filling the screen, dimmed */
         {
            float s2 = fmaxf((float)W / cw_, (float)H / chh), bw = cw_ * s2, bh = chh * s2;
            const float bc[4] = { 1, 1, 1, 0.55f };
            draw_2d(cover, (W - bw) * 0.5f, (H - bh) * 0.5f, bw, bh, bc, 0);
         }
         draw_2d(0, 0, 0, W, H, scrim, 0);
         draw_shadow(cx, cy, cwid, chgt, 28, 40, 14, 0.55f * a);
         draw_round_rect(cx, cy, cwid, chgt, 28, card, 0.10f * a);
         /* header: cover thumbnail, kicker, title */
         {
            float tx = cx + pad;
            if (cover)
            {
               float th = 92, tw = th * cw_ / (float)chh;
               if (tw > 92) { tw = 92; th = tw * chh / (float)cw_; }
               draw_shadow(cx + pad, cy + pad, tw, th, 10, 10, 3, 0.45f * a);
               draw_2d(cover, cx + pad, cy + pad, tw, th, white, 0);
               tx += tw + 22;
            }
            if (kicker.tex) draw_2d(kicker.tex, tx, cy + pad + 8, kicker.w, kicker.h, acc, 0);
            if (title.tex)
            {
               float maxw = cx + cwid - pad - tx, tw2 = title.w > maxw ? maxw : title.w;
               draw_2d(title.tex, tx, cy + pad + 8 + kicker.h + 4, tw2, title.h, white, 0);
            }
         }
         /* separator, section label, selection pill, rows */
         {
            const float sep[4] = { 1, 1, 1, 0.08f * a };
            draw_2d(0, cx + pad, cy + head - 14, cwid - 2 * pad, 1, sep, 0);
         }
         if (sect_before >= 0 && sect.tex) draw_2d(sect.tex, cx + pad + 18, rowy[sect_before] - 28, sect.w, sect.h, dim, 0);
         draw_shadow(cx + pad, pill.x + 3, cwid - 2 * pad, RW - 6, (RW - 6) * 0.5f, 10, 3, 0.35f * a);
         draw_round_rect(cx + pad, pill.x + 3, cwid - 2 * pad, RW - 6, (RW - 6) * 0.5f, pillc, 0.0f);
         for (i = 0; i < n; i++)
         {
            const float *c = (ans[i] == 2 && i != sel) ? red : (i == sel ? white : dim);
            if (row_l[i].tex) draw_2d(row_l[i].tex, cx + pad + 24, rowy[i] + (RW - row_l[i].h) * 0.5f, row_l[i].w, row_l[i].h, c, 0);
            if (sect_before >= 0 && i == sect_before + cur - 1 && tag.tex)
            {
               const float tg[4] = { 1, 1, 1, (i == sel ? 0.85f : 0.55f) * a };
               draw_2d(tag.tex, cx + cwid - pad - 24 - tag.w, rowy[i] + (RW - tag.h) * 0.5f, tag.w, tag.h, tg, 0);
            }
         }
         /* help bar */
         {
            float ih = 26, hy = H - 58, wok = (ic_ok && ic_ok->tex ? ih + 8 : 0) + h_ok.w, wbk = (ic_back && ic_back->tex ? ih + 8 : 0) + h_back.w;
            float hx = (W - (wok + 36 + wbk)) * 0.5f;
            if (ic_ok && ic_ok->tex) { draw_2d(ic_ok->tex, hx, hy, ih, ih, white, 0); hx += ih + 8; }
            if (h_ok.tex) { draw_2d(h_ok.tex, hx, hy + (ih - h_ok.h) * 0.5f, h_ok.w, h_ok.h, dim, 0); hx += h_ok.w + 36; }
            if (ic_back && ic_back->tex) { draw_2d(ic_back->tex, hx, hy, ih, ih, white, 0); hx += ih + 8; }
            if (h_back.tex) draw_2d(h_back.tex, hx, hy + (ih - h_back.h) * 0.5f, h_back.w, h_back.h, dim, 0);
         }
      }
      SDL_GL_SwapWindow(win);
   }
   logf_("dialog answer %d", answer);
   return answer;
}

int main(int argc, char **argv)
{
   char font_b[300], font_r[300], font_m[300], font_sb[300], font_b_old[300], font_r_old[300], padb[300];
   const char *fonts_bold[] = { font_b, font_b_old, "/usr/share/fonts/ttf/LiberationSans-Bold.ttf", NULL };
   const char *fonts_reg[]  = { font_r, font_r_old, "/usr/share/fonts/ttf/LiberationSans-Regular.ttf", NULL };
   /* Outfit ships four weights (assets/README.md); medium / semibold fall back to bold */
   const char *fonts_med[]  = { font_m, font_b, font_b_old, "/usr/share/fonts/ttf/LiberationSans-Bold.ttf", NULL };
   const char *fonts_semi[] = { font_sb, font_b, font_b_old, "/usr/share/fonts/ttf/LiberationSans-Bold.ttf", NULL };
   const char *log_path = NULL, *dialog = NULL;
   int start_attract = 0, focus_demo = 0, home_plat = 0, home_sel = 0, att_prev = 0;
   double last_activity = 0;
   attract_out_t att;
   SDL_Window *win; SDL_GLContext ctx;
   int win_w = W, win_h = H;
   double b_draw = 0, b_swap = 0;   /* bench: time issuing frames / blocked in the swap */
   SDL_GameController *pad = NULL;
   double t0 = now_s(), last, fps_t, info_t = 0, play_t = -1;
   float play_pt = 0.0f;   /* launch clock: the sum of the (clamped, <= 50 ms) frame times since
                            * Play, so a slow frame never skips part of the animation (it was
                            * t - play_t: a 560 ms stall on the first frame jumped 0.6 s ahead) */
   int i, sel_plat = 0, sel = 0, running = 1, exit_code = 0, frames = 0;
   int held_dir = 0; double held_next = 0, sel_t = 0;
   int start_down = 0, select_down = 0;
   spring bg[3], info_y, info_a, plat_x, dpos;
   label_t l_start = { 0 }, l_coin1 = { 0 }, l_coin2 = { 0 };
   float fade = 1.0f; /* 1 = black, fades in at start */

   for (i = 1; i < argc; i++)
   {
      if (!strcmp(argv[i], "--root") && i + 1 < argc) snprintf(g_root, sizeof(g_root), "%s", argv[++i]);
      else if (!strcmp(argv[i], "--log") && i + 1 < argc) log_path = argv[++i];
      else if (!strcmp(argv[i], "--home") && i + 1 < argc) snprintf(g_home, sizeof(g_home), "%s", argv[++i]);
      else if (!strcmp(argv[i], "--attract")) start_attract = 1;
      else if (!strcmp(argv[i], "--focus-demo")) focus_demo = 1;
      else if (!strcmp(argv[i], "--dialog") && i + 1 < argc) dialog = argv[++i];
   }
   snprintf(g_core_dir, sizeof(g_core_dir), "%s/cores", g_home);
   snprintf(g_state, sizeof(g_state), "%s/state.txt", g_home);
   snprintf(font_b, sizeof(font_b), "%s/assets/fonts/bold.ttf", g_home);
   snprintf(font_r, sizeof(font_r), "%s/assets/fonts/regular.ttf", g_home);
   snprintf(font_m, sizeof(font_m), "%s/assets/fonts/medium.ttf", g_home);
   snprintf(font_sb, sizeof(font_sb), "%s/assets/fonts/semibold.ttf", g_home);
   snprintf(font_b_old, sizeof(font_b_old), "%s/fonts/bold.ttf", g_home);      /* old layout */
   snprintf(font_r_old, sizeof(font_r_old), "%s/fonts/regular.ttf", g_home);
   snprintf(padb, sizeof(padb), "%s/gamecontrollerdb.txt", g_home);
   if (log_path) g_log = fopen(log_path, "w");
   atexit(log_sync_atexit);
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
   /* 24-bit colour, no alpha (setting gl_rgb, 565 to compare): SDL 2.0.4 asks for
    * 3/3/2 bits by default and takes the smallest config, RGB565. That bands gradients,
    * and Weston can only hand a fullscreen buffer straight to the display (no
    * recomposition) when its format matches the output's XRGB8888. */
   if (settings_get_int("gl_rgb", 888, 565, 888) != 565)
   {
      SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8); SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
      SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8); SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 0);
   }
   else
   {
      SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 5); SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 6);
      SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 5); SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 0);
   }
   {
      int ms = settings_get_int("msaa", 2, 0, 4);   /* 2x: edges of text, glyphs and the 3D launch */
      SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, ms > 0);
      SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, ms);
   }
   {  /* the window takes the output's size: under a compositor FULLSCREEN_DESKTOP does
       * it anyway, without one (psc-sim's Xvfb) the requested size is what you get */
      SDL_DisplayMode dm;
      if (SDL_GetDesktopDisplayMode(0, &dm) == 0 && dm.w > 0 && dm.h > 0) { win_w = dm.w; win_h = dm.h; }
   }
   win = SDL_CreateWindow("lumen", 0, 0, win_w, win_h, SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN_DESKTOP);
   ctx = win ? SDL_GL_CreateContext(win) : NULL;
   if (!ctx)
   {
      logf_("MSAA unavailable (%s), retrying without", SDL_GetError());
      if (win) SDL_DestroyWindow(win);
      SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 0);
      SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 0);
      win = SDL_CreateWindow("lumen", 0, 0, win_w, win_h, SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN_DESKTOP);
      if (!win) { logf_("CreateWindow: %s", SDL_GetError()); return 1; }
      ctx = SDL_GL_CreateContext(win);
      if (!ctx) { logf_("GL context: %s", SDL_GetError()); return 1; }
   }
   {
      GLint samples = 0;
      glGetIntegerv(GL_SAMPLES, &samples);
      GLint cr = 0, cg = 0, cb = 0, ca = 0, cd = 0;
      glGetIntegerv(GL_RED_BITS, &cr); glGetIntegerv(GL_GREEN_BITS, &cg); glGetIntegerv(GL_BLUE_BITS, &cb);
      glGetIntegerv(GL_ALPHA_BITS, &ca); glGetIntegerv(GL_DEPTH_BITS, &cd);
      logf_("MSAA samples: %d", samples);
      logf_("egl config: R%d G%d B%d A%d D%d", cr, cg, cb, ca, cd);
   }
   SDL_GL_SetSwapInterval(1);
   SDL_ShowCursor(SDL_DISABLE);
   SDL_GL_GetDrawableSize(win, &g_dw, &g_dh);
   if (g_dw <= 0 || g_dh <= 0) { g_dw = W; g_dh = H; }
   g_ui_scale = g_dh / (float)H;
   logf_("output %dx%d, ui scale %.3f", g_dw, g_dh, g_ui_scale);
   logf_("GL %s | %s", glGetString(GL_VERSION), glGetString(GL_RENDERER));
   gl_setup();
   make_fx_textures();
   make_tile_shadow();
   bg_mx = SDL_CreateMutex();
   bg_cv = SDL_CreateCond();
   SDL_CreateThread(bg_worker, "bg", NULL);
   cv_mx = SDL_CreateMutex();
   cv_cv = SDL_CreateCond();
   SDL_CreateThread(cover_worker, "covers", NULL);
   dc_mx = SDL_CreateMutex();
   dc_cv = SDL_CreateCond();
   SDL_CreateThread(disc_worker, "disc", NULL);

   g_font_title = open_font(fonts_semi, 34);
   g_font_sub   = open_font(fonts_reg, 21);
   g_font_small = open_font(fonts_reg, 17);
   g_font_cover = open_font(fonts_bold, 22);
   g_font_hero  = open_font(fonts_semi, 50);
   g_font_tab   = open_font(fonts_med, 19);
   g_font_name  = open_font(fonts_med, 21);
   if (dialog)   /* in-game dialog: no library, no music */
   {
      g_ortho = m4_ortho(0, W, H, 0);
      return run_dialog(win, dialog);
   }

   load_systems();
   scan_all();
   build_platform_index();
   logf_("games: %d, platforms: %d, scan %.0f ms", g_ngames, g_nplat, (now_s() - t0) * 1000);
   platforms_load_art();

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
   memset(&dpos, 0, sizeof(dpos)); dpos.x = (float)sel;
   asset_path(g_menu_music, sizeof(g_menu_music), "music/menu.ogg", "music/menu.ogg");
   if (!file_exists(g_menu_music)) g_menu_music[0] = 0;
   if (music_init() == 0) ui_sound_changed();
   hints_load(g_home);
   panel_init();
   welcome_init();
   attract_init(start_attract, g_home);
   memset(&att, 0, sizeof(att));

   /* first frame positions: start off to the right so they fly in */
   for (i = 0; i < g_ngames; i++)
   {
      game_t *g = &g_games[i];
      g->x.x = 6.0f + (i % 8) * 0.6f; g->z.x = -3.0f; g->ry.x = -1.2f; g->sc.x = 1.0f; g->al.x = 0.0f;
   }

   launch_warmup();
   last = fps_t = now_s();
   logf_("startup %.0f ms, rss %ld kB", (last - t0) * 1000, rss_kb());
   test_parse();

   while (running)
   {
      SDL_Event e; double t = now_s(); float dt = (float)(t - last);
      double t_upd = t, t_pre = t;   /* launch trace: end of the update, start of the swap */
      const double lp_px = g_cost_px_opaque + g_cost_px_blend, lp_heavy = g_cost_px_heavy;
      platform_t *pl;
      if (g_lib_rescan && play_t < 0)
      {
         g_lib_rescan = 0;
         library_rescan(&sel_plat, &sel);
         home_plat = sel_plat; home_sel = sel; sel_t = t;
         info_y.x = 18.0f; info_a.x = 0.0f;
      }
      pl = g_nplat ? &g_plat[sel_plat] : NULL;
      int n = pl ? pl->count : 0, move = 0, plat_move = 0, page = 0, play = 0, att_swallow, restore = -1;
      if (dt > 0.05f) dt = 0.05f;
      last = t;

      memset(in_pressed, 0, sizeof(in_pressed));
      while (SDL_PollEvent(&e))
      {
         if (e.type == SDL_QUIT) running = 0;
         else in_event(&e);
      }
      if (g_ntin && !test_input(t - t0)) running = 0;
      if (g_want_reboot) { exit_code = 12; running = 0; }
      {  /* screensaver / attract scene: any input wakes it and is swallowed (10.2) */
         int was = attract_active();
         if (in_last_activity != last_activity) { last_activity = in_last_activity; attract_note_input(t); }
         attract_update(t, dt, !panel_active() && !welcome_active() && play_t < 0 && fade < 0.02f && g_ngames > 0, &att);
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
         /* L1 / R1: previous / next page of games; L2 / R2 (and up / down): system */
         if (in_pressed[IN_L1]) page = -1;
         if (in_pressed[IN_R1]) page = 1;
         if (in_pressed[IN_L2] || in_pressed[IN_UP]) plat_move = -1;
         if (in_pressed[IN_R2] || in_pressed[IN_DOWN]) plat_move = 1;
         if (in_pressed[IN_LEFT]) { move = -1; held_next = t + 0.32; }
         if (in_pressed[IN_RIGHT]) { move = 1; held_next = t + 0.32; }
      }
      held_dir = in_down[IN_LEFT] ? -1 : (in_down[IN_RIGHT] ? 1 : 0);
      (void)start_down; (void)select_down; (void)pad;
      if (welcome_active())
      {  /* first-run presentation: owns the input until it is closed */
         ui_nav_t wn;
         nav_fill(&wn, t);
         if (att_swallow) memset(&wn, 0, sizeof(wn));
         welcome_update(&wn, dt);
         memset(in_pressed, 0, sizeof(in_pressed));
         g_want_settings = 0; held_dir = 0;
         move = plat_move = page = play = 0;
      }
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
         if (was_open || panel_active()) { move = plat_move = page = play = 0; held_dir = 0; }
      }
      if (play_t >= 0) { move = plat_move = page = play = 0; }
      if (att_swallow) { move = plat_move = page = play = 0; held_dir = 0; held_next = t + 0.32; }
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
      if (n > 1 && page)
      {  /* a page is what the row shows to the right of the selection; from the last
          * (first) game the next (previous) page wraps to the other end */
         int ns = page > 0 ? (sel == n - 1 ? 0 : sel + DASH_PAGE) : (sel == 0 ? n - 1 : sel - DASH_PAGE);
         ns = ns < 0 ? 0 : (ns > n - 1 ? n - 1 : ns);
         if (ns != sel) { sel = ns; info_y.x = 18.0f; info_a.x = 0.0f; sel_t = t; }
      }
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
         dpos.x = (float)sel - 0.6f * plat_move; dpos.v = 0;   /* the new row slides in a little */
      }
      if (n && play && play_t < 0)
      {
         game_t *pg = &g_games[pl->first + sel];
         play_t = t;
         logf_("play: %s", pg->content);
         /* the 3D media of the game comes out of the dashboard towards the camera */
         pg->x.x = -1.6f; pg->x.v = 0; pg->z.x = -1.4f; pg->z.v = 0; pg->ry.x = 0.7f; pg->ry.v = 0;
         pg->sc.x = 0.9f; pg->al.x = 0.0f; pg->al.v = 0;
         disc_want(pg->disc);   /* normally decoded already (selection settled) */
         play_pt = 0.0f;
      }
      /* disc art of the settled selection, ready before Play */
      if (n && pl->kind == KIND_DISC && play_t < 0 && t - sel_t > 0.4) disc_want(g_games[pl->first + sel].disc);
      disc_poll_upload();

      /* background: once the selection has settled, the game's image (decoded on the
       * worker) or a background generated from its cover colours */
      if (n && t - sel_t > 0.25)
      {
         const game_t *cg = &g_games[pl->first + sel];
         char key[512];
         if (cg->bg[0]) snprintf(key, sizeof(key), "%s", cg->bg);
         else snprintf(key, sizeof(key), "gen:%d", pl->first + sel);
         /* a generated one waits for the cover (its palette), the placeholder counts */
         if (strcmp(key, bg_shown) && (cg->bg[0] || cg->tex_state == 1 || cg->tex_state == -1))
         {
            snprintf(bg_shown, sizeof(bg_shown), "%s", key);
            bg_game = pl->first + sel;
            if (cg->bg[0]) bg_request(cg->bg);
            else bg_push(bg_gen_layer(bg_game));
         }
      }
      bg_poll_upload();
      bg_advance(dt);

      /* music: the game's track once the selection has settled 0.8 s, else the menu
       * theme; the attract scene keeps the menu theme; launching fades it out */
      {
         const char *want = NULL;
         float fade_s = 1.0f;
         if (!settings_get_int("music", 1, 0, 1)) want = "";
         else if (play_t >= 0) { want = ""; fade_s = 0.6f; }
         else if (attract_active()) { want = g_menu_music; fade_s = 1.5f; }
         else if (n && t - sel_t > 0.8)
         {
            const game_t *cg = &g_games[pl->first + sel];
            want = cg->music[0] ? cg->music : g_menu_music;
         }
         else if (!n) want = g_menu_music;
         if (want) music_play(want, fade_s);
         music_duck(0.55f * panel_amount());
      }
      status_update(t, &g_status);

      /* cover streaming: decoded on the worker, nearest to the selection first */
      {
         int done = -1, k;
         SDL_Surface *cs = NULL; float avg[3], acc[3];
         SDL_LockMutex(cv_mx);
         if (cv_done >= 0) { done = cv_done; cs = cv_surf; memcpy(avg, cv_avg, sizeof(avg)); memcpy(acc, cv_acc, sizeof(acc)); cv_done = -1; cv_surf = NULL; cv_busy = 0; }
         if (done >= 0 && cv_done_gen != cv_gen) { done = -1; if (cs) SDL_FreeSurface(cs); cs = NULL; }   /* before a rescan */
         SDL_UnlockMutex(cv_mx);
         if (done >= 0)
         {
            game_finish_cover(&g_games[done], cs, avg, acc);
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
         int covered, blackout;
         float launch_ui;   /* dashboard UI and help bar: fade out in 0.35 s as the launch starts */
         int anim = pl ? pl->launch : ANIM_OPEN_CASE_DISC;
         float pdt = 0.0f;   /* launch clock step this frame */
         if (play_t >= 0 && t > play_t) { pdt = dt; play_pt += pdt; }
         if (play_t >= 0)
         {
            float pt = play_pt, u, end = 2.8f, fl_t = 2.35f;
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
                  disc_spin += w * pdt;
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
         launch_ui = play_t >= 0 ? 1.0f - clampf(play_pt / 0.35f, 0.0f, 1.0f) : 1.0f;
         /* the launch has faded to black: nothing under the black is drawn (it was three
          * full-screen layers, art + the 3D media + the black quad, for the last 0.15 s) */
         blackout = play_t >= 0 && fade >= 0.999f;

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
            for (i = 0; i < 3; i++) spring_step(&bg[i], want[i], 6, 5, dt);
         }
         if (fabsf(dpos.x - sel) > 2.5f) { dpos.x = (float)sel; dpos.v = 0; }   /* wrap-around: no long slide */
         spring_step(&dpos, (float)sel, 210, 24, dt);
         spring_step(&info_y, 0, 170, 17, dt);
         spring_step(&info_a, 1, 60, 12, dt);
         spring_step(&plat_x, 0, 120, 16, dt);

         /* ---- render ---- */
         t_upd = now_s();
         glViewport(0, 0, g_dw, g_dh);
         if (blackout) glClearColor(0, 0, 0, 1);
         else glClearColor(bg[0].x, bg[1].x, bg[2].x, 1);
         glClearDepthf(1.0f);
         glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
         glEnable(GL_BLEND);
         glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

         glUseProgram(g_prog_2d);
         glUniform1i(u2_tex, 0);
         covered = (panel_active() && panel_amount() > 0.995f) || welcome_amount() > 0.995f || blackout;   /* a full-screen section is open */
         if (!covered)
         {  /* full-screen art (current layer, next one crossfading over it), then the
             * legibility overlay (left side, top and bottom bands) in one pass */
            float e = bg_mix * bg_mix * (3 - 2 * bg_mix);
            const float ov[4] = { 1, 1, 1, 1 };
            (void)ov;
            g_bg_dim = settings_get_int("bg_dim", 45, 0, 90) / 100.0f;
            bg_draw(&bg_cur, 1.0f, t);   /* opaque: the scrim is baked into both kinds of layer */
            if (!bg_empty(&bg_next)) bg_draw(&bg_next, e, t);
         }
         if (g_fx_text && !covered)
         {  /* dashboard UI: dimmed under the panel, gone in the attract scene, fading out
             * as the launch starts (the 3D media is drawn over it) */
            const float pa = panel_amount();
            const float ua = (1 - fade) * (1 - 0.6f * pa) * (1 - att.amount) * launch_ui;
            dash_draw(pl, sel_plat, n, sel, dpos.x, sel_t, t, ua, info_a.x, info_y.x);
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
         /* 3D only while launching: the selected game's case / cartridge / marquee */
         for (i = 0; i >= 0 && n && play_t >= 0 && !blackout; i--)
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
               if (o == 0 && g_fx_refl && g->al.x * (1.0f - fade) * (1.0f - zoom) > 0.004f)
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
         if (play_t >= 0 && anim == ANIM_OPEN_CASE_DISC && fly > 0.0f && !blackout)
         {
            glDisable(GL_DEPTH_TEST);
            draw_flying_disc(fly, spin, 1.0f);
            glEnable(GL_DEPTH_TEST);
         }
         if (play_t >= 0 && anim == ANIM_INSERT_COIN && pl && !blackout)
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
            (void)ua;
            if (att.dim > 0.003f)
            {
               const float dimq[4] = { 0, 0, 0, att.dim };
               draw_2d(0, 0, 0, W, H, dimq, 0);
            }
            if (pa > 0.003f)
            {
               /* the settings are a full-screen section: opaque, so nothing under it is drawn
                * once it is fully open (see `covered`) */
               const float back[4] = { 0.043f, 0.047f, 0.062f, pa };
               draw_2d(0, 0, 0, W, H, back, 0);
            }
            {  /* help bar: the set follows the panel, crossfading over 0.15 s; while the
                * panel is open it is centred in the free space left of it */
               static int h_cur = HINTS_CAROUSEL, h_prev = -1;
               static float h_mix = 1.0f;
               int want = welcome_active() ? (welcome_last_page() ? HINTS_WELCOME_LAST : HINTS_WELCOME) :
                          panel_active() ? panel_hint_set() : (attract_active() ? HINTS_ATTRACT : HINTS_CAROUSEL);
               const float ha = (1 - fade) * (1 - att.amount) * launch_ui;
               if (want != h_cur) { h_prev = h_cur; h_cur = want; h_mix = 0.0f; }
               h_mix = clampf(h_mix + dt / 0.15f, 0.0f, 1.0f);
               g_hint_cx = W * 0.5f;
               if (h_prev >= 0 && h_mix < 1.0f) hints_draw(h_prev, 672, ha * (1.0f - h_mix));
               hints_draw(h_cur, 672, ha * h_mix);
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
            if (welcome_active())
            {  /* over everything; the help bar is drawn again on top of it */
               int hs = welcome_last_page() ? HINTS_WELCOME_LAST : HINTS_WELCOME;
               welcome_draw(t);
               hints_draw(hs, 672, welcome_amount());
            }
            if ((flash > 0.001f || fade > 0.001f) && !blackout)
            {  /* the white flash under the fade to black, in one full-screen pass:
                * premultiplied, dst * (1 - w) * (1 - f) + w * (1 - f) */
               const float w = flash * 0.85f, k = w * (1 - fade);
               const float over[4] = { k, k, k, 1 - (1 - w) * (1 - fade) };
               glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
               draw_2d(0, 0, 0, W, H, over, 0);
               glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            }
            if (fade > 0.001f)
            {
               if (play_t >= 0)
               {
                  const float st[4] = { 0.93f, 0.92f, 0.97f, fade };
                  label_set(&l_start, g_font_sub, tr("launch.starting"));
                  draw_2d(l_start.tex, (W - l_start.w) * 0.5f, (H - l_start.h) * 0.5f, l_start.w, l_start.h, st, 0);
               }
            }
         }
      }

      if (g_ntshot) test_shots(t - t0);
      {
         double pre = now_s();
         t_pre = pre;
         SDL_GL_SwapWindow(win);
         b_draw += pre - t; b_swap += now_s() - pre;
         if (frames == 30)
         {  /* boot.sh: a picture is on screen (clears the compositor's one-shot guard) */
            FILE *pf = fopen("/tmp/lumen-presented", "w");
            if (pf) fclose(pf);
         }
      }
      frames++;
      if (g_cost_on && play_t >= 0)
      {  /* launch trace: one line per frame (pt, frame time, CPU issuing, swap, cost) */
         double fpx = g_cost_px_opaque + g_cost_px_blend, fh = g_cost_px_heavy;
         logf_("launch frame: pt %.3f dt %.1f ms, update %.1f ms, draw %.1f ms, swap %.1f ms, %.2f Mpx (heavy %.2f)",
               play_pt, dt * 1000.0, (t_upd - t) * 1000.0, (t_pre - t_upd) * 1000.0, (now_s() - t_pre) * 1000.0,
               (fpx - lp_px) / 1e6, (fh - lp_heavy) / 1e6);
      }
      log_tick();
      {  /* bench: <home>/bench present -> two rounds (vsync on, then off: frame rates
          * not quantised to 60/30/20), each turning one more thing off per step; per
          * step: fps, CPU ms spent issuing the frame, ms blocked in the swap */
         static int bench = -1, step, round_; static double bt, bframes;
         static const char *names[] = { "all", "no-shadows", "no-side-tiles", "no-ui", "bg-flat", "clear-only" };
         if (bench < 0)
         {
            char bp[300]; snprintf(bp, sizeof(bp), "%s/bench", g_home);
            bench = file_exists(bp) ? 1 : 0;
            if (bench) { remove(bp); bt = t + 3.0; step = -1; logf_("bench: start (MSAA %d, %dx%d)", settings_get_int("msaa", 2, 0, 4), g_dw, g_dh); }
         }
         if (bench == 1 && t >= bt)
         {
            if (step >= 0 && bframes > 0)
               logf_("bench %s %-14s %5.1f fps, draw %.1f ms, swap %.1f ms", round_ ? "free " : "vsync", names[step],
                     bframes / 2.5, b_draw / bframes * 1000.0, b_swap / bframes * 1000.0);
            step++;
            if (step >= (int)(sizeof(names) / sizeof(names[0])) && round_ == 0)
            { round_ = 1; step = 0; SDL_GL_SetSwapInterval(0); }
            if (step >= (int)(sizeof(names) / sizeof(names[0])))
            { bench = 2; g_fx_shadows = g_fx_sides = g_fx_text = g_fx_bg = 1; g_fx_bgflat = 0; SDL_GL_SetSwapInterval(1); logf_("bench: done"); }
            else
            {
               /* each step turns one more thing off: shadows, side tiles, the dashboard UI
                * (text, tabs, tiles), then the background (clear + help bar only) */
               g_fx_shadows = step < 1; g_fx_sides = step < 2; g_fx_text = step < 3; g_fx_bgflat = step == 4; g_fx_bg = step < 5;
               bt = t + 2.5; bframes = 0; b_draw = b_swap = 0;
            }
         }
         if (bench == 1 && step >= 0) bframes += 1;
      }
      if (g_cost_on)
      {
         static int cf; static double ct;
         cf++;
         if (t - ct >= 1.0)
         {
            if (ct > 0 && cf)
               logf_("cost: %.2f Mpx/frame (opaque %.2f, blended %.2f, heavy shaders %.2f), %.0f draws/frame",
                     (g_cost_px_opaque + g_cost_px_blend) / cf / 1e6, g_cost_px_opaque / cf / 1e6,
                     g_cost_px_blend / cf / 1e6, g_cost_px_heavy / cf / 1e6, g_cost_draws / cf);
            g_cost_px_opaque = g_cost_px_blend = g_cost_px_heavy = g_cost_draws = 0; cf = 0; ct = t;
         }
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
   music_shutdown();   /* release the audio device before RetroArch starts */
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
   SDL_LockMutex(dc_mx); dc_quit = 1; SDL_CondSignal(dc_cv); SDL_UnlockMutex(dc_mx);
   logf_("exit %d", exit_code);
   SDL_GL_DeleteContext(ctx);
   SDL_DestroyWindow(win);
   TTF_Quit(); IMG_Quit(); SDL_Quit();
   return exit_code;
}
