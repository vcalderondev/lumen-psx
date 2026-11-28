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
 *   - "play": the flat launch (src/flat.inc): the system's machine as an illustration, the
 *     game's medium goes into it, fade to black; then the frontend exits with code 10 and writes the launch request to
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
#include <pthread.h>
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
#include "collections.h"
#include "gamemenu.h"
#include "filepicker.h"
#include "transfer.h"
#include "audit.h"
#include "net.h"
#include "module.h"
#include "conn.h"
#include "kern.h"
#include "update.h"
#include "padwait.h"
#include "ui.h"
#include "theme.h"
#include "crt.h"

#define COVER_CACHE_BYTES (32.0 * 1024 * 1024)   /* covers kept on the GPU */

/* ------------------------------------------------------------------------ */
/* logging                                                                   */
/* The log lives on the USB stick (exFAT through FUSE): a file's size is only committed
 * on close/fsync, so a power-off would leave the log of the running frontend empty.
 * Every line is written and flushed at once; fsync runs at most once per second,
 * immediately for lines that report an error, and at exit.
 * None of that happens on the caller's thread: a write goes through the FUSE daemon and
 * an fsync waits for the stick, and logf_() is called from the render loop (the fps line
 * every 5 s, key presses, the perf probes). Lines are queued and a writer thread does the
 * I/O. Before the writer starts (and for stderr) lines are written directly. */
static FILE  *g_log;
static double g_log_synced = -10.0;   /* monotonic time of the last fsync */
static int    g_log_pending;          /* lines written since that fsync (writer thread) */
static pthread_mutex_t g_log_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_log_cv = PTHREAD_COND_INITIALIZER;
static pthread_cond_t  g_log_room = PTHREAD_COND_INITIALIZER;   /* the writer took the queue */
static pthread_t       g_log_thread;
static int    g_log_async, g_log_quit, g_log_force;
static char   g_log_q[16384];         /* queued lines, '\n' terminated */
static size_t g_log_qn;

static double log_clock(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* writer thread, or any thread before it exists */
static void log_sync(int force)
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

static void *log_writer(void *unused)
{
   static char out[sizeof(g_log_q)];
   (void)unused;
   pthread_mutex_lock(&g_log_mx);
   for (;;)
   {
      size_t n; int force, quit;
      while (!g_log_qn && !g_log_quit)
      {
         if (g_log_pending)
         {  /* lines not yet synced: wake up when the 1 s limit allows it */
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 250000000L;
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            pthread_cond_timedwait(&g_log_cv, &g_log_mx, &ts);
            if (!g_log_qn && !g_log_quit) { pthread_mutex_unlock(&g_log_mx); log_sync(0); pthread_mutex_lock(&g_log_mx); }
         }
         else pthread_cond_wait(&g_log_cv, &g_log_mx);
      }
      n = g_log_qn; force = g_log_force; quit = g_log_quit;
      memcpy(out, g_log_q, n);
      g_log_qn = 0; g_log_force = 0;
      pthread_cond_broadcast(&g_log_room);
      pthread_mutex_unlock(&g_log_mx);
      if (n) { fwrite(out, 1, n, g_log); fflush(g_log); g_log_pending = 1; }
      log_sync(force || quit);
      pthread_mutex_lock(&g_log_mx);
      if (quit && !g_log_qn) break;
   }
   pthread_mutex_unlock(&g_log_mx);
   return NULL;
}

/* after the log file is open: lines go through the writer thread from here on */
static void log_start_writer(void)
{
   if (!g_log || g_log == stderr) return;
   if (pthread_create(&g_log_thread, NULL, log_writer, NULL) == 0) g_log_async = 1;
}

void logf_(const char *fmt, ...)
{
   va_list ap;
   char line[1024];
   size_t n;
   if (!g_log) g_log = stderr;
   va_start(ap, fmt); vsnprintf(line, sizeof(line) - 1, fmt, ap); va_end(ap);
   n = strlen(line);
   line[n++] = '\n';
   pthread_mutex_lock(&g_log_mx);
   /* a burst that fills the queue waits for the writer to take it (lines stay in order) */
   while (g_log_async && n > sizeof(g_log_q) - g_log_qn) pthread_cond_wait(&g_log_room, &g_log_mx);
   if (g_log_async)
   {
      memcpy(g_log_q + g_log_qn, line, n);
      g_log_qn += n;
      if (log_is_error(line)) g_log_force = 1;
      pthread_cond_signal(&g_log_cv);
      pthread_mutex_unlock(&g_log_mx);
      return;
   }
   pthread_mutex_unlock(&g_log_mx);
   /* no writer (startup, stderr, exit): write here */
   flockfile(g_log);
   fwrite(line, 1, n, g_log); fflush(g_log);
   if (!g_log_async) { g_log_pending = 1; log_sync(log_is_error(line)); }
   funlockfile(g_log);
}

static void log_sync_atexit(void)
{
   if (!g_log) return;
   if (g_log_async)
   {
      pthread_mutex_lock(&g_log_mx);
      g_log_quit = 1;
      pthread_cond_signal(&g_log_cv);
      pthread_mutex_unlock(&g_log_mx);
      pthread_join(g_log_thread, NULL);
      pthread_mutex_lock(&g_log_mx);
      g_log_async = 0;
      if (g_log_qn) { fwrite(g_log_q, 1, g_log_qn, g_log); g_log_qn = 0; }   /* queued as the writer left */
      pthread_cond_broadcast(&g_log_room);
      pthread_mutex_unlock(&g_log_mx);
   }
   flockfile(g_log); fflush(g_log); g_log_pending = 1; log_sync(1); funlockfile(g_log);
}

/* Record mode (LUMEN_RECORD=<file or fifo>, for videos made in a simulator): from the
 * main loop on, time is virtual and advances exactly 1/LUMEN_RECORD_FPS (default 60) per
 * presented frame, and every frame is appended to the file as raw RGBA, bottom-up
 * (glReadPixels order). A slow renderer then still yields a smooth, exact-rate video.
 * LUMEN_RECORD_SECONDS ends the run after that much virtual time. */
static FILE    *g_rec;
static int      g_rec_on;
static double   g_rec_base, g_rec_fps = 60.0, g_rec_secs;
static unsigned g_rec_frames;

static double real_now_s(void) { return SDL_GetPerformanceCounter() / (double)SDL_GetPerformanceFrequency(); }
double now_s(void) { return g_rec_on ? g_rec_base + g_rec_frames / g_rec_fps : real_now_s(); }

/* perf probes. Startup: one "boot:" line per phase (always; the splash's own frames are
 * part of the phase they follow). With LUMEN_COST=1 or <home>/debug/perf, once per
 * second: the frame's CPU time (update = input, logic, uploads; draw = issuing GL), the
 * time blocked in the swap, their worst frame, and the latency of the button presses of
 * that second (SDL event timestamp -> the swap of the frame that shows it returned). */
static int    g_perf_on;
static double g_boot_last;
static void boot_mark(const char *what)
{
   double n = now_s();
   if (g_boot_last > 0.0) logf_("boot: %-10s %5.0f ms", what, (n - g_boot_last) * 1000.0);
   g_boot_last = n;
}
/* main-thread work that can hold a frame back (uploads, text rendering): logged when a
 * single piece takes over 2 ms */
static void perf_stall(const char *what, double since)
{
   double ms = (now_s() - since) * 1000.0;
   if (g_perf_on && ms > 2.0) logf_("perf: stall %s %.1f ms", what, ms);
}
/* Where the main thread is when a frame hangs (debug/perf): a watchdog thread sees that no
 * frame has ended for 150 ms and signals the main thread, whose handler notes the program
 * counter, the link register and every word of the stack that points into the program's
 * code. The frame that finally ends logs them ("perf: stuck at ..."); addr2line on the
 * unstripped binary of the same build names the functions. */
#include <signal.h>
#include <ucontext.h>
extern char __executable_start[], etext[];
static volatile unsigned g_hb;                  /* frames ended */
static volatile int      g_stuck_n;
static uintptr_t         g_stuck_at[20];
static pthread_t         g_main_pt;
static uintptr_t         g_stack_hi;            /* end of the main thread's stack (/proc/self/maps) */
static void stuck_handler(int sig, siginfo_t *si, void *ctx)
{
   const ucontext_t *uc = ctx;
   const uintptr_t lo = (uintptr_t)__executable_start, hi = (uintptr_t)etext;
   const uintptr_t *sp;
   int n = 0, i;
   (void)sig; (void)si;
   if (g_stuck_n) return;
#if defined(__arm__)
   g_stuck_at[n++] = uc->uc_mcontext.arm_pc; g_stuck_at[n++] = uc->uc_mcontext.arm_lr; sp = (const uintptr_t *)uc->uc_mcontext.arm_sp;
#elif defined(__aarch64__)
   g_stuck_at[n++] = uc->uc_mcontext.pc; g_stuck_at[n++] = uc->uc_mcontext.regs[30]; sp = (const uintptr_t *)uc->uc_mcontext.sp;
#else
   (void)uc; sp = (const uintptr_t *)&uc;
#endif
   for (i = 0; i < 6000 && n < 20 && (uintptr_t)(sp + i + 1) <= g_stack_hi; i++)
      if (sp[i] >= lo && sp[i] < hi && (n < 3 || sp[i] != g_stuck_at[n - 1])) g_stuck_at[n++] = sp[i];
   g_stuck_n = n;
}
static void *stuck_watch(void *unused)
{
   unsigned seen = g_hb, sent = (unsigned)-1; int same = 0;
   (void)unused;
   for (;;)
   {
      struct timespec ts = { 0, 50000000L };
      nanosleep(&ts, NULL);
      if (g_hb != seen) { seen = g_hb; same = 0; continue; }
      if (++same == 3 && sent != seen) { sent = seen; pthread_kill(g_main_pt, SIGUSR2); }
   }
   return NULL;
}
static void stuck_start(void)
{
   struct sigaction sa;
   pthread_t th;
   memset(&sa, 0, sizeof(sa));
   sa.sa_sigaction = stuck_handler;
   sa.sa_flags = SA_SIGINFO | SA_RESTART;
   sigemptyset(&sa.sa_mask);
   {
      FILE *f = fopen("/proc/self/maps", "r");
      char ln[300]; unsigned long a, b;
      while (f && fgets(ln, sizeof(ln), f))
         if (strstr(ln, "[stack]") && sscanf(ln, "%lx-%lx", &a, &b) == 2) g_stack_hi = (uintptr_t)b;
      if (f) fclose(f);
   }
   if (!g_stack_hi || sigaction(SIGUSR2, &sa, NULL) != 0) return;
   g_main_pt = pthread_self();
   if (pthread_create(&th, NULL, stuck_watch, NULL) == 0) pthread_detach(th);
   logf_("perf: code at 0x%lx - 0x%lx", (unsigned long)(uintptr_t)__executable_start, (unsigned long)(uintptr_t)etext);
}
static void stuck_report(void)
{
   char line[400]; int i, k = 0;
   if (!g_stuck_n) return;
   for (i = 0; i < g_stuck_n && k < (int)sizeof(line) - 12; i++) k += snprintf(line + k, sizeof(line) - k, " 0x%lx", (unsigned long)g_stuck_at[i]);
   logf_("perf: stuck at%s", line);
   g_stuck_n = 0;
}

static double g_pf_text_ms; static int g_pf_text_n;   /* text rendered and uploaded during this frame */
static struct { int frames, slow, nin; double upd, draw, swap, upd_max, draw_max, swap_max, in_sum, in_max; Uint32 in_ts[8]; int in_n; } g_pf;
static void perf_input(Uint32 ts) { if (g_perf_on && g_pf.in_n < 8) g_pf.in_ts[g_pf.in_n++] = ts; }
static void perf_frame(double t, double t_upd, double t_pre, double t_end)
{
   static double last;
   const double u = t_upd - t, d = t_pre - t_upd, w = t_end - t_pre;
   int i;
   if (!g_perf_on) return;
   g_hb++;
   stuck_report();
   g_pf.frames++; g_pf.upd += u; g_pf.draw += d; g_pf.swap += w;
   if (u > g_pf.upd_max) g_pf.upd_max = u;
   if (d > g_pf.draw_max) g_pf.draw_max = d;
   if (w > g_pf.swap_max) g_pf.swap_max = w;
   if (t_end - t > 0.025)
   {  /* a frame the eye sees as a stutter: one line each (at most 30 a second) */
      static double win0; static int nwin;
      g_pf.slow++;
      if (t - win0 > 1.0) { win0 = t; nwin = 0; }
      if (nwin++ < 30)
         logf_("perf: slow frame %.0f ms (update %.1f, draw %.1f, swap %.1f; %d texts %.1f ms)", (t_end - t) * 1000.0, u * 1000.0, d * 1000.0, w * 1000.0,
               g_pf_text_n, g_pf_text_ms);
   }
   g_pf_text_ms = 0.0; g_pf_text_n = 0;
   for (i = 0; i < g_pf.in_n; i++)
   {
      double l = (double)(Uint32)(SDL_GetTicks() - g_pf.in_ts[i]);
      g_pf.in_sum += l; g_pf.nin++;
      if (l > g_pf.in_max) g_pf.in_max = l;
   }
   g_pf.in_n = 0;
   if (t - last < 1.0) return;
   if (last > 0.0 && g_pf.frames)
   {
      const double k = 1000.0 / g_pf.frames;
      logf_("perf: %d frames (%d over 25 ms), update %.2f ms (max %.1f), draw %.2f ms (max %.1f), swap %.1f ms (max %.1f)",
            g_pf.frames, g_pf.slow, g_pf.upd * k, g_pf.upd_max * 1000.0, g_pf.draw * k, g_pf.draw_max * 1000.0,
            g_pf.swap * k, g_pf.swap_max * 1000.0);
      if (g_pf.nin) logf_("perf: input latency %.0f ms avg, %.0f ms max (%d presses)", g_pf.in_sum / g_pf.nin, g_pf.in_max, g_pf.nin);
   }
   memset(&g_pf, 0, sizeof(g_pf)); last = t;
}

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
int        g_nreal_games, g_nreal_plat;   /* the library proper; collections are appended after */

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
   if (theme_asset(out, n, rel_new)) return;   /* the active theme's own file wins */
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

/* The shape of a system's tiles: the proportions of its retail game box (width / height),
 * so a shelf of NES games is a row of tall boxes and a shelf of Super Nintendo ones a row
 * of wide ones. [media] cover = W:H (or a number) in the declaration wins; without it the
 * system's id is looked up here; an unknown system gets a square. Kept within 0.55 .. 1.5:
 * the row has one height. */
static float box_aspect(const char *id, const char *decl)
{
   static const struct { const char *id; float ar; } k_box[] = {
      /* Sony */
      { "psx", 1.0f }, { "psxstock", 1.0f }, { "ps1", 1.0f }, { "ps2", 0.71f }, { "ps3", 0.87f }, { "psp", 0.59f }, { "psvita", 0.78f },
      /* Nintendo */
      { "nes", 0.71f }, { "famicom", 0.71f }, { "fds", 0.86f }, { "snes", 1.40f }, { "sfc", 0.56f }, { "superfamicom", 0.56f },
      { "n64", 1.40f }, { "gamecube", 0.71f }, { "gc", 0.71f }, { "wii", 0.71f }, { "virtualboy", 1.0f },
      { "gb", 1.0f }, { "gbc", 1.0f }, { "gba", 1.0f }, { "nds", 1.10f }, { "3ds", 1.10f }, { "gameandwatch", 1.0f },
      /* Sega */
      { "sg1000", 0.71f }, { "mastersystem", 0.71f }, { "sms", 0.71f }, { "megadrive", 0.71f }, { "genesis", 0.71f }, { "md", 0.71f },
      { "sega32x", 0.71f }, { "32x", 0.71f }, { "segacd", 0.60f }, { "megacd", 0.71f }, { "saturn", 0.60f }, { "dreamcast", 1.0f },
      { "gamegear", 0.71f }, { "gg", 0.71f },
      /* NEC, SNK, Atari, Bandai and others */
      { "pcengine", 1.0f }, { "pce", 1.0f }, { "tg16", 1.0f }, { "pcenginecd", 1.0f }, { "pcecd", 1.0f }, { "supergrafx", 1.0f },
      { "neogeo", 0.74f }, { "neogeocd", 1.0f }, { "ngp", 0.86f }, { "ngpc", 0.86f },
      { "atari2600", 0.72f }, { "atari5200", 0.72f }, { "atari7800", 0.72f }, { "lynx", 0.80f }, { "jaguar", 0.72f },
      { "wonderswan", 0.71f }, { "wonderswancolor", 0.71f }, { "colecovision", 0.72f }, { "intellivision", 0.72f },
      { "vectrex", 0.72f }, { "3do", 0.60f }, { "cdi", 0.71f },
      /* arcade flyers, home computers */
      { "arcade", 0.75f }, { "mame", 0.75f }, { "fbneo", 0.75f }, { "cps1", 0.75f }, { "cps2", 0.75f }, { "cps3", 0.75f },
      { "msx", 0.71f }, { "msx2", 0.71f }, { "c64", 0.71f }, { "amiga", 0.80f }, { "zxspectrum", 0.64f }, { "amstradcpc", 0.64f },
      { "atarist", 0.80f }, { "dos", 0.80f }, { "pc", 0.80f }, { "scummvm", 0.80f }, { "x68000", 0.80f }, { "pc98", 0.80f },
   };
   float ar = 0.0f, a = 0.0f, b = 0.0f;
   size_t i;
   if (decl && *decl)
   {
      if (sscanf(decl, "%f : %f", &a, &b) == 2 && a > 0.0f && b > 0.0f) ar = a / b;
      else if (sscanf(decl, "%f", &a) == 1 && a > 0.0f) ar = a;
      else logf_("system %s: bad [media] cover = %s (W:H expected)", id, decl);
   }
   for (i = 0; ar <= 0.0f && i < sizeof(k_box) / sizeof(k_box[0]); i++) if (!strcasecmp(id, k_box[i].id)) ar = k_box[i].ar;
   if (ar <= 0.0f) ar = 1.0f;
   return ar < 0.55f ? 0.55f : ar > 1.5f ? 1.5f : ar;
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
   decl_get(ini, "library", "ports", pl->ports, sizeof(pl->ports), "");
   if (!pl->ports[0] && strcmp(pl->layout, "stock") && strcmp(pl->layout, "apps"))
      snprintf(pl->ports, sizeof(pl->ports), "%s/games/%s-native", g_root, pl->id);   /* convention */
   decl_get(ini, "library", "extensions", pl->exts, sizeof(pl->exts), !strcmp(pl->layout, "stock") ? "cue,bin" : "");
   decl_get(ini, "library", "metadata", v, sizeof(v), "");
   if (!v[0]) snprintf(v, sizeof(v), "%s.games", pl->id);
   snprintf(pl->meta, sizeof(pl->meta), "%s/systems/%s", g_home, v);   /* next to the declaration */
   decl_get(ini, "launch", "core", pl->core, sizeof(pl->core), "");
   decl_get(ini, "media", "kind", v, sizeof(v), "disc");
   pl->kind = !strcmp(v, "cartridge") || !strcmp(v, "card") ? KIND_CART : (!strcmp(v, "arcade") ? KIND_ARCADE : KIND_DISC);
   decl_get(ini, "media", "cover", v, sizeof(v), "");
   pl->tile_ar = box_aspect(pl->id, v);
   /* the launch: the flat illustration <home>/assets/launch/<flat>/ (flat.inc); a system
    * without one gets the generic launch, the game's cover as the object */
   decl_get(ini, "animation", "flat", pl->flat, sizeof(pl->flat), pl->id);
   decl_get(ini, "animation", "sound", v, sizeof(v), "");
   if (v[0]) system_asset(pl->sound, sizeof(pl->sound), pl->id, v);
   decl_get(ini, "graphics", "core_options_name", pl->gfx_core, sizeof(pl->gfx_core), "");
   decl_get(ini, "graphics", "enhanced", pl->gfx_enh, sizeof(pl->gfx_enh), "");
   decl_get(ini, "graphics", "original", pl->gfx_orig, sizeof(pl->gfx_orig), "");
   decl_get(ini, "graphics", "retroarch", v, sizeof(v), "");   /* alias of retroarch_enhanced */
   decl_get(ini, "graphics", "retroarch_enhanced", pl->gfx_ra_enh, sizeof(pl->gfx_ra_enh), v);
   decl_get(ini, "graphics", "retroarch_original", pl->gfx_ra_orig, sizeof(pl->gfx_ra_orig), "");
   decl_get(ini, "graphics", "native_720", pl->gfx_nat720, sizeof(pl->gfx_nat720), "");
   decl_get(ini, "graphics", "native_1080", pl->gfx_nat1080, sizeof(pl->gfx_nat1080), "");
   decl_get(ini, "graphics", "retroarch_native", pl->gfx_ra_nat, sizeof(pl->gfx_ra_nat), pl->gfx_ra_enh);
   pl->has_gfx = pl->gfx_enh[0] || pl->gfx_ra_enh[0];
   pl->has_native = pl->has_gfx && (pl->gfx_nat720[0] || pl->gfx_nat1080[0]);
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

/* built-in-game systems (layout = stock) hidden by show_stock_games=0: kept aside so a
 * stick with no games of its own still lists the console's games (scan_library) */
static platform_t s_stock_hidden[MAX_PLATFORMS];
static int        s_nstock_hidden;

static void load_systems(void)
{
   char dir[300], path[600];
   DIR *d; struct dirent *de;
   int ndecl = 0;
   s_nstock_hidden = 0;
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
            {
               logf_("[%s] built-in games hidden (show_stock_games=0)", pl->id);
               if (s_nstock_hidden < MAX_PLATFORMS) s_stock_hidden[s_nstock_hidden++] = *pl;
            }
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

/* A folder's file names, read once. The scans looked every candidate file up with
 * stat() (up to 12 per game: cover, media art, background and music under several
 * names). The stick is exFAT through FUSE: every lookup is a round trip to its daemon,
 * which walks the folder's entries, so a folder of n games cost n * 12 walks. Now the
 * names are listed once and looked up in memory, ignoring ASCII case like the file
 * system does; the path returned carries the name as it is on disk. */
typedef struct { char **name; int *sorted; int n, cap; } dirlist_t;
static const dirlist_t *s_dl_cmp;
static int dirlist_cmp(const void *a, const void *b)
{
   return strcasecmp(s_dl_cmp->name[*(const int *)a], s_dl_cmp->name[*(const int *)b]);
}
static void dirlist_free(dirlist_t *dl)
{
   int i;
   for (i = 0; i < dl->n; i++) free(dl->name[i]);
   free(dl->name); free(dl->sorted);
   memset(dl, 0, sizeof(*dl));
}
/* names in readdir order (name[]), plus an index sorted for the lookups; 0 = no folder */
static int dirlist_read(dirlist_t *dl, const char *dir)
{
   DIR *d; struct dirent *de; int i;
   memset(dl, 0, sizeof(*dl));
   if (!(d = opendir(dir))) return 0;
   while ((de = readdir(d)))
   {
      if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
      if (dl->n == dl->cap)
      {
         int cap = dl->cap ? dl->cap * 2 : 64;
         char **nn = realloc(dl->name, sizeof(char *) * cap);
         if (!nn) break;
         dl->name = nn; dl->cap = cap;
      }
      if (!(dl->name[dl->n] = strdup(de->d_name))) break;
      dl->n++;
   }
   closedir(d);
   dl->sorted = malloc(sizeof(int) * (dl->n ? dl->n : 1));
   if (!dl->sorted) { dirlist_free(dl); return 1; }
   for (i = 0; i < dl->n; i++) dl->sorted[i] = i;
   s_dl_cmp = dl;
   qsort(dl->sorted, dl->n, sizeof(int), dirlist_cmp);
   return 1;
}
/* the entry called `name` (ASCII case ignored) as it is on disk, or NULL */
static const char *dirlist_find(const dirlist_t *dl, const char *name)
{
   int lo = 0, hi = dl->n - 1;
   while (lo <= hi)
   {
      int mid = (lo + hi) / 2, c = strcasecmp(name, dl->name[dl->sorted[mid]]);
      if (!c) return dl->name[dl->sorted[mid]];
      if (c < 0) hi = mid - 1; else lo = mid + 1;
   }
   return NULL;
}
/* <dir>/<name> when the folder has it, else "". A name with non-ASCII letters that is
 * not listed is still asked to the file system (its own case folding decides) */
static int dirlist_path(const dirlist_t *dl, char *out, size_t n, const char *dir, const char *name)
{
   const char *hit = dirlist_find(dl, name);
   const unsigned char *c;
   if (hit) { snprintf(out, n, "%s/%s", dir, hit); return 1; }
   for (c = (const unsigned char *)name; *c; c++)
      if (*c >= 0x80) { snprintf(out, n, "%s/%s", dir, name); if (file_exists(out)) return 1; break; }
   out[0] = 0;
   return 0;
}
/* find_first() over a listed folder: the first existing <dir>/<stem><suffix> */
static void dirlist_first(const dirlist_t *dl, char *out, size_t n, const char *dir, const char *stem, const char *const *suffixes)
{
   char name[600];
   int b;
   for (b = 0; suffixes[b]; b++)
   {
      snprintf(name, sizeof(name), "%s%s", stem, suffixes[b]);
      if (dirlist_path(dl, out, n, dir, name)) return;
   }
   out[0] = 0;
}

/* PSC/BleemSync layout: <path>/<n>/Game.ini + <disc>.cue|pbp|... + <disc>.png */
static void splash_pulse(void);   /* startup splash: keeps "Loading" alive while the scan blocks */

static void scan_psc(int pi, const char *dir)
{
   char path[700], buf[4096], discs[512], gdir[700], name[600];
   static const char *const bg_sfx[] = { "background.jpg", "background.png", "bg.jpg", "bg.png", NULL };
   static const char *const music_sfx[] = { "music.ogg", "music.wav", NULL };
   DIR *d; struct dirent *de; platform_t *pl = &g_plat[pi];
   dirlist_t dl;
   if (!(d = opendir(dir))) { logf_("[%s] no dir %s", pl->id, dir); return; }
   memset(&dl, 0, sizeof(dl));
   while ((de = readdir(d)) && g_ngames < MAX_GAMES)
   {
      FILE *f; size_t n; game_t *g; char *c; char exts[160], *tok, *save = NULL;
      if (de->d_name[0] == '.') continue;
      splash_pulse();
      snprintf(path, sizeof(path), "%s/%s/Game.ini", dir, de->d_name);
      if (!(f = fopen(path, "rb"))) continue;
      snprintf(gdir, sizeof(gdir), "%s/%s", dir, de->d_name);
      dirlist_free(&dl);
      dirlist_read(&dl, gdir);   /* the game's folder, listed once for every file below */
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
         snprintf(name, sizeof(name), "%s.%s", discs, tok);
         dirlist_path(&dl, g->content, sizeof(g->content), gdir, name);
      }
      if (!g->content[0]) continue;
      snprintf(name, sizeof(name), "%s.png", discs);
      g->has_cover = dirlist_path(&dl, g->cover, sizeof(g->cover), gdir, name);
      if (!g->has_cover) snprintf(g->cover, sizeof(g->cover), "%s/%s", gdir, name);
      dirlist_first(&dl, g->bg, sizeof(g->bg), gdir, "", bg_sfx);
      dirlist_first(&dl, g->music, sizeof(g->music), gdir, "", music_sfx);
      if (!g->title[0]) snprintf(g->title, sizeof(g->title), "%s", discs);
      g->platform = pi;
      g_ngames++;
   }
   closedir(d);
   dirlist_free(&dl);
}

/* Files layout: <path>/<name>.<ext>, art next to it: <name>.png|jpg,
 * <name>-background.jpg|png */
static void scan_files(int pi, const char *dir)
{
   platform_t *pl = &g_plat[pi];
   dirlist_t dl; int k;
   static const char *const img_sfx[] = { ".png", ".jpg", NULL };
   static const char *const bg_sfx[] = { "-background.jpg", "-background.png", "-bg.jpg", "-bg.png", NULL };
   static const char *const music_sfx[] = { "-music.ogg", "-music.wav", NULL };
   if (!dirlist_read(&dl, dir)) { logf_("[%s] no dir %s", pl->id, dir); return; }
   for (k = 0; k < dl.n && g_ngames < MAX_GAMES; k++)
   {
      const char *fname = dl.name[k];
      game_t *g; char stem[256], *dot, *p;
      if (fname[0] == '.' || !ext_ok(pl, fname)) continue;
      g = &g_games[g_ngames];
      memset(g, 0, sizeof(*g));
      snprintf(g->content, sizeof(g->content), "%s/%s", dir, fname);
      snprintf(stem, sizeof(stem), "%s", fname);
      if ((dot = strrchr(stem, '.'))) *dot = 0;
      dirlist_first(&dl, g->cover, sizeof(g->cover), dir, stem, img_sfx);
      g->has_cover = g->cover[0] != 0;
      dirlist_first(&dl, g->bg, sizeof(g->bg), dir, stem, bg_sfx);
      dirlist_first(&dl, g->music, sizeof(g->music), dir, stem, music_sfx);
      /* title: file name without "(...)" / "[...]" tags */
      snprintf(g->title, sizeof(g->title), "%s", stem);
      if ((p = strpbrk(g->title, "([")) && p > g->title) *p = 0;
      trim(g->title);
      g->platform = pi;
      g_ngames++;
   }
   dirlist_free(&dl);
}

/* Apps layout (homebrew): <path>/<name>/app.ini, Game.ini style lines:
 *   Title=  Exec=<program, relative to the folder>  Publisher=  Year=  Players=
 * art in the folder: icon.png|cover.png|.jpg, background.jpg|png, music.ogg.
 * content = the program; run.sh starts it natively (kind=app), RESET or OPEN quit it.
 * port: the folder holds native ports of a system's games ([library] ports): the same
 * layout, listed among that system's games and marked as ports; the folder may not exist. */
static void scan_apps(int pi, const char *dir, int port)
{
   char path[700], buf[4096], exe[256], base[700];
   static const char *const cover_sfx[] = { "icon.png", "cover.png", "icon.jpg", "cover.jpg", NULL };
   static const char *const bg_sfx[] = { "background.jpg", "background.png", "bg.jpg", "bg.png", NULL };
   static const char *const music_sfx[] = { "music.ogg", "music.wav", NULL };
   DIR *d; struct dirent *de; platform_t *pl = &g_plat[pi];
   int found = 0;
   if (!(d = opendir(dir))) { if (!port) logf_("[%s] no dir %s", pl->id, dir); return; }
   while ((de = readdir(d)) && g_ngames < MAX_GAMES)
   {
      FILE *f; size_t n; game_t *g;
      if (de->d_name[0] == '.') continue;
      snprintf(path, sizeof(path), "%s/%s/app.ini", dir, de->d_name);
      if (!(f = fopen(path, "rb"))) continue;
      buf[0] = '\n';
      n = fread(buf + 1, 1, sizeof(buf) - 2, f); fclose(f);
      buf[n + 1] = 0;
      ini_get(buf, "Exec", exe, sizeof(exe));
      trim(exe);
      if (!exe[0] || strstr(exe, "..") || exe[0] == '/') { logf_("[%s] %s: no valid Exec=", pl->id, de->d_name); continue; }
      g = &g_games[g_ngames];
      memset(g, 0, sizeof(*g));
      snprintf(g->content, sizeof(g->content), "%s/%s/%s", dir, de->d_name, exe);
      if (!file_exists(g->content)) { logf_("[%s] %s: %s not found", pl->id, de->d_name, exe); continue; }
      ini_get(buf, "Title", g->title, sizeof(g->title));
      ini_get(buf, "Publisher", g->publisher, sizeof(g->publisher));
      if (!g->publisher[0]) ini_get(buf, "Author", g->publisher, sizeof(g->publisher));
      ini_get(buf, "Year", g->year, sizeof(g->year));
      ini_get(buf, "Players", g->players, sizeof(g->players));
      if (!g->title[0]) snprintf(g->title, sizeof(g->title), "%s", de->d_name);
      snprintf(base, sizeof(base), "%s/%s/", dir, de->d_name);
      find_first(g->cover, sizeof(g->cover), base, cover_sfx);
      g->has_cover = g->cover[0] != 0;
      find_first(g->bg, sizeof(g->bg), base, bg_sfx);
      find_first(g->music, sizeof(g->music), base, music_sfx);
      g->platform = pi;
      g->port = port;
      g_ngames++; found++;
   }
   closedir(d);
   if (port && found) logf_("[%s] %d native port(s) in %s", pl->id, found, dir);
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
      splash_pulse();
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
         splash_pulse();
         if (!strcmp(g_plat[pi].layout, "psc")) scan_psc(pi, tok);
         else if (!strcmp(g_plat[pi].layout, "stock")) scan_stock(pi, tok);
         else if (!strcmp(g_plat[pi].layout, "apps")) scan_apps(pi, tok, 0);
         else scan_files(pi, tok);
      }
      snprintf(paths, sizeof(paths), "%s", g_plat[pi].ports);
      for (save = NULL, tok = strtok_r(paths, ";", &save); tok; tok = strtok_r(NULL, ";", &save))
      { trim(tok); scan_apps(pi, tok, 1); }
   }
}

/* load the declarations and scan every library. A stick with no games of its own (a
 * fresh install) lists the console's built-in games even when show_stock_games=0, so
 * there is always something to play; the setting itself is left as it is. */
static void scan_library(void)
{
   load_systems();
   scan_all();
   if (g_ngames == 0 && s_nstock_hidden > 0)
   {
      int i;
      for (i = 0; i < s_nstock_hidden && g_nplat < MAX_PLATFORMS; i++) g_plat[g_nplat++] = s_stock_hidden[i];
      logf_("no games on the stick: listing the built-in games (show_stock_games=0 kept)");
      /* no game exists yet, so the platform order can still change before the scan */
      qsort(g_plat, g_nplat, sizeof(platform_t), cmp_order);
      for (i = 0; i < g_nplat; i++)
      {
         char paths[512], *tok, *save = NULL;
         if (strcmp(g_plat[i].layout, "stock")) continue;
         snprintf(paths, sizeof(paths), "%s", g_plat[i].paths);
         for (tok = strtok_r(paths, ";", &save); tok; tok = strtok_r(NULL, ";", &save))
         { trim(tok); scan_stock(i, tok); }
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
static GLuint g_prog_2d, g_vbo_quad, g_vao_quad;
static GLint  u2_mvp, u2_tex, u2_color, u2_mode, u2_rect, u2_aux0, u2_aux1, u2_aux2, u2_uvw;
static GLuint g_white;
TTF_Font *g_font_title, *g_font_sub, *g_font_small, *g_font_cover;
TTF_Font *g_font_caps, *g_font_medium;

static const char *VS_2D =
   "#version 300 es\n"
   "layout(location=0) in vec3 pos; layout(location=1) in vec2 uv;\n"
   "uniform mat4 mvp; uniform vec4 uvw; out vec2 v_uv;\n"   /* uvw: the part of the shape this quad is (offset, size) */
   "void main(){ v_uv=uvw.xy+uv*uvw.zw; gl_Position=mvp*vec4(pos,1.0); }\n";

static const char *FS_2D =
   "#version 300 es\n"
   "precision mediump float;\n"
   "in highp vec2 v_uv; uniform sampler2D tex; uniform vec4 color; uniform int mode; uniform highp vec4 rect; out vec4 frag;\n"
   "uniform highp vec4 aux0, aux1, aux2; uniform vec4 dim;\n"
   /* signed distance to a rounded rectangle of rect.xy pixels, corner radius rect.z */
   "highp float rr_dist(){ highp vec2 hs = rect.xy * 0.5; highp float r = min(rect.z, min(hs.x, hs.y));\n"
   "  highp vec2 q = abs((v_uv - 0.5) * rect.xy) - (hs - vec2(r)); return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r; }\n"
   "void shade(){\n"
   /* 6: dashboard tile: texture (uv window aux0) in a rounded mask; aux1.x sheen position
    *    (< -1 none), aux1.y top-light amount */
   "  if (mode==6) { float m = clamp(0.5 - rr_dist(), 0.0, 1.0);\n"
   "                 vec3 c = texture(tex, aux0.xy + v_uv * aux0.zw).rgb * color.rgb;\n"
   "                 if (aux1.x > -1.0) { float s = v_uv.x + v_uv.y * 0.55 - aux1.x; c += vec3(0.22 * exp(-s * s * 55.0)); }\n"
   "                 c += vec3(aux1.y * 0.10 * (1.0 - v_uv.y));\n"
   "                 frag = vec4(c, color.a * m); return; }\n"
   /* 12: the inside of that tile, where the mask is 1: mode 6 without its distance field */
   "  if (mode==12) { vec3 c = texture(tex, aux0.xy + v_uv * aux0.zw).rgb * color.rgb;\n"
   "                  if (aux1.x > -1.0) { float s = v_uv.x + v_uv.y * 0.55 - aux1.x; c += vec3(0.22 * exp(-s * s * 55.0)); }\n"
   "                  c += vec3(aux1.y * 0.10 * (1.0 - v_uv.y));\n"
   "                  frag = vec4(c, color.a); return; }\n"
   /* 7: generated background: diagonal gradient base(aux0) -> colour A(aux1), two soft
    *    light blobs (accent aux2, A) centred at rect.xy / rect.zw (moved by the CPU),
    *    dithered against banding */
   "  if (mode==7) { highp vec2 p = v_uv;\n"
   "                 float g = clamp(p.x * 0.55 + (1.0 - p.y) * 0.45, 0.0, 1.0);\n"
   "                 vec3 c = mix(aux0.rgb, aux1.rgb * 0.70, g * g);\n"
   "                 highp vec2 d1 = (p - rect.xy) * vec2(1.7778, 1.0), d2 = (p - rect.zw) * vec2(1.7778, 1.0);\n"
   "                 c += aux2.rgb * 0.55 * exp(-dot(d1, d1) * 2.6) + aux1.rgb * 0.35 * exp(-dot(d2, d2) * 3.5);\n"
   /*    then the legibility scrim in the same pass (it was a second, blended pass over
    *    the picture): a uniform dark layer (aux0.w opacity), plus the left 74 % (the hero
    *    block), the top (status, tabs) and the bottom half (the shelf, the help bar)
    *    darkened further; bake_scrim() and the generated layer use the same curve. The dither goes on after it, at full strength */
   "                 float l = 1.0 - smoothstep(0.0, 0.74, p.x); l = 0.78 * l * (0.55 + 0.45 * l);\n"
   "                 float b = 0.74 * smoothstep(0.48, 1.0, p.y), t = 0.52 * (1.0 - smoothstep(0.0, 0.32, p.y));\n"
   "                 c *= (1.0 - l) * (1.0 - b) * (1.0 - t) * (1.0 - aux0.w);\n"
   "                 highp float n = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453);\n"
   "                 frag = vec4(c + (n - 0.5) * (1.5 / 255.0), color.a); return; }\n"
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
   /* 11: the inside of a glass card and its shadow under it as one layer (ui_glass_card):
    *     mode 5's fill, times aux0.x, at opacity aux0.y; aux0.z = 1 a flat fill (no glass light) */
   "  if (mode==11) { vec3 fill = clamp(color.rgb + vec3(mix(0.06, -0.04, v_uv.y)) * (1.0 - aux0.z), 0.0, 1.0);\n"
   "                  frag = vec4(fill * aux0.x, aux0.y); return; }\n"
   "  if (mode==5) { highp vec2 hs = rect.xy * 0.5; highp float r = min(rect.z, min(hs.x, hs.y));\n"
   "                 highp vec2 q = abs((v_uv - 0.5) * rect.xy) - (hs - vec2(r));\n"
   "                 highp float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;\n"
   "                 float edge = clamp(0.5 - d, 0.0, 1.0);\n"
   "                 float bw = clamp(d + aux0.x, 0.0, 1.0) * rect.w;\n"
   "                 vec3 fill = color.rgb + vec3(mix(0.06, -0.04, v_uv.y)) * (1.0 - aux0.z);\n"
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
   "}\n"
   /* dim: a scrim folded into what is under it, see scrim_fold() (0, 0, 0, 1 = none) */
   /* (clamped first, as the frame buffer would have: a tile's sheen goes over 1) */
   "void main(){ shade(); frag.rgb = clamp(frag.rgb, 0.0, 1.0) * dim.a + dim.rgb; }\n";

/* plain textured quad (mode 0: backgrounds, text, glyphs): its own tiny program, so the
 * most-drawn pixels do not run through FS_2D's chain of mode branches */
static const char *FS_TEX =
   "#version 300 es\n"
   "precision mediump float;\n"
   "in highp vec2 v_uv; uniform sampler2D tex; uniform vec4 color, dim; out vec4 frag;\n"
   "void main(){ frag = texture(tex, v_uv) * color; frag.rgb = clamp(frag.rgb, 0.0, 1.0) * dim.a + dim.rgb; }\n";
static GLuint g_prog_tex;
static GLint  ut_mvp, ut_color, ut_uvw;

/* background crossfade: both layers mixed in one opaque full-screen pass (it was the
 * next layer alpha-blended over the current one: two full-screen passes for the 0.6 s
 * of every change of background). ma / mb: uv scale (xy) and offset (zw) of each layer */
static const char *VS_MIX =
   "#version 300 es\n"
   "layout(location=0) in vec3 pos; layout(location=1) in vec2 uv;\n"
   "uniform mat4 mvp; uniform vec4 ma, mb; out highp vec2 v_a; out highp vec2 v_b;\n"
   "void main(){ v_a=uv*ma.xy+ma.zw; v_b=uv*mb.xy+mb.zw; gl_Position=mvp*vec4(pos,1.0); }\n";
static const char *FS_MIX =
   "#version 300 es\n"
   "precision mediump float;\n"
   "in highp vec2 v_a; in highp vec2 v_b; uniform sampler2D ta, tb; uniform float k; uniform vec4 dim; out vec4 frag;\n"
   "void main(){ frag = vec4(mix(texture(ta, v_a).rgb, texture(tb, v_b).rgb, k) * dim.a + dim.rgb, 1.0); }\n";
static GLuint g_prog_mix;
static GLint  um_mvp, um_ma, um_mb, um_k;

/* Generated backgrounds, drawn straight into the frame. The picture (FS_2D mode 7) is a
 * gradient, two wide gaussian lights and the legibility scrim: nothing in it changes
 * faster than over a quarter of the screen. It was worked out per pixel into a 640x360
 * target 20 times a second (two exp, a sin and three smoothsteps each) and that target
 * then read back by a full-screen textured quad every frame. Here the same formula runs
 * per vertex of a GEN_NX x GEN_NY grid over the screen and the pixel only interpolates
 * the colour and adds the dither (no sin: interleaved gradient noise, a dot and two
 * fracts): no offscreen pass, no texture read, and the lights drift every frame instead
 * of every third. Linear interpolation over a cell is within 1/255 of the formula. Two
 * palettes are mixed by k, so a crossfade between two generated backgrounds is the same
 * single pass. A layer that fades to or from an image still goes through its target
 * (bg_gen_render) and FS_MIX. Setting bg_mesh=0 draws them all through the target. */
#define GEN_NX 20   /* 1280 triangles: finer grids (48 x 48) give the same picture to 1/255 and */
#define GEN_NY 32   /* cost more to set up than they save; the scrim's bands need the rows */
static const char *VS_GEN =
   "#version 300 es\n"
   "layout(location=0) in vec2 p;\n"   /* 0..1, y down */
   "uniform vec4 lights; uniform vec3 b0, a0, c0, b1, a1, c1; uniform float k, sd; out highp vec3 v_c;\n"
   "vec3 layer(vec3 b, vec3 a, vec3 acc){\n"
   "  float g = clamp(p.x * 0.55 + (1.0 - p.y) * 0.45, 0.0, 1.0);\n"
   "  vec3 c = mix(b, a * 0.70, g * g);\n"
   "  vec2 d1 = (p - lights.xy) * vec2(1.7778, 1.0), d2 = (p - lights.zw) * vec2(1.7778, 1.0);\n"
   "  return c + acc * 0.55 * exp(-dot(d1, d1) * 2.6) + a * 0.35 * exp(-dot(d2, d2) * 3.5); }\n"
   "void main(){\n"
   "  vec3 c = layer(b0, a0, c0); if (k > 0.0) c = mix(c, layer(b1, a1, c1), k);\n"
   "  float l = 1.0 - smoothstep(0.0, 0.74, p.x); l = 0.78 * l * (0.55 + 0.45 * l);\n"
   "  float b = 0.74 * smoothstep(0.48, 1.0, p.y), t = 0.52 * (1.0 - smoothstep(0.0, 0.32, p.y));\n"
   "  v_c = c * ((1.0 - l) * (1.0 - b) * (1.0 - t) * (1.0 - sd));\n"
   "  gl_Position = vec4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.0, 1.0); }\n";
static const char *FS_GEN =
   "#version 300 es\n"
   "precision mediump float;\n"
   "in highp vec3 v_c; uniform vec4 dim; out vec4 frag;\n"
   "void main(){ highp float n = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));\n"
   "  frag = vec4(clamp(v_c + (n - 0.5) * (1.5 / 255.0), 0.0, 1.0) * dim.a + dim.rgb, 1.0); }\n";
static GLuint g_prog_gen, g_vao_gen;
static GLint  ug_lights, ug_b[2], ug_a[2], ug_c[2], ug_k, ug_sd;
static int    g_bg_mesh = 1;

/* A scrim folded into what is under it. A menu's scrim was a full-screen quad of colour c
 * at opacity a blended over the finished dashboard: 2.07 Mpx more on every frame a menu
 * was open (6.5 Mpx in all with the settings, 6.9 with the game menu: 30 fps on the
 * console). Blending is linear: with every colour written under the scrim sent through
 * x * (1 - a) + c * a (the clear colour, the opaque background, and each blended draw's
 * own colour), the frame is what the quad would have left, to the rounding, and the quad
 * is not drawn. `dim` = (c * a, 1 - a) in the three 2D programs; (0, 0, 0, 1) is none. */
static GLint u2_dim, ut_dim, um_dim, ug_dim;
static int   g_scrim_fold = 1;   /* setting scrim_fold=0: the scrims are drawn as quads again */
static float g_dim[4] = { 0, 0, 0, 1 };
static void dim_apply(const float d[4])
{
   memcpy(g_dim, d, sizeof(g_dim));
   glUseProgram(g_prog_mix); glUniform4fv(um_dim, 1, d);
   glUseProgram(g_prog_gen); glUniform4fv(ug_dim, 1, d);
   glUseProgram(g_prog_tex); glUniform4fv(ut_dim, 1, d);
   glUseProgram(g_prog_2d);  glUniform4fv(u2_dim, 1, d);
}
static void dim_fold(const float c[3], float a)
{
   const float d[4] = { c[0] * a, c[1] * a, c[2] * a, 1.0f - a };
   dim_apply(d);
}
static void dim_none(void)
{
   static const float d[4] = { 0, 0, 0, 1 };
   if (g_dim[3] != 1.0f || g_dim[0] != 0.0f || g_dim[1] != 0.0f || g_dim[2] != 0.0f) dim_apply(d);
}
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

/* Program binaries: compiling and linking the shaders is paid at every start, and Lumen
 * starts again after every game. The driver's linked program is kept in
 * <home>/cache/shaders/<key>.bin (key: the driver's version and renderer strings and both
 * sources), loaded with glProgramBinary on the next start; a binary the driver refuses
 * (another driver, a cut-short file) is compiled from source and written again.
 * Setting shader_cache=0 turns it off. */
static int g_progs_cached, g_progs_built;
static unsigned fnv1a(unsigned h, const char *s) { while (*s) h = (h ^ (unsigned char)*s++) * 16777619u; return h; }
static void prog_cache_path(char *out, size_t n, const char *vs, const char *fs)
{
   unsigned a = 2166136261u, b = 0x9747b28cu;
   const char *ver = (const char *)glGetString(GL_VERSION), *ren = (const char *)glGetString(GL_RENDERER);
   a = fnv1a(fnv1a(fnv1a(fnv1a(a, ver ? ver : ""), ren ? ren : ""), vs), fs);
   b = fnv1a(fnv1a(fnv1a(fnv1a(b, fs), vs), ren ? ren : ""), LUMEN_VERSION);
   snprintf(out, n, "%s/cache/shaders/%08x%08x.bin", g_home, a, b);
}

static int prog_cache_load(GLuint p, const char *path)
{
   FILE *f = fopen(path, "rb");
   unsigned hdr[2]; unsigned char *buf; GLint ok = 0;
   if (!f) return 0;
   if (fread(hdr, sizeof(unsigned), 2, f) == 2 && hdr[1] > 0 && hdr[1] < (8u << 20) && (buf = malloc(hdr[1])))
   {
      if (fread(buf, 1, hdr[1], f) == hdr[1])
      {
         while (glGetError() != GL_NO_ERROR) { }
         glProgramBinary(p, (GLenum)hdr[0], buf, (GLsizei)hdr[1]);
         if (glGetError() == GL_NO_ERROR) glGetProgramiv(p, GL_LINK_STATUS, &ok);
      }
      free(buf);
   }
   fclose(f);
   return ok;
}

static void prog_cache_save(GLuint p, const char *path)
{
   char tmp[420], dir[420]; GLint len = 0; GLsizei got = 0; unsigned hdr[2]; GLenum fmt = 0; unsigned char *buf; FILE *f;
   glGetProgramiv(p, GL_PROGRAM_BINARY_LENGTH, &len);
   if (len <= 0 || !(buf = malloc((size_t)len))) return;
   while (glGetError() != GL_NO_ERROR) { }
   glGetProgramBinary(p, len, &got, &fmt, buf);
   if (glGetError() == GL_NO_ERROR && got > 0)
   {
      snprintf(dir, sizeof(dir), "%s/cache", g_home); mkdir(dir, 0755);
      snprintf(dir, sizeof(dir), "%s/cache/shaders", g_home); mkdir(dir, 0755);
      snprintf(tmp, sizeof(tmp), "%s.tmp", path);
      if ((f = fopen(tmp, "wb")))
      {
         hdr[0] = (unsigned)fmt; hdr[1] = (unsigned)got;
         int okw = fwrite(hdr, sizeof(unsigned), 2, f) == 2 && fwrite(buf, 1, (size_t)got, f) == (size_t)got;
         if (fclose(f) != 0) okw = 0;
         if (okw) rename(tmp, path); else unlink(tmp);
      }
   }
   free(buf);
}

static GLuint link_prog(const char *vs, const char *fs)
{
   static int use = -1;
   GLint ok; char info[1024], path[400];
   GLuint p = glCreateProgram(), v, f;
   if (use < 0)
   {
      GLint nf = 0;
      glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &nf);
      use = nf > 0 && settings_get_int("shader_cache", 1, 0, 1);
   }
   if (use)
   {
      prog_cache_path(path, sizeof(path), vs, fs);
      if (prog_cache_load(p, path)) { g_progs_cached++; return p; }
      glDeleteProgram(p); p = glCreateProgram();   /* a refused binary leaves no state behind */
   }
   v = compile(GL_VERTEX_SHADER, vs); f = compile(GL_FRAGMENT_SHADER, fs);
   glAttachShader(p, v);
   glAttachShader(p, f);
   glLinkProgram(p);
   glGetProgramiv(p, GL_LINK_STATUS, &ok);
   if (!ok) { glGetProgramInfoLog(p, sizeof(info), NULL, info); logf_("link error: %s", info); }
   else if (use) prog_cache_save(p, path);
   g_progs_built++;
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
   g_prog_2d = link_prog(VS_2D, FS_2D);
   u2_mvp = glGetUniformLocation(g_prog_2d, "mvp");
   u2_tex = glGetUniformLocation(g_prog_2d, "tex");
   u2_color = glGetUniformLocation(g_prog_2d, "color");
   u2_mode = glGetUniformLocation(g_prog_2d, "mode");
   u2_rect = glGetUniformLocation(g_prog_2d, "rect");
   u2_aux0 = glGetUniformLocation(g_prog_2d, "aux0");
   u2_aux1 = glGetUniformLocation(g_prog_2d, "aux1");
   u2_aux2 = glGetUniformLocation(g_prog_2d, "aux2");
   u2_uvw = glGetUniformLocation(g_prog_2d, "uvw");
   u2_dim = glGetUniformLocation(g_prog_2d, "dim");
   glUseProgram(g_prog_2d);
   glUniform4f(u2_uvw, 0.0f, 0.0f, 1.0f, 1.0f);
   glUniform4f(u2_dim, 0.0f, 0.0f, 0.0f, 1.0f);
   g_prog_tex = link_prog(VS_2D, FS_TEX);
   ut_mvp = glGetUniformLocation(g_prog_tex, "mvp");
   ut_color = glGetUniformLocation(g_prog_tex, "color");
   glUseProgram(g_prog_tex);
   glUniform1i(glGetUniformLocation(g_prog_tex, "tex"), 0);
   ut_uvw = glGetUniformLocation(g_prog_tex, "uvw");
   ut_dim = glGetUniformLocation(g_prog_tex, "dim");
   glUniform4f(ut_uvw, 0.0f, 0.0f, 1.0f, 1.0f);
   glUniform4f(ut_dim, 0.0f, 0.0f, 0.0f, 1.0f);
   g_prog_mix = link_prog(VS_MIX, FS_MIX);
   um_mvp = glGetUniformLocation(g_prog_mix, "mvp");
   um_ma = glGetUniformLocation(g_prog_mix, "ma");
   um_mb = glGetUniformLocation(g_prog_mix, "mb");
   um_k = glGetUniformLocation(g_prog_mix, "k");
   um_dim = glGetUniformLocation(g_prog_mix, "dim");
   glUseProgram(g_prog_mix);
   glUniform4f(um_dim, 0.0f, 0.0f, 0.0f, 1.0f);
   glUniform1i(glGetUniformLocation(g_prog_mix, "ta"), 0);
   glUniform1i(glGetUniformLocation(g_prog_mix, "tb"), 1);

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

   {  /* generated backgrounds: their program and the grid they are worked out on */
      static float gv[(GEN_NX + 1) * (GEN_NY + 1) * 2];
      static unsigned short gi[GEN_NX * GEN_NY * 6];
      GLuint vb, ib;
      int x, y, n = 0;
      g_prog_gen = link_prog(VS_GEN, FS_GEN);
      ug_lights = glGetUniformLocation(g_prog_gen, "lights");
      ug_b[0] = glGetUniformLocation(g_prog_gen, "b0"); ug_b[1] = glGetUniformLocation(g_prog_gen, "b1");
      ug_a[0] = glGetUniformLocation(g_prog_gen, "a0"); ug_a[1] = glGetUniformLocation(g_prog_gen, "a1");
      ug_c[0] = glGetUniformLocation(g_prog_gen, "c0"); ug_c[1] = glGetUniformLocation(g_prog_gen, "c1");
      ug_k = glGetUniformLocation(g_prog_gen, "k");
      ug_sd = glGetUniformLocation(g_prog_gen, "sd");
      ug_dim = glGetUniformLocation(g_prog_gen, "dim");
      glUseProgram(g_prog_gen);
      glUniform4f(ug_dim, 0.0f, 0.0f, 0.0f, 1.0f);
      for (y = 0; y <= GEN_NY; y++)
         for (x = 0; x <= GEN_NX; x++) { gv[n++] = (float)x / GEN_NX; gv[n++] = (float)y / GEN_NY; }
      for (n = 0, y = 0; y < GEN_NY; y++)
         for (x = 0; x < GEN_NX; x++)
         {
            const unsigned short v0 = (unsigned short)(y * (GEN_NX + 1) + x), v1 = v0 + 1, v2 = v0 + GEN_NX + 1, v3 = v2 + 1;
            gi[n++] = v0; gi[n++] = v2; gi[n++] = v1; gi[n++] = v1; gi[n++] = v2; gi[n++] = v3;
         }
      glGenVertexArrays(1, &g_vao_gen);
      glBindVertexArray(g_vao_gen);
      glGenBuffers(1, &vb);
      glBindBuffer(GL_ARRAY_BUFFER, vb);
      glBufferData(GL_ARRAY_BUFFER, sizeof(gv), gv, GL_STATIC_DRAW);
      glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 8, (void *)0);
      glEnableVertexAttribArray(0);
      glGenBuffers(1, &ib);
      glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ib);   /* kept by the VAO */
      glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(gi), gi, GL_STATIC_DRAW);
      glBindVertexArray(g_vao_quad);
      glBindBuffer(GL_ARRAY_BUFFER, g_vbo_quad);
   }

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
static int g_cover_mip = 2;   /* cover_trilinear=1 in settings.ini: 1, the covers sampled trilinear as before */
static GLuint surface_to_tex(SDL_Surface *s, int mip, float *avg)
{
   /* the workers hand over ABGR8888 already: no second copy of it on the render thread */
   const int ready = s->format->format == SDL_PIXELFORMAT_ABGR8888 && s->pitch == s->w * 4;
   GLuint t; SDL_Surface *c = ready ? s : SDL_ConvertSurfaceFormat(s, SDL_PIXELFORMAT_ABGR8888, 0);
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
   /* mip 2 (the dashboard's covers): bilinear in the nearest level, 4 texels a pixel.
    * Trilinear reads 8 (two levels) for every pixel of every cover, and a tile only has
    * two sizes on screen; what is scaled continuously (launch art, icons) stays trilinear */
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mip == 2 ? GL_LINEAR_MIPMAP_NEAREST : mip ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
   if (mip) glGenerateMipmap(GL_TEXTURE_2D);
   if (c != s) SDL_FreeSurface(c);
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
   double q0;
   if (!f || !s || !*s) { *w = *h = 0; return 0; }
   q0 = now_s();
   surf = TTF_RenderUTF8_Blended(f, s, white);
   if (!surf) { *w = *h = 0; return 0; }
   t = text_upload(surf, w, h);
   SDL_FreeSurface(surf);
   g_pf_text_ms += (now_s() - q0) * 1000.0; g_pf_text_n++;
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

static float smooth01(float e0, float e1, float x) { float t = clampf((x - e0) / (e1 - e0), 0, 1); return t * t * (3 - 2 * t); }
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
   t = surface_to_tex(card, g_cover_mip, NULL);
   SDL_FreeSurface(card);
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
}

static void game_finish_cover(game_t *g, SDL_Surface *s, const float avg[3], const float acc[3])
{
   if (s)
   {
      g->tex = surface_to_tex(s, g_cover_mip, NULL);
      g->tex_w = s->w; g->tex_h = s->h;
      memcpy(g->col, avg, sizeof(g->col));
      memcpy(g->acc, acc, sizeof(g->acc));
      g->tex_state = 1;
   }
   if (!g->tex) game_placeholder(g);
}

static void game_free_tex(game_t *g)
{
   if (g->tex) glDeleteTextures(1, &g->tex);
   g->tex = 0;
   g->tex_state = 0;
}

/* ------------------------------------------------------------------------ */
/* drawing                                                                   */
static mat4 g_ortho;

static void cost_add(float w, float h, int mode);
static void cost_add_at(float x, float y, float w, float h, int mode);
static int g_cost_on;

/* <home>/debug/launch-shots (a test switch, for the simulator): the launch clock steps
 * exactly 1/30 s per frame and every third frame (0.1 s) is saved to
 * <home>/debug/shots/launch_NNN.bmp, so the animation can be reviewed frame by frame
 * however slowly the machine renders it */
static int g_launch_shots;
static void launch_shot(int k)
{
   char path[400];
   SDL_Surface *sf = SDL_CreateRGBSurface(0, g_dw, g_dh, 32, 0x000000ff, 0x0000ff00, 0x00ff0000, 0xff000000);
   int y;
   unsigned char *row;
   if (!sf) return;
   glPixelStorei(GL_PACK_ALIGNMENT, 4);
   glReadPixels(0, 0, g_dw, g_dh, GL_RGBA, GL_UNSIGNED_BYTE, sf->pixels);
   row = malloc(sf->pitch);
   for (y = 0; row && y < g_dh / 2; y++)
   {  /* GL rows go bottom-up */
      unsigned char *a = (unsigned char *)sf->pixels + y * sf->pitch, *b = (unsigned char *)sf->pixels + (g_dh - 1 - y) * sf->pitch;
      memcpy(row, a, sf->pitch); memcpy(a, b, sf->pitch); memcpy(b, row, sf->pitch);
   }
   free(row);
   snprintf(path, sizeof(path), "%s/debug/shots/launch_%03d.bmp", g_home, k);
   if (SDL_SaveBMP(sf, path)) logf_("launch shot: %s", SDL_GetError());
   SDL_FreeSurface(sf);
}

static int    g_cost_on;

/* effect switches (all on; the bench mode turns them off one at a time) */
static int    g_fx_sides = 1, g_fx_text = 1;
static int    g_fx_shadows = 1, g_fx_bg = 1;   /* bench switches: dashboard shadows, background layers */
static float  g_bg_dim = 0.45f;                /* scrim opacity baked into backgrounds (setting bg_dim) */
static int    g_dash_profile_dirty = 1; /* dashboard: avatar + play statistics to reload */

/* cost meter (LUMEN_COST=1): device pixels covered per frame, split by blending and by
 * shader weight, plus draw calls; logged once per second. A proxy for GPU load that a
 * software renderer can measure too (tools/host). */
static double g_cost_px_opaque, g_cost_px_blend, g_cost_px_heavy, g_cost_draws;
/* a quad at x, y: only what of it is on screen is shaded (a shape that grows past the
 * frame was counted whole: 20 Mpx for the launch's circle) */
static void cost_add_at(float x, float y, float w, float h, int mode)
{
   float x0 = w < 0 ? x + w : x, x1 = w < 0 ? x : x + w, y0 = h < 0 ? y + h : y, y1 = h < 0 ? y : y + h;
   if (x0 < 0) x0 = 0;
   if (y0 < 0) y0 = 0;
   if (x1 > W) x1 = W;
   if (y1 > H) y1 = H;
   if (x1 <= x0 || y1 <= y0) { g_cost_draws += 1; return; }
   cost_add(x1 - x0, y1 - y0, mode);
}
static void cost_add(float w, float h, int mode)
{
   double px = fabs((double)w * h) * g_ui_scale * g_ui_scale;
   GLint vp[4];
   glGetIntegerv(GL_VIEWPORT, vp);
   if (vp[2] != g_dw) px *= (double)vp[2] * vp[3] / ((double)g_dw * g_dh);   /* offscreen FBO */
   if (glIsEnabled(GL_BLEND)) g_cost_px_blend += px; else g_cost_px_opaque += px;
   if (mode == 7 || mode == 10 || mode == 5) g_cost_px_heavy += px;
   g_cost_draws += 1;
}

void draw_2d(GLuint tex, float x, float y, float w, float h, const float rgba[4], int mode)
{
   if (g_cost_on) cost_add_at(x, y, w, h, mode);
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

/* A shape antialiased by its shader (modes 5, 6, 9) fades over the pixel its edge
 * crosses, but a quad the size of the shape stops at the edge: on its straight sides
 * the outer half of that fade was never drawn (the window's multisampling used to cover
 * for it). The quad is one device pixel larger on every side, the shape's own
 * coordinates unchanged (uvw). */
/* the theme's corner rounding (theme.ini radius): scales the radius of every rectangle,
 * card, tile and shadow; circles (avatars, discs) stay round */
static float g_rk = 1.0f;
static void draw_sdf(GLuint tex, float x, float y, float w, float h, const float rgba[4], int mode)
{
   const float p = 1.0f / g_ui_scale;
   glUniform4f(u2_uvw, -p / w, -p / h, (w + 2 * p) / w, (h + 2 * p) / h);
   draw_2d(tex, x - p, y - p, w + 2 * p, h + 2 * p, rgba, mode);
   glUniform4f(u2_uvw, 0.0f, 0.0f, 1.0f, 1.0f);
}

/* glass rounded rectangle (mode 5): the shader needs the size in pixels for its distance field */
void draw_round_rect(float x, float y, float w, float h, float radius, const float rgba[4], float border_alpha)
{
   const float k = g_ui_scale;   /* shape maths in device px: 1-px antialiased edges */
   glUniform4f(u2_rect, w * k, h * k, radius * g_rk * k, border_alpha);
   glUniform4f(u2_aux0, 2.0f * k, 0.0f, 0.0f, 0.0f);   /* border band: 2 logical px */
   draw_sdf(0, x, y, w, h, rgba, 5);
}

/* flat rounded rectangle (mode 9): filled, or stroked inside with width `stroke` */
/* A filled, opaque disc of radius r at cx, cy as large as the launch's field gets: the
 * square inside it is drawn without blending (the GPU then shades nothing of the
 * dashboard under it), the four segments around the square with the shape's shader as
 * before. Same pixels as draw_shape() of the whole disc. */
static void draw_disc(float cx, float cy, float r, const float rgba[4])
{
   const float k = g_ui_scale, p = 1.0f / k, hf = floorf(r * 0.70710678f) - 3.0f, d = 2.0f * r, e = r - hf;
   int i;
   if (hf < 24.0f || rgba[3] < 0.999f)
   {
      glUniform4f(u2_rect, d * k, d * k, r * k, 0.0f);
      glUniform4f(u2_aux0, 0.0f, 0.0f, 0.0f, 0.0f);
      draw_sdf(0, cx - r, cy - r, d, d, rgba, 9);
      return;
   }
   glDisable(GL_BLEND);
   draw_2d(0, cx - hf, cy - hf, 2.0f * hf, 2.0f * hf, rgba, 0);
   glEnable(GL_BLEND);
   glUniform4f(u2_rect, d * k, d * k, r * k, 0.0f);
   glUniform4f(u2_aux0, 0.0f, 0.0f, 0.0f, 0.0f);
   for (i = 0; i < 4; i++)
   {  /* top, bottom (full width, with the edge's 1 px of fade), left, right (between them) */
      /* each reaches 1 px into the square (same colour over it), so no seam can open */
      const float px = i < 2 ? -p : (i == 2 ? -p : r + hf - 1.0f), py = i == 0 ? -p : (i == 1 ? r + hf - 1.0f : e + 1.0f);
      const float pw = i < 2 ? d + 2 * p : e + p + 1.0f, ph = i < 2 ? e + p + 1.0f : 2.0f * hf - 2.0f;   /* the four never overlap each other */
      glUniform4f(u2_uvw, px / d, py / d, pw / d, ph / d);
      draw_2d(0, cx - r + px, cy - r + py, pw, ph, rgba, 9);
   }
   glUniform4f(u2_uvw, 0.0f, 0.0f, 1.0f, 1.0f);
}

/* A stroked shape (a focus ring) is empty inside: past the stroke and the corners its
 * alpha is exactly 0, but drawn as one quad every pixel of the inside still ran the
 * distance field and was blended over what the ring surrounds (the focused tile's ring:
 * 55 000 pixels for a line 3 px wide). It is drawn as the four strips of its frame
 * instead; the same pixels come out. */
static void draw_shape(float x, float y, float w, float h, float radius, float stroke, const float rgba[4])
{
   const float k = g_ui_scale, p = 1.0f / k;
   const float in = ceilf((fmaxf(radius, stroke) + 2.0f) * k) / k;   /* inside this inset: alpha 0 */
   glUniform4f(u2_rect, w * k, h * k, radius * g_rk * k, 0.0f);
   glUniform4f(u2_aux0, stroke * k, 0.0f, 0.0f, 0.0f);
   if (stroke > 0.0f && (w - 2 * in) * k >= 24.0f && (h - 2 * in) * k >= 24.0f)
   {
      int i;
      for (i = 0; i < 4; i++)
      {  /* top, bottom (full width, with the edge's 1 px of fade: draw_sdf()), left, right */
         const float px = i == 3 ? w - in : -p, py = i == 0 ? -p : (i == 1 ? h - in : in);
         const float pw = i < 2 ? w + 2 * p : in + p, ph = i < 2 ? in + p : h - 2 * in;
         glUniform4f(u2_uvw, px / w, py / h, pw / w, ph / h);
         draw_2d(0, x + px, y + py, pw, ph, rgba, 9);
      }
      glUniform4f(u2_uvw, 0.0f, 0.0f, 1.0f, 1.0f);
      return;
   }
   draw_sdf(0, x, y, w, h, rgba, 9);
}

/* soft shadow under a rounded rectangle x,y,w,h: blur radius, offset dy, opacity a;
 * cut = 1 leaves the area under the shape itself clear (translucent shapes) */
/* Baked shadows. FS_2D mode 10 evaluates one or two distance fields and a smoothstep
 * per pixel over the whole blurred quad (1.3 Mpx for the game menu's card at 1080p,
 * every frame). The falloff of a shape only depends on its size, so it is computed once
 * on the CPU with the shader's own formula, kept as a texture and drawn as a plain
 * textured quad. A soft shadow needs no resolution: its texels are up to 4 device px
 * (at least 15 across the blur radius). One with the shape cut out has the cut's 1 px
 * edge: it is baked on the device's own pixel grid for where the quad sits (fx, fy: the
 * fraction of a pixel its corner is at) and drawn texel for pixel. A size is baked the second frame in a row
 * it is asked for: one that is animating (the card growing) uses the shader meanwhile. */
typedef struct { float w, h, r, blur, dy, fx, fy; int cut; } shadow_key_t;
typedef struct { shadow_key_t k; GLuint tex; unsigned used; } shadow_tex_t;
static shadow_tex_t g_shadow_tex[32];
static struct { shadow_key_t k; unsigned frame; } g_shadow_miss[16];
static unsigned g_frame_id = 1;        /* frames presented (main loop and dialog) */
static unsigned g_shadow_bake_frame;   /* texels baked during g_frame_id == this */
static int      g_shadow_baked_px;

static int shadow_key_eq(const shadow_key_t *a, const shadow_key_t *b)
{
   return fabsf(a->w - b->w) < 0.126f && fabsf(a->h - b->h) < 0.126f && a->r == b->r && a->blur == b->blur &&
          a->cut == b->cut && (!a->cut || (a->dy == b->dy && fabsf(a->fx - b->fx) < 0.01f && fabsf(a->fy - b->fy) < 0.01f));
}

/* key in device px: quad size, corner radius, blur, offset of the cut */
static GLuint shadow_bake(const shadow_key_t *q)
{
   const float f = q->cut ? 1.0f : clampf(q->blur / 15.0f, 1.0f, 4.0f);
   const int tw = (int)ceilf(q->cut ? q->fx + q->w : q->w / f), th = (int)ceilf(q->cut ? q->fy + q->h : q->h / f);
   const float hx = q->w * 0.5f - q->blur, hy = q->h * 0.5f - q->blur;
   const float r = q->r < (hx < hy ? hx : hy) ? q->r : (hx < hy ? hx : hy);
   unsigned char *px;
   GLuint t;
   int x, y;
   if (tw < 2 || th < 2 || tw > 1024 || th > 1024 || !(px = malloc((size_t)tw * th * 4))) return 0;
   for (y = 0; y < th; y++)
      for (x = 0; x < tw; x++)
      {
         const float ppx = q->cut ? x + 0.5f - q->fx - q->w * 0.5f : ((x + 0.5f) / tw - 0.5f) * q->w;
         const float ppy = q->cut ? y + 0.5f - q->fy - q->h * 0.5f : ((y + 0.5f) / th - 0.5f) * q->h;
         float qx = fabsf(ppx) - (hx - r), qy = fabsf(ppy) - (hy - r);
         float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0, in = qx > qy ? qx : qy;
         float d = sqrtf(ox * ox + oy * oy) + (in < 0 ? in : 0) - r;
         float k = 1.0f - smooth01(-q->blur * 0.35f, q->blur, d);
         unsigned char *o = px + ((size_t)y * tw + x) * 4;
         k *= k;
         if (q->cut)
         {
            float d2;
            qx = fabsf(ppx) - (hx - r); qy = fabsf(ppy + q->dy) - (hy - r);
            ox = qx > 0 ? qx : 0; oy = qy > 0 ? qy : 0; in = qx > qy ? qx : qy;
            d2 = sqrtf(ox * ox + oy * oy) + (in < 0 ? in : 0) - r;
            k *= clampf(d2 + 0.5f, 0.0f, 1.0f);
            if (fabsf(ppx) > q->w * 0.5f || fabsf(ppy) > q->h * 0.5f) k = 0.0f;   /* pixel outside the quad */
         }
         o[0] = o[1] = o[2] = 255; o[3] = (unsigned char)(255.0f * k + 0.5f);
      }
   glGenTextures(1, &t);
   glBindTexture(GL_TEXTURE_2D, t);
   glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
   glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tw, th, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
   free(px);
   g_shadow_baked_px += tw * th;
   return t;
}

/* the baked texture of a shadow, 0 while it has none (the shader draws it) */
static GLuint shadow_lookup(const shadow_key_t *q)
{
   const int NT = (int)(sizeof(g_shadow_tex) / sizeof(g_shadow_tex[0])), NM = (int)(sizeof(g_shadow_miss) / sizeof(g_shadow_miss[0]));
   int i, slot = 0, seen = 0, ms = 0;
   for (i = 0; i < NT; i++)
      if (g_shadow_tex[i].tex && shadow_key_eq(&g_shadow_tex[i].k, q)) { g_shadow_tex[i].used = g_frame_id; return g_shadow_tex[i].tex; }
   for (i = 0; i < NM; i++)
   {
      if (g_shadow_miss[i].frame && shadow_key_eq(&g_shadow_miss[i].k, q))
      {
         if (g_shadow_miss[i].frame + 1 == g_frame_id) seen = 1;
         if (g_shadow_miss[i].frame == g_frame_id) return 0;   /* asked twice this frame, noted already */
         ms = i; break;
      }
      if (g_shadow_miss[i].frame < g_shadow_miss[ms].frame) ms = i;   /* else the oldest note is replaced */
   }
   if (g_shadow_bake_frame != g_frame_id) { g_shadow_bake_frame = g_frame_id; g_shadow_baked_px = 0; }
   if (!seen || g_shadow_baked_px > 150000)
   {  /* first frame of this size, or enough baked for one frame: note it, try the next */
      g_shadow_miss[ms].k = *q; g_shadow_miss[ms].frame = g_frame_id;
      return 0;
   }
   g_shadow_miss[ms].frame = 0;
   for (i = 0; i < NT; i++)
   {
      if (!g_shadow_tex[i].tex) { slot = i; break; }
      if (g_shadow_tex[i].used < g_shadow_tex[slot].used) slot = i;
   }
   if (g_shadow_tex[slot].tex) glDeleteTextures(1, &g_shadow_tex[slot].tex);
   g_shadow_tex[slot].k = *q; g_shadow_tex[slot].used = g_frame_id;
   g_shadow_tex[slot].tex = shadow_bake(q);
   return g_shadow_tex[slot].tex;
}

static void draw_shadow_ex(float x, float y, float w, float h, float radius, float blur, float dy, float a, int cut)
{
   const float c[4] = { 0, 0, 0, a };
   const float k = g_ui_scale;
   shadow_key_t q;
   GLuint tex;
   if (!g_fx_shadows || a <= 0.003f) return;
   q.w = (w + 2 * blur) * k; q.h = (h + 2 * blur) * k; q.r = radius * g_rk * k; q.blur = blur * k; q.dy = dy * k; q.cut = cut;
   q.fx = q.fy = 0.0f;
   if (cut)
   {
      q.fx = (x - blur) * k; q.fx -= floorf(q.fx);
      q.fy = (y - blur + dy) * k; q.fy -= floorf(q.fy);
   }
   if ((tex = shadow_lookup(&q)))
   {  /* draw_2d() mode 0 without its snap to device pixels (a shadow follows its shape);
       * a cut one covers whole device pixels from the one its corner is in */
      const float qx = cut ? floorf((x - blur) * k) / k : x - blur, qy = cut ? floorf((y - blur + dy) * k) / k : y - blur + dy;
      const float qw = cut ? ceilf(q.fx + q.w) / k : w + 2 * blur, qh = cut ? ceilf(q.fy + q.h) / k : h + 2 * blur;
      mat4 m = m4_mul(g_ortho, m4_mul(m4_translate(qx + qw * 0.5f, qy + qh * 0.5f, 0), m4_scale(qw, -qh, 1)));
      if (g_cost_on) cost_add(qw, qh, 0);
      glUseProgram(g_prog_tex);
      glUniformMatrix4fv(ut_mvp, 1, GL_FALSE, m.m);
      glUniform4fv(ut_color, 1, c);
      glBindTexture(GL_TEXTURE_2D, tex);
      glDrawArrays(GL_TRIANGLES, 0, 6);
      glUseProgram(g_prog_2d);
      return;
   }
   glUniform4f(u2_rect, (w + 2 * blur) * k, (h + 2 * blur) * k, radius * g_rk * k, 0.0f);
   glUniform4f(u2_aux0, blur * k, cut ? 1.0f : 0.0f, dy * k, 0.0f);
   draw_2d(0, x - blur, y - blur + dy, w + 2 * blur, h + 2 * blur, c, 10);
}
static void draw_shadow(float x, float y, float w, float h, float radius, float blur, float dy, float a)
{
   draw_shadow_ex(x, y, w, h, radius, blur, dy, a, 0);
}
void ui_shadow(float x, float y, float w, float h, float radius, float blur, float dy, float a)
{
   draw_shadow_ex(x, y, w, h, radius, blur, dy, a, 0);
}

/* A glass card on its shadow (ui_card). The shadow, at full strength under the card, and
 * the card's fill over it were two blended layers over the whole card: half a megapixel
 * each for the settings' card at 1080p, on top of the art and its scrim. Inside the card,
 * away from the rounded corners, the border and the shadow's falloff, the two are one
 * layer: a shadow of opacity s under a fill of opacity k is that fill at opacity
 * 1 - (1 - s)(1 - k), its colour scaled by k over that. So the inside (a rectangle on
 * whole device pixels) is drawn once, and the shadow and the card only as the four strips
 * around it. Needs the shadow's baked texture: until it exists (its first two frames, or
 * a size that is animating) both are drawn whole. Setting card_merge=0 always does. */
static int g_card_merge = 1;
void ui_glass_card(float x, float y, float w, float h, float radius, const float rgba[4], float border_alpha,
                   float blur, float dy, float sh_a)
{
   const float k = g_ui_scale, p = 1.0f / k;
   const float inset = fmaxf(radius + 4.0f, dy + 0.35f * blur + 5.0f);
   const float ix0 = ceilf((x + inset) * k) / k, iy0 = ceilf((y + inset) * k) / k;
   const float ix1 = floorf((x + w - inset) * k) / k, iy1 = floorf((y + h - inset) * k) / k;
   const float qx = x - blur, qy = y - blur + dy, qw = w + 2 * blur, qh = h + 2 * blur;
   const float sc[4] = { 0, 0, 0, sh_a };
   const float flat = theme_card_alpha() >= 1.0f ? 1.0f : 0.0f;   /* an opaque theme card is one flat colour: no glass light */
   shadow_key_t q;
   GLuint tex = 0;
   int i;
   if (g_card_merge && g_fx_shadows && sh_a > 0.003f && rgba[3] > 0.003f && ix1 - ix0 > 32.0f && iy1 - iy0 > 32.0f)
   {
      q.w = qw * k; q.h = qh * k; q.r = radius * g_rk * k; q.blur = blur * k; q.dy = dy * k; q.cut = 0; q.fx = q.fy = 0.0f;
      tex = shadow_lookup(&q);
   }
   if (!tex)
   {
      draw_shadow_ex(x, y, w, h, radius, blur, dy, sh_a, 0);
      glUniform4f(u2_rect, w * k, h * k, radius * g_rk * k, border_alpha);
      glUniform4f(u2_aux0, 2.0f * k, 0.0f, flat, 0.0f);
      draw_sdf(0, x, y, w, h, rgba, 5);
      return;
   }
   /* the shadow's frame: its quad without the inside */
   glUseProgram(g_prog_tex);
   glUniform4fv(ut_color, 1, sc);
   glBindTexture(GL_TEXTURE_2D, tex);
   for (i = 0; i < 4; i++)
   {
      const float sx = i < 2 ? qx : (i == 2 ? qx : ix1), sy = i == 0 ? qy : (i == 1 ? iy1 : iy0);
      const float sw = i < 2 ? qw : (i == 2 ? ix0 - qx : qx + qw - ix1), sh = i == 0 ? iy0 - qy : (i == 1 ? qy + qh - iy1 : iy1 - iy0);
      mat4 m = m4_mul(g_ortho, m4_mul(m4_translate(sx + sw * 0.5f, sy + sh * 0.5f, 0), m4_scale(sw, -sh, 1)));
      if (g_cost_on) cost_add_at(sx, sy, sw, sh, 0);
      glUniform4f(ut_uvw, (sx - qx) / qw, (sy - qy) / qh, sw / qw, sh / qh);
      glUniformMatrix4fv(ut_mvp, 1, GL_FALSE, m.m);
      glDrawArrays(GL_TRIANGLES, 0, 6);
   }
   glUniform4f(ut_uvw, 0.0f, 0.0f, 1.0f, 1.0f);
   glUseProgram(g_prog_2d);
   /* the card's frame (rounded corners, border; 1 px more outside for the edge's fade) */
   glUniform4f(u2_rect, w * k, h * k, radius * g_rk * k, border_alpha);
   glUniform4f(u2_aux0, 2.0f * k, 0.0f, flat, 0.0f);
   for (i = 0; i < 4; i++)
   {
      const float sx = i < 2 ? x - p : (i == 2 ? x - p : ix1), sy = i == 0 ? y - p : (i == 1 ? iy1 : iy0);
      const float sw = i < 2 ? w + 2 * p : (i == 2 ? ix0 - (x - p) : x + w + p - ix1), sh = i == 0 ? iy0 - (y - p) : (i == 1 ? y + h + p - iy1 : iy1 - iy0);
      glUniform4f(u2_uvw, (sx - x) / w, (sy - y) / h, sw / w, sh / h);
      draw_2d(0, sx, sy, sw, sh, rgba, 5);
   }
   {  /* the inside: fill and shadow as one layer */
      const float s = sh_a, kf = rgba[3], a = 1.0f - (1.0f - s) * (1.0f - kf);
      glUniform4f(u2_uvw, (ix0 - x) / w, (iy0 - y) / h, (ix1 - ix0) / w, (iy1 - iy0) / h);
      glUniform4f(u2_aux0, kf / a, a, flat, 0.0f);
      draw_2d(0, ix0, iy0, ix1 - ix0, iy1 - iy0, rgba, 11);
   }
   glUniform4f(u2_uvw, 0.0f, 0.0f, 1.0f, 1.0f);
}

/* label with a soft 1-2 px shadow, for text over bright art */
static void draw_text(const label_t *l, float x, float y, const float c[4])
{
   const float sh[4] = { 0, 0, 0, 0.45f * c[3] };
   if (!l->tex) return;
   draw_2d(l->tex, floorf(x) + 1, floorf(y) + 2, l->w, l->h, sh, 0);
   draw_2d(l->tex, floorf(x), floorf(y), l->w, l->h, c, 0);
}

#include "flat.inc"

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

/* The focused tile's glow (the game's light), baked once like its shadow: the tile's shape
 * with a wide, soft falloff (GLOW_BLUR px around a 220 px tile), tinted when drawn. One
 * small texture and one blended quad; nothing is blurred per frame. */
#define GLOW_BLUR 56.0f
static GLuint g_tile_glow;
static void make_tile_glow(void)
{
   enum { N = 96 };
   static unsigned char px[N * N * 4];
   const float full = 220.0f + 2.0f * GLOW_BLUR, hs = 110.0f, r = 14.0f;
   int x, y;
   for (y = 0; y < N; y++)
      for (x = 0; x < N; x++)
      {
         float qx = fabsf((x + 0.5f) / N * full - full * 0.5f) - (hs - r), qy = fabsf((y + 0.5f) / N * full - full * 0.5f) - (hs - r);
         float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0, in = qx > qy ? qx : qy;
         float d = sqrtf(ox * ox + oy * oy) + (in < 0 ? in : 0) - r;
         float k = 1.0f - smooth01(-6.0f, GLOW_BLUR, d);
         unsigned char *p = px + (y * N + x) * 4;
         p[0] = p[1] = p[2] = 255; p[3] = (unsigned char)(255.0f * k * k + 0.5f);
      }
   glGenTextures(1, &g_tile_glow);
   glBindTexture(GL_TEXTURE_2D, g_tile_glow);
   glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, N, N, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

/* one rectangle (px, py, pw, ph inside the w x h tile) of a tile, same pixels as the
 * whole tile would give there */
static void tile_part(GLuint tex, float x, float y, float w, float h, float px, float py, float pw, float ph,
                      const float rgba[4], int opaque)
{
   glUniform4f(u2_uvw, px / w, py / h, pw / w, ph / h);
   if (opaque) glDisable(GL_BLEND);
   draw_2d(tex, x + px, y + py, pw, ph, rgba, opaque ? 12 : 6);   /* the opaque part is the inside: no mask to work out */
   if (opaque) glEnable(GL_BLEND);
}

/* textured rounded tile (mode 6), the texture (tw x th) cover-fitted into w x h;
 * sheen < -1 = none.
 * A fully opaque tile is drawn in parts: its inside (everything but the rounded sides
 * and a 1 px rim) without blending, then the four edge strips blended as before. The
 * GPU is a tile-based deferred renderer: pixels under an unblended quad are never shaded,
 * so the full-screen background (the most expensive pass of the frame) and the focused
 * tile's shadow are skipped under the covers; a blended tile made it shade both.
 * Setting tile_split=0 draws them whole again (to compare frame rates on the console). */
static int g_tile_split = -1;
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
   glUniform4f(u2_rect, w * g_ui_scale, h * g_ui_scale, radius * g_rk * g_ui_scale, 0.0f);
   glUniform4f(u2_aux0, u0, v0, us, vs);
   glUniform4f(u2_aux1, sheen, light, 0.0f, 0.0f);
   if (g_tile_split < 0) g_tile_split = settings_get_int("tile_split", 1, 0, 1);
   if (g_tile_split && rgba[3] >= 0.999f && w > 2.0f * radius + 12.0f && h > 12.0f)
   {
      const float ix = radius + 1.0f, iy = 1.0f, p = 1.0f / g_ui_scale;   /* p: the edge's fade, see draw_sdf() */
      if (sheen <= -1.0f && light == 0.0f)
      {  /* no sheen, no top light (every tile but the focused one): inside, mode 6 is the
          * texture times the colour, so the inside goes through the plain program (no
          * distance field, no branches) at the same texture coordinates. Not draw_2d():
          * it would snap the quad to whole pixels, away from its edge strips */
         const float pw = w - 2 * ix, ph = h - 2 * iy;
         mat4 m = m4_mul(g_ortho, m4_mul(m4_translate(x + ix + pw * 0.5f, y + iy + ph * 0.5f, 0), m4_scale(pw, -ph, 1)));
         glDisable(GL_BLEND);
         if (g_cost_on) cost_add_at(x + ix, y + iy, pw, ph, 0);
         glUseProgram(g_prog_tex);
         glUniform4f(ut_uvw, u0 + ix / w * us, v0 + iy / h * vs, pw / w * us, ph / h * vs);
         glUniformMatrix4fv(ut_mvp, 1, GL_FALSE, m.m);
         glUniform4fv(ut_color, 1, rgba);
         glBindTexture(GL_TEXTURE_2D, tex ? tex : g_white);
         glDrawArrays(GL_TRIANGLES, 0, 6);
         glUniform4f(ut_uvw, 0.0f, 0.0f, 1.0f, 1.0f);
         glUseProgram(g_prog_2d);
         glEnable(GL_BLEND);
      }
      else tile_part(tex, x, y, w, h, ix, iy, w - 2 * ix, h - 2 * iy, rgba, 1);
      tile_part(tex, x, y, w, h, -p, -p, ix + p, h + 2 * p, rgba, 0);
      tile_part(tex, x, y, w, h, w - ix, -p, ix + p, h + 2 * p, rgba, 0);
      tile_part(tex, x, y, w, h, ix, -p, w - 2 * ix, iy + p, rgba, 0);
      tile_part(tex, x, y, w, h, ix, h - iy, w - 2 * ix, iy + p, rgba, 0);
      glUniform4f(u2_uvw, 0.0f, 0.0f, 1.0f, 1.0f);
      return;
   }
   draw_sdf(tex, x, y, w, h, rgba, 6);
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

/* A font file is read once, whole, and every size of it is opened from that copy.
 * Opened from the file, FreeType reads each glyph's outline from the stick the first time
 * the glyph is drawn: a round trip to the FUSE daemon in the middle of a frame whenever a
 * new title or a new letter appeared, and one more open and parse per size at start-up. */
static const void *font_bytes(const char *path, int *len)
{
   static struct { char path[300]; void *data; int len; } cache[8];
   static int n;
   FILE *f; long sz; void *d; int i;
   for (i = 0; i < n; i++) if (!strcmp(cache[i].path, path)) { *len = cache[i].len; return cache[i].data; }
   if (n >= 8 || !(f = fopen(path, "rb"))) return NULL;
   fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
   d = sz > 0 && sz < (16 << 20) ? malloc((size_t)sz) : NULL;
   if (d && fread(d, 1, (size_t)sz, f) != (size_t)sz) { free(d); d = NULL; }
   fclose(f);
   if (!d) return NULL;
   snprintf(cache[n].path, sizeof(cache[n].path), "%s", path);
   cache[n].data = d; cache[n].len = (int)sz;
   *len = (int)sz;
   return cache[n++].data;
}

static TTF_Font *open_font(const char **paths, int size)
{
   int i;
   for (i = 0; paths[i]; i++)
   {
      int len = 0;
      const void *d = font_bytes(paths[i], &len);
      SDL_RWops *rw = d ? SDL_RWFromConstMem(d, len) : NULL;
      TTF_Font *f = rw ? TTF_OpenFontRW(rw, 1, (int)(size * g_ui_scale + 0.5f)) : NULL;
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

/* The legibility scrim (the one in FS_2D mode 7) baked into a decoded background, so drawing it
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
      float l = 1.0f - sstep(0.0f, 0.74f, (x + 0.5f) / c->w);
      l = 0.78f * l * (0.55f + 0.45f * l);
      fx[x] = (unsigned short)((1.0f - l) * 256.0f);
   }
   for (y = 0; y < c->h; y++)
   {
      float v = (y + 0.5f) / c->h;
      float fy = (1.0f - 0.74f * sstep(0.48f, 1.0f, v)) * (1.0f - 0.52f * (1.0f - sstep(0.0f, 0.32f, v))) * (1.0f - dim);
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

/* The worker keeps the last three decoded backgrounds (scrim baked, 8 MB each at 1080p):
 * decoding a 1080p JPEG is the wait between settling on a game and its background, and
 * going back to the game just left, or on to the next one (bg_prefetch, asked for once
 * the selection has settled), finds it done. Worker thread only. */
typedef struct { char path[512]; float dim; SDL_Surface *s; unsigned used; } bg_cache_t;
static bg_cache_t bg_cache[3];
static unsigned   bg_cache_tick;
static char       bg_pre[512];      /* path to decode ahead (guarded by bg_mx) */
static int        bg_pre_new;

/* the decoded background for path, from the cache or decoded into it; NULL if unreadable */
static SDL_Surface *bg_cache_get(const char *path)
{
   const float dim = g_bg_dim;
   SDL_Surface *s, *c = NULL;
   int k, slot = 0;
   for (k = 0; k < 3; k++)
      if (bg_cache[k].s && bg_cache[k].dim == dim && !strcmp(bg_cache[k].path, path)) { bg_cache[k].used = ++bg_cache_tick; return bg_cache[k].s; }
   s = IMG_Load(path);
   if (s) { c = SDL_ConvertSurfaceFormat(s, SDL_PIXELFORMAT_ABGR8888, 0); SDL_FreeSurface(s); }
   if (!c) return NULL;
   bake_scrim(c, dim);
   for (k = 0; k < 3; k++)
   {
      if (!bg_cache[k].s) { slot = k; break; }
      if (bg_cache[k].used < bg_cache[slot].used) slot = k;
   }
   if (bg_cache[slot].s) SDL_FreeSurface(bg_cache[slot].s);
   bg_cache[slot].s = c; bg_cache[slot].dim = dim; bg_cache[slot].used = ++bg_cache_tick;
   snprintf(bg_cache[slot].path, sizeof(bg_cache[slot].path), "%s", path);
   return c;
}

static int bg_worker(void *unused)
{
   (void)unused;
   SDL_LockMutex(bg_mx);
   while (!bg_quit)
   {
      char path[512]; SDL_Surface *s, *c = NULL;
      int pre;
      while (!bg_req_new && !bg_pre_new && !bg_quit) SDL_CondWait(bg_cv, bg_mx);
      if (bg_quit) break;
      pre = !bg_req_new;   /* the request of the selection goes first */
      snprintf(path, sizeof(path), "%s", pre ? bg_pre : bg_req);
      if (pre) bg_pre_new = 0; else bg_req_new = 0;
      SDL_UnlockMutex(bg_mx);
      {
         const double w0 = now_s();
         s = path[0] ? bg_cache_get(path) : NULL;
         if (s && !pre) c = SDL_ConvertSurface(s, s->format, 0);   /* the render thread's own copy */
         if (g_perf_on && path[0]) logf_("perf: background %s by the worker in %.0f ms", pre ? "decoded ahead" : "handed over", (now_s() - w0) * 1000.0);
      }
      SDL_LockMutex(bg_mx);
      if (pre) continue;
      if (bg_done) SDL_FreeSurface(bg_done);
      bg_done = c;
      bg_done_failed = !c && path[0];
      snprintf(bg_done_path, sizeof(bg_done_path), "%s", path);
      if (!c && path[0]) logf_("background failed: %s", path);
   }
   SDL_UnlockMutex(bg_mx);
   return 0;
}

/* decode ahead a background the selection is likely to ask for next */
static void bg_prefetch(const char *path)
{
   SDL_LockMutex(bg_mx);
   snprintf(bg_pre, sizeof(bg_pre), "%s", path);
   bg_pre_new = 1;
   SDL_CondSignal(bg_cv);
   SDL_UnlockMutex(bg_mx);
}

static double g_bg_req_t;   /* perf probe: when the selection's background was asked for */
static void bg_request(const char *path)
{
   g_bg_req_t = now_s();
   SDL_LockMutex(bg_mx);
   snprintf(bg_req, sizeof(bg_req), "%s", path);
   bg_req_new = 1;
   SDL_CondSignal(bg_cv);
   SDL_UnlockMutex(bg_mx);
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

static int g_home_pump;   /* PS / Home pressed: Back presses still to send while something is open */
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
   {
      in_last_activity = now_s();
      if (e->type != SDL_KEYDOWN || !e->key.repeat) perf_input(e->common.timestamp);
   }
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
      case SDL_CONTROLLER_BUTTON_GUIDE:   /* PS / Home: back to the games, whatever is open */
         if (d) { g_home_pump = 12; in_last_activity = now_s(); }
         break;
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
      case SDLK_m: b = IN_START; break;       /* keyboards / psc-sim: Start alone (game menu) */
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
      { { "start" },    "btn.start",        "hint.options" },
      { { "l1", "r1" }, "btn.l1_r1",        "hint.page" },
      { { "select" },   "btn.select",       "hint.settings" } },
   [HINTS_GAMEMENU] = {   /* lumen-2 mockup "Game menu": Accept and Close, inside the sheet */
      { { "cross" },    "btn.cross",        "hint.select" },
      { { "circle" },   "btn.circle",       "hint.close" } },
   [HINTS_GAMEMENU_FOLDERS] = {
      { { "dpad" },     "btn.dpad",         "hint.move" },
      { { "cross" },    "btn.cross",        "hint.toggle" },
      { { "circle" },   "btn.circle",       "hint.fold" } },
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
   [HINTS_FILEPICKER] = {
      { { "dpad" },     "btn.dpad",         "hint.move" },
      { { "cross" },    "btn.cross",        "hint.select" },
      { { "l1", "r1" }, "btn.l1_r1",        "hint.page" },
      { { "circle" },   "btn.circle",       "hint.back" },
      { { "triangle" }, "btn.triangle",     "hint.cancel" } },
   [HINTS_TRANSFER] = {       /* USB transfer, browsing (transfer.h) */
      { { "cross" },    "btn.cross",        "hint.open" },
      { { "square" },   "btn.square",       "hint.mark" },
      { { "start" },    "btn.start",        "hint.copy" },
      { { "triangle" }, "btn.triangle",     "hint.options" },
      { { "circle" },   "btn.circle",       "hint.back" } },
   [HINTS_TRANSFER_COPY] = {  /* USB transfer, a copy running */
      { { "circle" },   "btn.circle",       "hint.cancel" } },
   [HINTS_AUDIT] = {          /* library check (audit.h) */
      { { "dpad" },     "btn.dpad",         "hint.move" },
      { { "l1", "r1" }, "btn.l1_r1",        "hint.page" },
      { { "cross" },    "btn.cross",        "hint.recheck" },
      { { "circle" },   "btn.circle",       "hint.back" } },
   [HINTS_WIFI] = {           /* Connections, Wi-Fi networks (conn.h) */
      { { "dpad" },     "btn.dpad",         "hint.move" },
      { { "cross" },    "btn.cross",        "hint.connect" },
      { { "square" },   "btn.square",       "hint.search" },
      { { "triangle" }, "btn.triangle",     "hint.forget" },
      { { "circle" },   "btn.circle",       "hint.back" } },
   [HINTS_KERNEL] = {         /* Settings, About, Kernel (kern.h): the offer */
      { { "dpad" },     "btn.dpad",         "hint.move" },
      { { "cross" },    "btn.cross",        "hint.select" },
      { { "square" },   "btn.square",       "hint.backup_only" },
      { { "circle" },   "btn.circle",       "hint.cancel" } },
   [HINTS_KERNEL_DONE] = {    /* done / failed */
      { { "dpad" },     "btn.dpad",         "hint.move" },
      { { "cross" },    "btn.cross",        "hint.select" },
      { { "circle" },   "btn.circle",       "hint.close" } },
   [HINTS_KERNEL_BUSY] = { { { NULL }, NULL, NULL } },   /* writing: nothing may interrupt it */
   [HINTS_PADWAIT] = {        /* no controller: plug a receiver / pair one (padwait.h) */
      { { "circle" },   "btn.circle",       "hint.not_now" } },
   [HINTS_BT] = {             /* Connections, Bluetooth controllers (conn.h) */
      { { "dpad" },     "btn.dpad",         "hint.move" },
      { { "cross" },    "btn.cross",        "hint.pair" },
      { { "triangle" }, "btn.triangle",     "hint.forget" },
      { { "circle" },   "btn.circle",       "hint.back" } },
   [HINTS_DASH_TABS] = {      /* dashboard, focus on the system tabs */
      { { "dpad" },     "btn.dpad",         "hint.platform" },
      { { "cross" },    "btn.cross",        "hint.select" },
      { { "select" },   "btn.select",       "hint.settings" } },
   [HINTS_DASH_PROFILE] = {   /* dashboard, focus on the profile */
      { { "cross" },    "btn.cross",        "section.profile" },
      { { "dpad" },     "btn.dpad",         "hint.move" },
      { { "select" },   "btn.select",       "hint.settings" } },
   [HINTS_ATTRACT] = { { { NULL }, NULL, NULL } },   /* the attract scene draws its own PRESS START */
   [HINTS_WELCOME] = {
      { { "cross" },    "btn.cross",        "hint.next" },
      { { "circle" },   "btn.circle",       "hint.back" },
      { { "triangle" }, "btn.triangle",     "hint.dont_show" } },
   [HINTS_WELCOME_LANG] = {
      { { "cross" },    "btn.cross",        "hint.select" },
      { { "dpad" },     "btn.dpad",         "hint.move" } },
   [HINTS_WELCOME_LAST] = {
      { { "cross" },    "btn.cross",        "hint.start" },
      { { "circle" },   "btn.circle",       "hint.back" },
      { { "triangle" }, "btn.triangle",     "hint.dont_show" } },
};
typedef struct { char name[16]; GLuint tex; int w, h; } icon_t;
static icon_t g_icons[56];
static int    g_nicons;
typedef struct { const icon_t *icon[3]; int have_icons; GLuint fb, txt; int fbw, fbh, tw, th; } hint_gl_t;
static hint_gl_t g_hint_gl[HINTS_COUNT][SET_HINTS];
static float     g_hint_cx = W * 0.5f;   /* horizontal centre of the help bar */
static float     g_hint_w = 0.0f;        /* width it may take; 0: centred in [0, 2 * g_hint_cx] */

/* the icon's picture from the stick (theme's glyphs/ or icons/ first, then the flat
 * glyphs, the pixel-art icons, the old layout); no GL: any thread */
static SDL_Surface *icon_file(const char *home, const char *name)
{
   char path[400], rel[64];
   snprintf(rel, sizeof(rel), "glyphs/%s.png", name);
   if (!theme_asset(path, sizeof(path), rel)) { snprintf(rel, sizeof(rel), "icons/%s.png", name); if (!theme_asset(path, sizeof(path), rel)) path[0] = 0; }
   if (!path[0]) snprintf(path, sizeof(path), "%s/assets/glyphs/%s.png", home, name);
   if (!file_exists(path)) snprintf(path, sizeof(path), "%s/assets/icons/%s.png", home, name);
   if (!file_exists(path)) snprintf(path, sizeof(path), "%s/icons/%s.png", home, name);   /* old layout */
   return IMG_Load(path);
}

/* Every icon the interface can show, read and decoded by the start-up's scan thread: an
 * icon first drawn in the middle of a session (a help bar set, a glyph) then costs an
 * upload, not a question and a read to the stick, which hang while the stick is busy
 * (157 ms and more per set on the console). */
static struct { const char *name; SDL_Surface *sf; int done; } g_icon_pre[] = {
   { "cross" }, { "circle" }, { "square" }, { "triangle" }, { "start" }, { "select" }, { "dpad" },
   { "l1" }, { "r1" }, { "l2" }, { "r2" }, { "pad" }, { "network" }, { "disc" }, { "cartridge" }, { "coin" },
   { "clock" }, { "gallery" }, { "info" }, { "settings" },
   /* the status bar glyphs of the built-in modules (mod_net.c) */
   { "wifi-0" }, { "wifi-1" }, { "wifi-2" }, { "wifi-3" }, { "bluetooth" }, { "ethernet" },
   { "chevron-right" }, { "chevron-down" }, { "players" }, { "calendar" }, { "star" },
   { "play" }, { "folder" }, { "save" }, { "exit" },
   { "user" }, { "screen" }, { "sound" }, { "library" }, { "modules" }, { "system" },
};
static void icons_preload(const char *home)
{
   int i;
   for (i = 0; i < (int)(sizeof(g_icon_pre) / sizeof(g_icon_pre[0])); i++)
   {
      g_icon_pre[i].sf = icon_file(home, g_icon_pre[i].name);
      g_icon_pre[i].done = 1;
   }
}

static const icon_t *icon_get(const char *home, const char *name)
{
   int i; SDL_Surface *sf = NULL; icon_t *ic;
   for (i = 0; i < g_nicons; i++) if (!strcmp(g_icons[i].name, name)) return &g_icons[i];
   if (g_nicons >= (int)(sizeof(g_icons) / sizeof(g_icons[0]))) return NULL;
   ic = &g_icons[g_nicons++];
   snprintf(ic->name, sizeof(ic->name), "%s", name);
   for (i = 0; i < (int)(sizeof(g_icon_pre) / sizeof(g_icon_pre[0])); i++)
      if (!strcmp(g_icon_pre[i].name, name) && g_icon_pre[i].done) break;
   if (i < (int)(sizeof(g_icon_pre) / sizeof(g_icon_pre[0]))) { sf = g_icon_pre[i].sf; g_icon_pre[i].sf = NULL; }
   else sf = icon_file(home, name);   /* not in the list, or asked before the scan thread got to it */
   if (sf) { ic->w = sf->w; ic->h = sf->h; ic->tex = surface_to_tex(sf, 1, NULL); SDL_FreeSurface(sf); }
   return ic;
}

/* (re)build the hint texts in the active language; icons stay */
/* A set's icons and texts are made the first time it is drawn: at start-up only the
 * carousel's (there are 12 sets, some 45 text textures and 12 icon files on the stick;
 * most sessions show two or three of them). */
static unsigned char g_hint_ready[HINTS_COUNT];
static const char   *g_hint_home = "";

static void hints_set_load(int s)
{
   int i, k;
   g_hint_ready[s] = 1;
   for (i = 0; i < SET_HINTS && g_hints[s][i].text_key; i++)
   {
      hint_gl_t *h = &g_hint_gl[s][i];
      h->have_icons = 1;
      for (k = 0; k < 3 && g_hints[s][i].icons[k]; k++)
      {
         h->icon[k] = icon_get(g_hint_home, g_hints[s][i].icons[k]);
         if (!h->icon[k] || !h->icon[k]->tex || h->icon[k]->h <= 0) h->have_icons = 0;
      }
      if (!h->have_icons)
         h->fb = text_tex(g_font_small, tr(g_hints[s][i].fallback_key), &h->fbw, &h->fbh);
      h->txt = text_tex(g_font_small, tr(g_hints[s][i].text_key), &h->tw, &h->th);
   }
}

/* the texts follow the language: dropped, made again when their set is next drawn */
static void hints_reload(void)
{
   int s, i;
   for (s = 0; s < HINTS_COUNT; s++)
   {
      for (i = 0; i < SET_HINTS && g_hints[s][i].text_key; i++)
      {
         hint_gl_t *h = &g_hint_gl[s][i];
         if (h->fb) glDeleteTextures(1, &h->fb);
         if (h->txt) glDeleteTextures(1, &h->txt);
         h->fb = h->txt = 0; h->fbw = h->fbh = h->tw = h->th = 0;
      }
      g_hint_ready[s] = 0;
   }
}

/* icons from $home/assets/icons/<name>.png (loaded once each, shared by the sets) */
static void hints_load(const char *home)
{
   g_hint_home = home;
   hints_reload();
   hints_set_load(HINTS_CAROUSEL);
}

/* section 5.3: text baked into textures follows the language; every other label is
 * set each frame through label_set(..., tr(...)) */
void ui_language_changed(void)
{
   modules_emit(MEV_LANGUAGE, i18n_language());
   hints_reload();
   ui_cards_changed();
}

/* the active profile changed: what the dashboard shows of it is reloaded */
void ui_cards_changed(void)
{
   g_dash_profile_dirty = 1;   /* also the dashboard's avatar and statistics */
   coll_load(g_home, profiles_active()->id);   /* and its favourites, folders and resume marks */
}

/* Display page: leave with exit code 12; run.sh / boot.sh restart the console so the
 * new output and compositor are set up from scratch */
static int g_want_reboot;
static int g_play_resume, g_play_asked;   /* Play came from the game menu / resume question */
void ui_request_reboot(void) { g_want_reboot = 1; logf_("restart requested (display settings)"); }
/* Settings > System > Software update: exit 13, boot.sh installs the staged update and restarts */
static int g_want_update;
void ui_request_update(void) { g_want_update = 1; logf_("restart requested (software update)"); }
/* theme: quit with /tmp/lumen-restart, run.sh starts the frontend again at once */
static int g_want_restart;
void ui_request_restart(void)
{
   FILE *f = fopen("/tmp/lumen-restart", "w");
   if (f) fclose(f);
   g_want_restart = 1;
   logf_("frontend restart requested (theme)");
}

/* the Sound page changed music / music_volume / ui_sounds */
void ui_sound_changed(void)
{
   int on = settings_get_int("music", 1, 0, 1);
   float vol = settings_get_int("music_volume", 60, 0, 100) / 100.0f;
   music_set_volume(on ? vol : 0.0f);
   music_sfx_level(settings_get_int("ui_sounds", 1, 0, 1) ? vol : 0.0f);   /* guidelines/06 "Sound" */
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
   if (!g_hint_ready[set]) hints_set_load(set);
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
   avail = g_hint_w > 0.0f ? g_hint_w : 2.0f * g_hint_cx - 32.0f;
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
typedef struct { GLuint tex; int w, h; int gen; float a[3], acc[3]; GLuint gfbo, gtex; double gen_t; } bgtex_t;
#define GEN_W 640    /* generated backgrounds render at 640x360 and are scaled up: */
#define GEN_H 360    /* smooth gradients, a tenth of the pixels of a 1080p pass */
#define GEN_DT 0.05  /* and are redrawn 20 times a second: the lights drift under 1 % of the
                      * screen per second, so a frame's step is below one of its pixels */
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

/* A decoded background goes to the GPU a strip per frame (about 2 MB): the whole 1080p
 * image is 8 MB, and copying it in one call held the frame it landed on. */
static struct { SDL_Surface *s; GLuint tex; int row; char path[512]; } bg_up;
static void bg_up_cancel(void)
{
   if (bg_up.s) SDL_FreeSurface(bg_up.s);
   if (bg_up.tex) glDeleteTextures(1, &bg_up.tex);
   memset(&bg_up, 0, sizeof(bg_up));
}

static void bg_poll_upload(void)
{
   SDL_Surface *s = NULL; char path[512] = "";
   int failed, got;
   SDL_LockMutex(bg_mx);
   s = bg_done; bg_done = NULL;
   failed = bg_done_failed; bg_done_failed = 0;
   got = s || failed;
   if (got) snprintf(path, sizeof(path), "%s", bg_done_path);
   SDL_UnlockMutex(bg_mx);
   if (got)
   {
      bg_up_cancel();   /* a newer result replaces an upload in progress */
      if (!s)
      {  /* unreadable image: the generated background of that game instead */
         if (!strcmp(path, bg_shown)) bg_push(bg_gen_layer(bg_game));
         return;
      }
      if (strcmp(path, bg_shown)) { SDL_FreeSurface(s); return; } /* stale */
      if (s->format->format != SDL_PIXELFORMAT_ABGR8888 || s->pitch != s->w * 4)
      {  /* not the worker's format (never happens): the plain upload */
         bgtex_t l;
         memset(&l, 0, sizeof(l));
         l.tex = surface_to_tex(s, 0, NULL);
         l.w = s->w; l.h = s->h;
         SDL_FreeSurface(s);
         bg_push(l);
         return;
      }
      bg_up.s = s;
      snprintf(bg_up.path, sizeof(bg_up.path), "%s", path);
      glGenTextures(1, &bg_up.tex);
      glBindTexture(GL_TEXTURE_2D, bg_up.tex);
      glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, s->w, s->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
   }
   if (!bg_up.s) return;
   if (strcmp(bg_up.path, bg_shown)) { bg_up_cancel(); return; }   /* the selection moved on */
   {
      SDL_Surface *u = bg_up.s;
      int rows = (2 * 1024 * 1024) / (u->w * 4 > 0 ? u->w * 4 : 1);
      if (rows < 16) rows = 16;
      if (rows > u->h - bg_up.row) rows = u->h - bg_up.row;
      glBindTexture(GL_TEXTURE_2D, bg_up.tex);
      glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
      glTexSubImage2D(GL_TEXTURE_2D, 0, 0, bg_up.row, u->w, rows, GL_RGBA, GL_UNSIGNED_BYTE,
                      (const unsigned char *)u->pixels + (size_t)bg_up.row * u->pitch);
      bg_up.row += rows;
      if (bg_up.row >= u->h)
      {
         bgtex_t l;
         memset(&l, 0, sizeof(l));
         l.tex = bg_up.tex; l.w = u->w; l.h = u->h;
         bg_up.tex = 0;
         bg_up_cancel();
         bg_push(l);
         if (g_perf_on) logf_("perf: background ready %.0f ms after it was asked for", (now_s() - g_bg_req_t) * 1000.0);
      }
   }
}

/* a generated layer's colours: A a bit more saturated around its mean, base = A darkened,
 * the accent a bit more saturated too */
static void bg_gen_palette(const bgtex_t *b, float a[3], float acc[3], float base[3])
{
   int k;
   memcpy(a, b->a, 3 * sizeof(float)); memcpy(acc, b->acc, 3 * sizeof(float));
   for (k = 0; k < 3; k++)
   {
      float m = (a[0] + a[1] + a[2]) / 3.0f;
      a[k] = clampf(m + (a[k] - m) * 1.35f, 0.0f, 1.0f);
      base[k] = a[k] * 0.22f + 0.015f;
   }
   for (k = 0; k < 3; k++) { float m = (acc[0] + acc[1] + acc[2]) / 3.0f; acc[k] = clampf(m + (acc[k] - m) * 1.2f, 0.0f, 1.0f); }
}
/* where its two lights are at t (they drift slowly) */
static void bg_gen_lights(double t, float r[4])
{
   r[0] = (float)(0.80 + 0.06 * sin(t * 0.314)); r[1] = (float)(0.20 + 0.06 * cos(t * 0.271));
   r[2] = (float)(0.22 + 0.06 * cos(t * 0.231)); r[3] = (float)(0.88 + 0.05 * sin(t * 0.293));
}

/* a generated layer's picture: its palette and the scrim rendered into its small FBO
 * (g_prog_2d bound on entry and on return, the frame's target and viewport restored) */
static void bg_gen_render(bgtex_t *b, double t)
{
   const float one[4] = { 1, 1, 1, 1 };
   float keep_dim[4];
   float a[3], acc[3], base[3], r[4];
   if (b->gfbo && t >= b->gen_t && t - b->gen_t < GEN_DT) return;   /* its picture is recent enough */
   b->gen_t = t;
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
   bg_gen_palette(b, a, acc, base);
   bg_gen_lights(t, r);
   {  /* the picture itself is not under a folded scrim: the quad that shows it is */
      static const float none[4] = { 0, 0, 0, 1 };
      memcpy(keep_dim, g_dim, sizeof(keep_dim));
      glUniform4fv(u2_dim, 1, none);
   }
   glBindFramebuffer(GL_FRAMEBUFFER, b->gfbo);
   glViewport(0, 0, GEN_W, GEN_H);
   glDisable(GL_BLEND);
   glUniform4fv(u2_rect, 1, r);
   glUniform4f(u2_aux0, base[0], base[1], base[2], g_bg_dim);   /* .w: same scrim as the baked images */
   glUniform4f(u2_aux1, a[0], a[1], a[2], 0);
   glUniform4f(u2_aux2, acc[0], acc[1], acc[2], 0);
   draw_2d(0, 0, 0, W, H, one, 7);
   glEnable(GL_BLEND);
   glBindFramebuffer(GL_FRAMEBUFFER, crt_target());   /* the window, or the CRT filter's target */
   glViewport(0, 0, g_dw, g_dh);
   glUniform4fv(u2_dim, 1, keep_dim);
}

/* one or two generated layers (b1 mixed over b0 by k) straight into the frame: VS_GEN.
 * g_prog_2d bound on return, like the other background passes */
static void bg_gen_direct(const bgtex_t *b0, const bgtex_t *b1, float k, double t)
{
   const bgtex_t *l[2] = { b0, b1 };
   float a[3], acc[3], base[3], r[4];
   int i;
   glDisable(GL_BLEND);
   if (g_cost_on) cost_add(W, H, 0);
   glUseProgram(g_prog_gen);
   bg_gen_lights(t, r);
   glUniform4fv(ug_lights, 1, r);
   for (i = 0; i < 2; i++)
   {
      bg_gen_palette(l[i], a, acc, base);
      glUniform3fv(ug_b[i], 1, base); glUniform3fv(ug_a[i], 1, a); glUniform3fv(ug_c[i], 1, acc);
   }
   glUniform1f(ug_k, k);
   glUniform1f(ug_sd, g_bg_dim);
   glBindVertexArray(g_vao_gen);
   glDrawElements(GL_TRIANGLES, GEN_NX * GEN_NY * 6, GL_UNSIGNED_SHORT, (void *)0);
   glBindVertexArray(g_vao_quad);
   glUseProgram(g_prog_2d);
   glEnable(GL_BLEND);
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
      bg_gen_render(b, t);
      if (opaque) glDisable(GL_BLEND);
      draw_2d(b->gtex, 0, H, W, -H, c, 0);   /* FBO rows are bottom-up */
   }
   if (opaque) glEnable(GL_BLEND);
}

/* the texture of a layer and the uv scale / offset (m) that puts it on a full-screen
 * quad exactly as bg_draw() does */
static GLuint bg_layer_tex(bgtex_t *b, double t, float m[4])
{
   if (b->tex)
   {
      float sx = (float)W / b->w, sy = (float)H / b->h, sc = sx > sy ? sx : sy, w = b->w * sc, h = b->h * sc;
      m[0] = W / w; m[1] = H / h; m[2] = (w - W) * 0.5f / w; m[3] = (h - H) * 0.5f / h;
      return b->tex;
   }
   bg_gen_render(b, t);
   m[0] = 1.0f; m[1] = -1.0f; m[2] = 0.0f; m[3] = 1.0f;   /* FBO rows are bottom-up */
   return b->gtex;
}

/* the background of the frame: the current layer, the next one crossfading over it
 * (e: 0 -> 1). While both show, one opaque pass samples the two (FS_MIX) */
static void bg_draw_layers(float e, double t)
{
   float ma[4], mb[4];
   GLuint ta, tb;
   mat4 m;
   if (!g_fx_bg) return;
   if (g_bg_mesh && !g_fx_bgflat)
   {  /* generated layers only: one pass over the grid, no target, no texture */
      const int only_cur = bg_empty(&bg_next) || e <= 0.001f, only_next = !only_cur && e >= 0.999f;
      if (only_cur && bg_cur.gen) { bg_gen_direct(&bg_cur, &bg_cur, 0.0f, t); return; }
      if (only_next && bg_next.gen) { bg_gen_direct(&bg_next, &bg_next, 0.0f, t); return; }
      if (!only_cur && !only_next && bg_cur.gen && bg_next.gen) { bg_gen_direct(&bg_cur, &bg_next, e, t); return; }
   }
   if (bg_empty(&bg_next) || e <= 0.001f) { bg_draw(&bg_cur, 1.0f, t); return; }
   if (e >= 0.999f) { bg_draw(&bg_next, 1.0f, t); return; }   /* covers the current one */
   if (bg_empty(&bg_cur) || g_fx_bgflat)
   {  /* nothing under it yet (startup: over the clear colour), or the bench's flat fill */
      bg_draw(&bg_cur, 1.0f, t);
      bg_draw(&bg_next, e, t);
      return;
   }
   ta = bg_layer_tex(&bg_cur, t, ma);
   tb = bg_layer_tex(&bg_next, t, mb);
   if (g_cost_on) { glDisable(GL_BLEND); cost_add(W, H, 0); }
   m = m4_mul(g_ortho, m4_mul(m4_translate(W * 0.5f, H * 0.5f, 0), m4_scale(W, -H, 1)));
   glDisable(GL_BLEND);
   glUseProgram(g_prog_mix);
   glUniformMatrix4fv(um_mvp, 1, GL_FALSE, m.m);
   glUniform4fv(um_ma, 1, ma);
   glUniform4fv(um_mb, 1, mb);
   glUniform1f(um_k, e);
   glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, tb);
   glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, ta);
   glDrawArrays(GL_TRIANGLES, 0, 6);
   glUseProgram(g_prog_2d);
   glEnable(GL_BLEND);
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
      splash_pulse();
   }
}

static int g_lib_rescan;   /* set by ui_library_changed(), handled at the top of the next frame */

/* a setting that changes which systems are listed (show_stock_games) */
void ui_library_changed(void) { g_lib_rescan = 1; }
static void coll_rows_build(void);

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
   g_ngames = 0; g_nplat = 0; g_nreal_games = 0; g_nreal_plat = 0;
   scan_library();
   build_platform_index();
   platforms_load_art();
   coll_rows_build();
   *sel_plat = 0; *sel = 0;
   for (p = 0; p < g_nplat; p++)
      if (!strcmp(g_plat[p].id, sys))
      {
         *sel_plat = p;
         for (k = 0; k < g_plat[p].count; k++)
            if (!strcmp(g_games[g_plat[p].first + k].content, content)) { *sel = k; break; }
         break;
      }
   attract_library_changed();
   bg_shown[0] = 0;   /* request the selection again (layers keep their own palette) */
   bg_game = -1;
   logf_("library rescan: games %d, platforms %d, %.0f ms", g_ngames, g_nplat, (now_s() - t0) * 1000);
}

/* ---- collections as dashboard rows: after the systems, one row per folder, then
 * Favourites (one press Up from the first system, the rows wrap). A row repeats the
 * library's games (copies with their own textures; game_t.platform stays the game's
 * real system, so launching, art and attract use it); the media fields of the row follow
 * the selected game every frame (coll_row_follow). */
static unsigned g_coll_built;   /* coll_generation() the rows were built from */

static int cmp_gi_title(const void *a, const void *b)
{
   return strcasecmp(g_games[*(const int *)a].title, g_games[*(const int *)b].title);
}

static int game_by_content(const char *c)
{
   int i;
   for (i = 0; i < g_nreal_games; i++) if (!strcmp(g_games[i].content, c)) return i;
   return -1;
}

static void coll_row_add(const char *id, const char *name, int folder)
{
   static int idx[MAX_GAMES];
   int n = 0, k, total = folder < 0 ? coll_fav_count() : coll_folder_count(folder);
   platform_t *p;
   if (g_nplat >= MAX_PLATFORMS) return;
   for (k = 0; k < total; k++)
   {
      const char *c = folder < 0 ? coll_fav_at(k) : coll_folder_at(folder, k);
      int gi = c ? game_by_content(c) : -1;
      if (gi >= 0 && g_ngames + n < MAX_GAMES) idx[n++] = gi;
   }
   if (!n) return;   /* nothing of it on this stick */
   qsort(idx, n, sizeof(int), cmp_gi_title);
   p = &g_plat[g_nplat];
   *p = g_plat[g_games[idx[0]].platform];   /* media fields: the first game's system */
   snprintf(p->id, sizeof(p->id), "%s", id);
   snprintf(p->name, sizeof(p->name), "%s", name);
   p->logo[0] = 0; p->logo_tex = 0; p->label = 0; p->show_name = 1;
   p->has_gfx = 0; p->has_native = 0; p->demo_allowed = 0;
   p->first = g_ngames; p->count = n;
   p->label = text_tex(g_font_tab ? g_font_tab : g_font_small, p->name, &p->label_w, &p->label_h);
   for (k = 0; k < n; k++)
   {
      game_t *g = &g_games[g_ngames++];
      *g = g_games[idx[k]];
      g->tex = 0; g->tex_state = 0;
   }
   g_nplat++;
}

static void coll_rows_build(void)
{
   int i, f;
   char id[32];
   /* drop the previous rows (their textures, the cover worker's results for them) */
   if (g_nreal_games || g_nreal_plat)   /* 0: first build after a scan, nothing to drop */
   {
      for (i = g_nreal_games; i < g_ngames; i++) game_free_tex(&g_games[i]);
      for (i = g_nreal_plat; i < g_nplat; i++) if (g_plat[i].label) glDeleteTextures(1, &g_plat[i].label);
      SDL_LockMutex(cv_mx);
      cv_gen++;
      if (cv_req >= 0) { cv_req = -1; cv_busy = 0; }
      if (cv_done >= 0) { if (cv_surf) SDL_FreeSurface(cv_surf); cv_surf = NULL; cv_done = -1; cv_busy = 0; }
      SDL_UnlockMutex(cv_mx);
      /* the real games in flight are dropped too: they are requested again */
      for (i = 0; i < g_nreal_games; i++) if (g_games[i].tex_state == 2) g_games[i].tex_state = 0;
   }
   g_ngames = g_nreal_games ? g_nreal_games : g_ngames;
   g_nplat = g_nreal_plat ? g_nreal_plat : g_nplat;
   g_nreal_games = g_ngames; g_nreal_plat = g_nplat;
   if (g_nreal_games)
   {
      for (f = 0; f < coll_nfolders(); f++) { snprintf(id, sizeof(id), "folder-%d", f + 1); coll_row_add(id, coll_folder_name(f), f); }
      coll_row_add("fav", tr("coll.favorites"), -1);
   }
   g_coll_built = coll_generation();
   logf_("collections: %d rows, %d games", g_nplat - g_nreal_plat, g_ngames - g_nreal_games);
}

/* a collection row draws and launches like the selected game's system */
static void coll_row_follow(platform_t *row, int gi)
{
   const platform_t *src;
   if (gi < 0 || gi >= g_ngames) return;
   src = &g_plat[g_games[gi].platform];
   row->kind = src->kind;
   memcpy(row->flat, src->flat, sizeof(row->flat));
   memcpy(row->sound, src->sound, sizeof(row->sound));
}

/* rebuild after a change in the game menu, keeping the selection on the same game
 * (same row id + content), else the same row, else the last system */
static void coll_rows_rebuild(int *sel_plat, int *sel)
{
   char id[32] = "", content[512] = "";
   int p, k, was_virtual = *sel_plat >= g_nreal_plat;
   if (*sel_plat >= 0 && *sel_plat < g_nplat)
   {
      snprintf(id, sizeof(id), "%s", g_plat[*sel_plat].id);
      if (*sel >= 0 && *sel < g_plat[*sel_plat].count) snprintf(content, sizeof(content), "%s", g_games[g_plat[*sel_plat].first + *sel].content);
   }
   coll_rows_build();
   if (!was_virtual) return;   /* real rows keep their indices */
   for (p = g_nreal_plat; p < g_nplat; p++)
      if (!strcmp(g_plat[p].id, id))
      {
         *sel_plat = p;
         if (*sel >= g_plat[p].count) *sel = g_plat[p].count - 1;
         for (k = 0; k < g_plat[p].count; k++)
            if (!strcmp(g_games[g_plat[p].first + k].content, content)) { *sel = k; break; }
         return;
      }
   *sel_plat = g_nreal_plat > 0 ? g_nreal_plat - 1 : 0; *sel = 0;
}


/* ------------------------------------------------------------------------ */
/* dashboard (docs/dashboard-design.md): profile + status bar, system tabs, a row of
 * rounded cover tiles anchored on the selection, title and play statistics          */
/* blocks keep a TV-safe margin: many TVs overscan ~3 % of each edge */
#define DASH_X    84.0f    /* left / right margin of every block */
#define TOP_Y     40.0f    /* profile + status row */
#define TILE      146.0f   /* a cover at rest: its height (the width follows the system's box) */
#define TILE_SEL  196.0f
#define TILE_GAP  14.0f
#define SHELF_B   614.0f   /* the shelf: every cover stands on this line, the art owns the middle */
#define TILE_Y    (SHELF_B - TILE_SEL)
#define TAB_Y     112.0f
#define INFO_Y    212.0f   /* the hero block: kicker, title, chips */
#define DASH_PAGE 6        /* games per L1 / R1 page: the tiles visible right of the selection */
static status_t  g_status;
static char      g_menu_music[512];          /* <home>/assets/music/menu.ogg or "" */
static struct { char id[16]; GLuint tex; int w, h; label_t initials; } g_avatar;
/* dashboard focus: Up from the games reaches the system tabs, Up again the profile */
enum { FOCUS_GAMES, FOCUS_TABS, FOCUS_PROFILE };
static int g_focus;
static float g_dash_body = 1.0f;   /* the Home fades out under the settings, status row included */
static int g_avatar_dirty;   /* avatar.png of the active profile was replaced or removed */

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

void ui_avatar_changed(void) { g_avatar_dirty = 1; }

int ui_avatar(GLuint *tex, int *w, int *h)
{
   const profile_t *ap = profiles_active();
   if (g_avatar_dirty || strcmp(g_avatar.id, ap->id)) { g_avatar_dirty = 0; avatar_load(ap); }
   if (tex) *tex = g_avatar.tex;
   if (w) *w = g_avatar.w;
   if (h) *h = g_avatar.h;
   return g_avatar.tex != 0;
}

GLuint tex_from_surface(SDL_Surface *s) { return surface_to_tex(s, 1, NULL); }

void ui_shape(float x, float y, float w, float h, float radius, float stroke, const float rgba[4])
{
   draw_shape(x, y, w, h, radius, stroke, rgba);
}

void ui_avatar_draw(float x, float y, float d, float a)
{
   const profile_t *ap = profiles_active();
   const float c[4] = { 1, 1, 1, a }, ring[4] = { 1, 1, 1, 0.35f * a };
   if (a <= 0.003f) return;
   if (ui_avatar(NULL, NULL, NULL)) draw_tile(g_avatar.tex, g_avatar.w, g_avatar.h, x, y, d, d, d * 0.5f, -2.0f, 0.0f, c);
   else
   {
      float sw[4], k = d / 48.0f;
      profiles_color(ap->color, sw); sw[3] = a;
      draw_shape(x, y, d, d, d * 0.5f, 0, sw);
      if (g_avatar.initials.tex)
         draw_2d(g_avatar.initials.tex, x + d * 0.5f - g_avatar.initials.w * k * 0.5f, y + d * 0.5f - g_avatar.initials.h * k * 0.5f,
                 g_avatar.initials.w * k, g_avatar.initials.h * k, c, 0);
   }
   if (d >= 40.0f) draw_shape(x - 3, y - 3, d + 6, d + 6, d * 0.5f + 3, 1.5f, ring);
   gl_use_2d();
}

void draw_image(GLuint tex, int tw, int th, float x, float y, float w, float h, float radius, const float rgba[4])
{
   draw_tile(tex, tw, th, x, y, w, h, radius, -2.0f, 0.0f, rgba);
}

/* horizontal position and size of a tile at offset d from the selection (continuous,
 * so the row slides and the selected tile grows while the selection spring moves) */
static float tile_x(float d, float ar)   /* ar: the system's box proportions (platform_t tile_ar): the tiles' width */
{
   if (d >= 1.0f) return DASH_X + TILE_SEL * ar + TILE_GAP + (d - 1.0f) * (TILE * ar + TILE_GAP);
   if (d >= 0.0f) return DASH_X + d * (TILE_SEL * ar + TILE_GAP);
   return DASH_X + d * (TILE * ar + TILE_GAP);
}
/* games per L1 / R1 page: what the row shows right of the selection (6 square tiles) */
static int dash_page(float ar) { int n = (int)(DASH_PAGE / ar + 0.5f); return n < 3 ? 3 : n > 10 ? 10 : n; }
static float tile_size(float d) { float a = fabsf(d); return a >= 1.0f ? TILE : lerpf(TILE_SEL, TILE, a); }   /* the height */

void glyph_draw(const char *name, float x, float y, float h, const float c[4])
{
   const icon_t *ic = icon_get(g_home, name);
   if (ic && ic->tex && ic->h > 0) draw_2d(ic->tex, x, y, h * ic->w / (float)ic->h, h, c, 0);
}
float glyph_w(const char *name, float h)
{
   const icon_t *ic = icon_get(g_home, name);
   return ic && ic->tex && ic->h > 0 ? h * ic->w / (float)ic->h : 0.0f;
}

/* Lumen PSX logo (assets/brand/, tools/make-brand.py): the frontal LPSX, one texture
 * per letter (cut from the PNG at the gaps between letters) so the letters can move and
 * fill one by one. Two sizes:
 *  - logo-front[-1080].png, top centre of the dashboard: whenever the dashboard appears,
 *    L, P, S, X drop into place with a fade, 90 ms apart;
 *  - logo-front-splash[-1080].png, the startup splash: the logo fades in from black with
 *    its letters dark, and they fill with colour voxel row by voxel row, L to X, as
 *    startup goes on; then the splash fades into the dashboard. The boot payload's
 *    splash.png is this same first frame (the dark logo, same place: tools/make-brand.py),
 *    and boot.sh keeps it up until Lumen draws, so the two read as one splash: after a
 *    console boot the fade-in is skipped (see main).
 * Missing PNG: nothing is drawn (plain black splash, the top bar as it was). */
#define LOGO_W_VOX   28.5f   /* content width of logo-front.svg in voxels: 4 x 6 + 3 x 1.5 */
#define LOGO_H_VOX   8.0f    /* letter height in voxels (the extrusion hangs below) */
#define SPLASH_DARK  0.22f   /* brightness of an unfilled letter */
typedef struct { int tried; GLuint tex[4]; float x[4], w[4], h, vox; } logo_t;
static logo_t g_logo, g_logo_big;
static double g_logo_t0 = -1.0;   /* time the entry animation started; -1: replay on next show */

static void logo_load(logo_t *lg, const char *name)
{
   /* gap centres between letters, in voxels from the content's left edge */
   static const float cut[5] = { 0.0f, 6.75f, 14.25f, 21.75f, LOGO_W_VOX };
   char path[400], rel[160];
   float fs = 1.0f, vox;
   SDL_Surface *sf = NULL;
   int i;
   lg->tried = 1;
   if (g_ui_scale > 1.2f)
   {
      snprintf(rel, sizeof(rel), "brand/%s-1080.png", name);
      asset_path(path, sizeof(path), rel, rel);
      if ((sf = IMG_Load(path))) fs = 1.5f;
   }
   if (!sf)
   {
      snprintf(rel, sizeof(rel), "brand/%s.png", name);
      asset_path(path, sizeof(path), rel, rel);
      if (!(sf = IMG_Load(path))) { logf_("brand logo not found: %s", path); return; }
   }
   if (sf->format->format != SDL_PIXELFORMAT_ABGR8888)
   {
      SDL_Surface *c = SDL_ConvertSurfaceFormat(sf, SDL_PIXELFORMAT_ABGR8888, 0);
      SDL_FreeSurface(sf);
      if (!(sf = c)) return;
   }
   SDL_SetSurfaceBlendMode(sf, SDL_BLENDMODE_NONE);
   vox = (sf->w - 2) / LOGO_W_VOX;   /* px per voxel (1 px clear border around the content) */
   lg->h = sf->h / fs;
   lg->vox = vox / fs;
   for (i = 0; i < 4; i++)
   {
      int x0 = i ? (int)floorf(1 + cut[i] * vox) : 0, x1 = i < 3 ? (int)floorf(1 + cut[i + 1] * vox) : sf->w;
      SDL_Rect r = { x0, 0, x1 - x0, sf->h };
      SDL_Surface *part = SDL_CreateRGBSurface(0, r.w, r.h, 32, 0x000000ff, 0x0000ff00, 0x00ff0000, 0xff000000);
      if (!part) continue;
      SDL_BlitSurface(sf, &r, part, NULL);
      lg->tex[i] = surface_to_tex(part, 0, NULL);
      lg->x[i] = x0 / fs; lg->w[i] = r.w / fs;
      SDL_FreeSurface(part);
   }
   logf_("brand logo: %s (%dx%d)", path, sf->w, sf->h);
   SDL_FreeSurface(sf);
}

/* top-left of a logo centred on (cx, cy) */
static void logo_origin(const logo_t *lg, float cx, float cy, float *x0, float *y0)
{
   *x0 = floorf(cx - (lg->x[3] + lg->w[3]) * 0.5f);
   *y0 = floorf(cy - lg->h * 0.5f);
}

/* dashboard: the logo centred on (cx, cy); t: frame time; a: UI alpha */
static void logo_draw(float cx, float cy, double t, float a)
{
   float x0, y0;
   int i;
   if (!g_logo.tried) logo_load(&g_logo, "logo-front");
   if (!g_logo.tex[0]) return;
   if (g_logo_t0 < 0)
   {  /* start once the dashboard has mostly faded in, or the entry happens unseen */
      if (a < 0.6f) return;
      g_logo_t0 = t;
   }
   logo_origin(&g_logo, cx, cy, &x0, &y0);
   for (i = 0; i < 4; i++)
   {
      /* 0.15 s after the dashboard shows, letter i falls 14 px in 0.55 s (a small
       * overshoot: it lands), fading in over the first 0.3 s */
      float p = clampf((float)(t - g_logo_t0 - 0.15 - 0.09 * i) / 0.55f, 0.0f, 1.0f);
      float q = p - 1.0f, e = 1.0f + 2.2f * q * q * q + 1.2f * q * q;   /* ease out back */
      float c[4] = { 1, 1, 1, a * clampf(p / 0.55f, 0.0f, 1.0f) };
      if (c[3] <= 0.003f || !g_logo.tex[i]) continue;
      draw_2d(g_logo.tex[i], x0 + g_logo.x[i], y0 - 14.0f * (1.0f - e), g_logo.w[i], g_logo.h, c, 0);
   }
}

/* splash: the big logo at the centre, filled up to `fill` (0..1 over the four letters,
 * whole voxel rows from the bottom), at alpha a. Caller: blending on, g_prog_2d bound. */
static void splash_logo_draw(float fill, float a)
{
   float x0, y0;
   int i;
   if (!g_logo_big.tried) logo_load(&g_logo_big, "logo-front-splash");
   if (!g_logo_big.tex[0] || a <= 0.003f) return;
   logo_origin(&g_logo_big, W * 0.5f, H * 0.5f, &x0, &y0);
   for (i = 0; i < 4; i++)
   {
      const float dark[4] = { SPLASH_DARK, SPLASH_DARK, SPLASH_DARK, a }, full[4] = { 1, 1, 1, a };
      float f = clampf(fill * 4.0f - i, 0.0f, 1.0f), lx = x0 + g_logo_big.x[i];
      int rows = (int)ceilf(f * LOGO_H_VOX - 0.001f);
      if (!g_logo_big.tex[i]) continue;
      if (rows < LOGO_H_VOX) draw_2d(g_logo_big.tex[i], lx, y0, g_logo_big.w[i], g_logo_big.h, dark, 0);
      if (rows <= 0) continue;
      if (rows < LOGO_H_VOX)
      {  /* the filled part: from the top of row (8 - rows) down to the bottom of the PNG */
         float top = y0 + 1.0f / (g_ui_scale > 1.2f ? 1.5f : 1.0f) + (LOGO_H_VOX - rows) * g_logo_big.vox;
         glEnable(GL_SCISSOR_TEST);
         gl_scissor(lx, top, g_logo_big.w[i], y0 + g_logo_big.h - top);
      }
      draw_2d(g_logo_big.tex[i], lx, y0, g_logo_big.w[i], g_logo_big.h, full, 0);
      glDisable(GL_SCISSOR_TEST);
   }
}

/* "Loading" at the bottom right of the startup splash, followed by three square dots
 * that light up one after another: the proof that the console is alive while startup
 * blocks. The boot payload's splash (psc-platform boot/splash/lumen-splash.c) draws the
 * same thing in the same place from pre-rendered text (assets/boot/loading-<lang>.png,
 * tools/make-boot-text.py), so the picture does not jump when Lumen takes over: keep the
 * numbers below equal to that file's. Layout px (1280x720). */
#define LOADING_RIGHT   60.0f    /* right margin of the last dot */
#define LOADING_TOP     650.0f   /* top of the text's line box */
#define LOADING_DOT     6.0f     /* dot side, and the gap between dots */
#define LOADING_GAP     10.0f    /* text to first dot */
#define LOADING_DOT_Y   14.0f    /* dot top below LOADING_TOP: the dots sit on the baseline */
#define LOADING_STEP    0.35     /* seconds per dot */
static void splash_loading_draw(double t, float a)
{
   static label_t l_loading;
   const float txt[4] = { 0.84f, 0.84f, 0.88f, 0.85f * a };
   float x = W - LOADING_RIGHT - 3 * LOADING_DOT - 2 * LOADING_DOT;
   int i, lit = (int)(t / LOADING_STEP) % 4;   /* 0: none, then one more each step */
   TTF_Font *f = g_font_sub;
   if (a <= 0.003f) return;
   if (f) label_set(&l_loading, f, tr("boot.loading"));
   if (l_loading.tex)
      draw_2d(l_loading.tex, x - LOADING_GAP - l_loading.w, LOADING_TOP, l_loading.w, l_loading.h, txt, 0);
   for (i = 0; i < 3; i++)
   {
      const float c[4] = { 0.84f, 0.84f, 0.88f, (i < lit ? 0.95f : 0.28f) * a };
      draw_2d(0, x + i * 2 * LOADING_DOT, LOADING_TOP + LOADING_DOT_Y, LOADING_DOT, LOADING_DOT, c, 0);
   }
}

/* Startup splash: show the logo filled up to `target` (loading progress), animating the
 * fill from where it is at ~2.5 logos per second (whole frames, vsync). Blocking work
 * runs between calls, so each call is one step of progress; it costs ~0.4 s in all.
 * splash_pulse() draws one more frame from inside the long blocking steps (at most ~4 a
 * second), so the dots keep moving. */
static float g_splash_fill = -1.0f;   /* < 0: no splash (in-game dialog) */
static float g_splash_a;               /* the splash fades in from black first (0.25 s) */
static SDL_Window *g_splash_win;
static double g_splash_last;           /* time of the last splash frame */
static void splash_frame(void)
{
   static int frames;
   SDL_PumpEvents();
   crt_begin(g_dw, g_dh);   /* the splash goes through the CRT filter too: no jump into the dashboard */
   glViewport(0, 0, g_dw, g_dh);
   glDisable(GL_DEPTH_TEST);
   glDisable(GL_SCISSOR_TEST);
   glClearColor(0, 0, 0, 1);
   glClear(GL_COLOR_BUFFER_BIT);
   glEnable(GL_BLEND);
   glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
   glUseProgram(g_prog_2d);
   splash_logo_draw(g_splash_fill, g_splash_a);
   splash_loading_draw(now_s(), g_splash_a);
   crt_end();
   SDL_GL_SwapWindow(g_splash_win);
   g_splash_last = now_s();
   if (++frames == 2)
   {  /* boot.sh: Lumen's own splash is on screen, the boot payload's can go */
      FILE *pf = fopen("/tmp/lumen-splash-shown", "w");
      if (pf) fclose(pf);
   }
}

static SDL_threadID g_main_thread;   /* the thread with the GL context */
static void splash_pulse(void)
{
   if (g_splash_fill < 0.0f || !g_splash_win || now_s() - g_splash_last < 0.25) return;
   if (SDL_ThreadID() != g_main_thread) return;   /* the start-up scan runs on its own thread */
   splash_frame();
}

/* One frame of the splash, its fade-in and the logo's fill moved on by the time that has
 * passed, towards `target`; it never waits. Start-up calls it between its steps and while
 * the library scan runs on its thread, so the animation plays during the loading
 * instead of each stretch of it being waited for after the step it stands for (that was
 * 0.25 s of fade-in plus 0.4 s of fill added to every start, and Lumen starts again
 * after every game). */
static void splash_tick(float target)
{
   static double last;
   const double t = now_s();
   const float dt = last > 0.0 ? clampf((float)(t - last), 0.0f, 0.1f) : 0.0f;
   if (g_splash_fill < 0.0f || !g_splash_win) return;
   if (last > 0.0 && t - g_splash_last < 0.010) return;   /* a frame was presented just now: no second wait for the display */
   last = t;
   if (g_splash_a < 1.0f) g_splash_a = fminf(1.0f, g_splash_a + dt / 0.25f);
   else if (g_splash_fill < target) g_splash_fill = fminf(target, g_splash_fill + 2.5f * dt);
   splash_frame();
}

/* the end of the start-up: what is left of the fade and the fill, in 0.15 s at most */
static void splash_finish(void)
{
   double last = now_s();
   const float rate = fmaxf(2.5f, (1.0f - g_splash_fill) / 0.15f), arate = fmaxf(4.0f, (1.0f - g_splash_a) / 0.15f);
   if (g_splash_fill < 0.0f || !g_splash_win || !g_logo_big.tex[0]) return;
   while (g_splash_a < 1.0f || g_splash_fill < 1.0f)
   {
      const double t = now_s();
      const float dt = clampf((float)(t - last), 0.0f, 0.1f);
      last = t;
      g_splash_a = fminf(1.0f, g_splash_a + arate * dt);
      g_splash_fill = fminf(1.0f, g_splash_fill + rate * dt);
      splash_frame();
   }
}

/* the audio device and the interface sounds (0.3 s on the console), on their own thread */
static int          g_audio_ok = -1;
static SDL_atomic_t g_audio_done;
static int audio_worker(void *unused)
{
   const double t0 = now_s();
   (void)unused;
   g_audio_ok = music_init();
   logf_("boot: audio (its thread) %.0f ms", (now_s() - t0) * 1000.0);
   SDL_AtomicSet(&g_audio_done, 1);
   return 0;
}

/* the library scan of the start-up (files only, no GL), on its own thread; with it, the
 * other small reads the start-up makes on the stick: the selection to resume and whether
 * there is menu music (read on the main thread they waited 527 ms on a busy stick) */
static SDL_atomic_t g_scan_done;
static double       g_scan_ms;
static int          g_resume_plat, g_resume_sel;
static int scan_worker(void *unused)
{
   const double t0 = now_s();
   (void)unused;
   scan_library();
   build_platform_index();
   g_scan_ms = (now_s() - t0) * 1000.0;
   {
      FILE *f = fopen(g_state, "r");
      if (f) { if (fscanf(f, "%d %d", &g_resume_plat, &g_resume_sel) != 2) g_resume_plat = g_resume_sel = 0; fclose(f); }
      asset_path(g_menu_music, sizeof(g_menu_music), "music/menu.ogg", "music/menu.ogg");
      if (!file_exists(g_menu_music)) g_menu_music[0] = 0;
   }
   icons_preload(g_home);
   flat_preload();   /* every system's launch declaration: read here, not when its row is first settled on */
   SDL_AtomicSet(&g_scan_done, 1);
   return 0;
}


/* ua: alpha of the whole dashboard UI; pos: selection spring (tile index space);
 * info_a / info_y: title block fade and slide after a selection change */
/* The light (lumen-design, proposal "Lumen 2"): the one saturated colour of the chrome. The
 * theme's `lit` when it fixes one, else the accent of the game's cover (computed when the
 * cover is decoded), lifted so a dark cover still gives light. g NULL: the theme's accent. */
static void lit_color(const game_t *g, float o[3])
{
   float m;
   int i;
   if (theme_lit(o)) return;
   if (!g) { theme_color(TC_ACCENT, o); return; }
   m = fmaxf(g->acc[0], fmaxf(g->acc[1], g->acc[2]));
   for (i = 0; i < 3; i++) o[i] = m > 0.02f ? 0.18f + 0.82f * g->acc[i] / m : 1.0f;
}

static void dash_draw(const platform_t *pl, int sel_plat, int n, int sel, float pos, double sel_t, double t,
                      float ua, float info_a, float info_y)
{
   static label_t l_name, l_title, l_kick, l_clock, l_empty, l_chip[3], l_pos, l_count;
   float white[4] = { 0.98f, 0.98f, 0.99f, ua };
   char s[320], a1[96], a2[96], num[24];
   int i, k;
   if (ua <= 0.003f) { g_logo_t0 = -1.0; return; }   /* hidden: the logo enters again */
   if (g_dash_profile_dirty)
   {
      g_dash_profile_dirty = 0;
      stats_load(g_home, profiles_active()->id);
      avatar_load(profiles_active());
   }

   /* the settings take the whole screen: the status row leaves with the rest of the Home
    * (their title sits where the logo is) */
   {
      const float body = ua * g_dash_body;
      if (body <= 0.003f) return;
      /* Lumen PSX logo, left on the status row's line */
      logo_draw(DASH_X + 44, TOP_Y + 22, t, body);
      ua = body; white[3] = ua;
   }

   /* status capsule, right: clock (only when the time is known), the modules' items, network,
    * controllers | avatar and profile name. One ink shape. Laid out right to left, twice:
    * pass 0 measures, pass 1 draws; the focus ring goes around it when the profile has it */
   {
      /* the game menu's sheet covers this corner and is not opaque: the capsule leaves with it */
      const float ua_all = ua, ua_ = (white[3] = ua = ua * (1.0f - clampf(gamemenu_amount() * 1.5f, 0.0f, 1.0f)));
      const profile_t *ap = profiles_active();
      const float on[4] = { 1, 1, 1, 0.86f * ua }, off[4] = { 1, 1, 1, 0.35f * ua };
      const float ch = 44, cy = TOP_Y + 22, ad = 28, gh = 22;
      float x = 0, ink[4], edge[4], sw[4], fa;
      int np = SDL_NumJoysticks(), pass;
      static spring pf;
      if (np > 4) np = 4;
      if (g_avatar_dirty || strcmp(g_avatar.id, ap->id)) { g_avatar_dirty = 0; avatar_load(ap); }
      spring_step(&pf, g_focus == FOCUS_PROFILE ? 1.0f : 0.0f, 200, 22, 1.0f / 60.0f);
      fa = clampf(pf.x, 0.0f, 1.0f);
      label_set(&l_name, g_font_name ? g_font_name : g_font_sub, profiles_display_name(ap));
      if (g_status.clock_ok) label_set(&l_clock, g_font_name ? g_font_name : g_font_sub, g_status.clock);
      theme_color(TC_CARD, ink); ink[3] = 0.86f * ua;
      theme_color(TC_GLASS, edge); edge[3] = 0.14f * ua;
      for (pass = 0; pass < 2; pass++)
      {
         if (pass)
         {
            const float xl = x - 20, cw = W - DASH_X - xl;
            draw_shape(xl, cy - ch * 0.5f, cw, ch, ch * 0.5f, 0, ink);
            draw_shape(xl, cy - ch * 0.5f, cw, ch, ch * 0.5f, 1.0f, edge);
            if (fa > 0.01f)
            {
               float fr[4];
               theme_color(TC_FOCUS, fr); fr[3] = 0.95f * fa * ua;
               draw_shape(xl - 7, cy - ch * 0.5f - 7, cw + 14, ch + 14, ch * 0.5f + 7, 3.0f, fr);
            }
         }
         x = W - DASH_X - 20 - l_name.w;
         if (pass) draw_text(&l_name, x, cy - l_name.h * 0.5f, white);
         x -= 10 + ad;
         if (pass)
         {
            if (g_avatar.tex)
            {
               const float c[4] = { 1, 1, 1, ua };
               draw_tile(g_avatar.tex, g_avatar.w, g_avatar.h, x, cy - ad * 0.5f, ad, ad, ad * 0.5f, -2.0f, 0.0f, c);
            }
            else
            {
               profiles_color(ap->color, sw); sw[3] = ua;
               draw_shape(x, cy - ad * 0.5f, ad, ad, ad * 0.5f, 0, sw);
               if (g_avatar.initials.tex)
               {  /* the initials are rendered for the 48 px avatar of the settings */
                  const float iw = g_avatar.initials.w * 0.72f, ih = g_avatar.initials.h * 0.72f;
                  draw_2d(g_avatar.initials.tex, x + (ad - iw) * 0.5f, cy - ih * 0.5f, iw, ih, white, 0);
               }
            }
         }
         x -= 16;
         if (pass) { const float dv[4] = { edge[0], edge[1], edge[2], 0.24f * ua }; draw_2d(0, floorf(x), cy - 11, 1, 22, dv, 0); }
         x -= 2;
         for (k = (np > 0 ? np : 1) - 1; k >= 0; k--)   /* no pad: one dim glyph */
         {
            x -= 14 + glyph_w("pad", gh);
            if (pass) glyph_draw("pad", x, cy - gh * 0.5f, gh, np > 0 ? on : off);
         }
         /* the plain network glyph only when no network module shows its own */
         if (g_status.net_up && !module_running("wifi") && !module_running("ethernet"))
         {
            x -= 14 + glyph_w("network", gh);
            if (pass) glyph_draw("network", x, cy - gh * 0.5f, gh, on);
         }
         {  /* the modules' items (docs/modules.md, "status") */
            const int nm = mod_status_count();
            for (k = nm - 1; k >= 0; k--)
            {
               const mod_status_item_t *it = mod_status_item(k);
               x -= 14;
               if (it->badge[0])
               {
                  static label_t l_badge[MOD_STATUS_MAX];
                  label_set(&l_badge[k], g_font_small, it->badge);
                  x -= l_badge[k].w;
                  if (pass) draw_text(&l_badge[k], x, cy - l_badge[k].h * 0.5f, it->dim ? off : on);
                  x -= 4;
               }
               x -= glyph_w(it->glyph, gh);
               if (pass) glyph_draw(it->glyph, x, cy - gh * 0.5f, gh, it->dim ? off : on);
            }
         }
         if (g_status.clock_ok)
         {
            x -= 14 + l_clock.w;
            if (pass) draw_text(&l_clock, x, cy - l_clock.h * 0.5f, white);
         }
      }      (void)ua_; ua = ua_all; white[3] = ua;
   }


   /* system tabs: L2 [names] R2. Text only; the selected one is at full strength with the
    * light's bar under it (no pills: eight shapes and eight shadows fewer per frame) */
   if (g_nplat)
   {  /* a row wider than the safe area scrolls (spring) to keep the selected pill inside,
       * with the L1 / R1 glyphs pinned at the margins */
      static spring off, tf;
      float tabf;
      const float ph = 40.0f, lh = 22.0f, padx = 0.0f, tgap = 34.0f;
      const float gl[4] = { 1, 1, 1, 0.60f * ua };
      float pws[MAX_PLATFORMS], x0, x, total = 0, sx = 0, sw = 0, l1w = 0, r1w = 0, want = 0, left, right;
      for (i = 0; i < g_nplat; i++)
      {
         platform_t *p = &g_plat[i];
         float cw = 0;
         if (p->logo_tex) cw += lh * p->logo_w / (float)p->logo_h + (p->show_name && p->label ? 9.0f : 0.0f);
         if (p->show_name && p->label) cw += p->label_w;
         if (i == sel_plat && n > 0)
         {  /* how many games the open system has: a small figure after its name */
            snprintf(num, sizeof(num), "%d", n);
            label_set(&l_count, g_font_caps, num);
            cw += 5 + l_count.w;
         }
         pws[i] = cw + 2 * padx;
         if (i == sel_plat) { sx = total; sw = pws[i]; }
         total += pws[i] + (i + 1 < g_nplat ? tgap : 0.0f);
      }
      if (g_nplat > 1) { l1w = glyph_w("l2", 24) + 22; r1w = glyph_w("r2", 24) + 22; }
      left = DASH_X + l1w; right = W - DASH_X - r1w;
      if (total > right - left)
      {
         want = off.x;
         if (sx - want < 0) want = sx;
         if (sx + sw - want > right - left) want = sx + sw - (right - left);
         want = clampf(want, 0.0f, total - (right - left));
      }
      spring_step(&off, want, 200, 22, 1.0f / 60.0f);
      spring_step(&tf, g_focus == FOCUS_TABS ? 1.0f : 0.0f, 200, 22, 1.0f / 60.0f);
      tabf = clampf(tf.x, 0.0f, 1.0f);
      x0 = left - off.x;
      if (g_nplat > 1) glyph_draw("l2", DASH_X, TAB_Y + (ph - 24) * 0.5f, 24, gl);
      if (total > right - left)
      {
         glEnable(GL_SCISSOR_TEST);
         gl_scissor(left - 20, TAB_Y - 16, right - left + 40, ph + 40);
      }
      x = x0;
      for (i = 0; i < g_nplat; i++)
      {
         platform_t *p = &g_plat[i];
         const int on_ = i == sel_plat;
         float cx, pw = pws[i];
         const float c[4] = { 1, 1, 1, (on_ ? 1.0f : 0.62f) * ua };
         if (on_)
         {  /* the light's bar; with the focus on the tabs, the ring around the name */
            float bar[4];
            lit_color(n ? &g_games[pl->first + sel] : NULL, bar); bar[3] = ua;
            draw_shape(x, TAB_Y + ph - 4, pw - (n > 0 ? 5 + l_count.w : 0), 3, 1.5f, 0, bar);
            if (tabf > 0.01f)
            {
               float fring[4];
               theme_color(TC_FOCUS, fring); fring[3] = 0.95f * tabf * ua;
               draw_shape(x - 16, TAB_Y - 3, pw + 32, ph + 6, UI_CTL_R, 3.0f, fring);
            }
         }
         cx = x + padx;
         if (p->logo_tex)
         {
            float lw = lh * p->logo_w / (float)p->logo_h;
            draw_2d(p->logo_tex, floorf(cx), floorf(TAB_Y + (ph - lh) * 0.5f), lw, lh, c, 0);
            cx += lw + 9.0f;
         }
         if (p->show_name && p->label)
         {
            draw_2d(p->label, floorf(cx), floorf(TAB_Y + (ph - p->label_h) * 0.5f), p->label_w, p->label_h, c, 0);
            cx += p->label_w;
         }
         if (on_ && n > 0)
         {
            const float cc[4] = { 0.87f, 0.89f, 0.93f, 0.66f * ua };
            draw_text(&l_count, floorf(cx + 5), floorf(TAB_Y + 6), cc);
         }
         x += pw + tgap;
      }
      glDisable(GL_SCISSOR_TEST);
      if (g_nplat > 1)
         glyph_draw("r2", total > right - left ? W - DASH_X - glyph_w("r2", 24) : x + 2, TAB_Y + (ph - 24) * 0.5f, 24, gl);
   }

   /* the shelf: covers stand on one line (SHELF_B); the focused one is larger and lit by its
    * own game; covers left of the selection fade away */
   if (n)
   {
      static spring gf = { 1.0f, 0.0f };
      int base = (int)floorf(pos);
      float st = (float)(t - sel_t), sheen = -2.0f, gamef;
      spring_step(&gf, g_focus == FOCUS_GAMES ? 1.0f : 0.0f, 200, 22, 1.0f / 60.0f);
      gamef = clampf(gf.x, 0.0f, 1.0f);
      if (st > 0.35f && g_focus == FOCUS_GAMES)
      {  /* a light sweep over the selected tile shortly after it settles, then every 6 s */
         float u = fmodf(st - 0.35f, 6.0f);
         if (u < 1.0f) sheen = -0.6f + u * 2.4f;
      }
      for (k = -2; k <= 12; k++)
      {
         int idx = base + k;
         float d, x, sz, sw, ta, ty, kb;
         game_t *g;
         if (idx < 0 || idx >= n) continue;
         if (!g_fx_sides && idx != sel) continue;
         d = idx - pos; x = tile_x(d, pl->tile_ar); sz = tile_size(d); sw = sz * pl->tile_ar;   /* sw x sz: the box */
         ty = SHELF_B - sz;
         kb = lerpf(1.0f, 0.72f, clampf(fabsf(d), 0.0f, 1.0f));   /* at rest: darker, never translucent */
         ta = ua * (d < 0.0f ? clampf(1.0f + d * 1.8f, 0.0f, 1.0f) : 1.0f);
         if (ta <= 0.003f || x > W || x + sw < 0) continue;
         g = &g_games[pl->first + idx];
         /* only the focused tile casts a shadow: the others sit on dimmed art where it
          * barely shows, and seven blurred quads cost ~5 ms a frame on the console */
         if (idx == sel && g_fx_shadows)
         {
            const float sc_[4] = { 0, 0, 0, 0.60f * ta };
            float b = 22.0f * sz / 220.0f;
            draw_2d(g_tile_shadow, x - b, ty - b + 10, sw + 2 * b, sz + 2 * b, sc_, 0);
            {  /* the glow: the tile's shape with a wide falloff, in the game's light (one quad) */
               const float gb = GLOW_BLUR * sz / 220.0f;
               float gl_[4];
               lit_color(g, gl_); gl_[3] = 0.62f * ta * gamef;
               draw_2d(g_tile_glow, x - gb, ty - gb, sw + 2 * gb, sz + 2 * gb, gl_, 0);
            }
         }
         if (g->tex)
         {
            const float c[4] = { kb, kb, kb, ta };
            draw_tile(g->tex, g->tex_w, g->tex_h, x, ty, sw, sz, 14, idx == sel ? sheen : -2.0f, idx == sel ? 1.0f : 0.0f, c);
         }
         else
         {
            const float c[4] = { 0.16f, 0.16f, 0.20f, 0.7f * ta };
            draw_shape(x, ty, sw, sz, 14, 0, c);   /* (its shadow above is not cut: barely visible) */
         }
         if (g->port)
         {  /* a native port of the game: a chip in the cover's top left corner, on every tile */
            static label_t l_port;
            const float k = sz / TILE_SEL, chip[4] = { 0.04f, 0.04f, 0.06f, 0.78f * ta }, edge[4] = { 1, 1, 1, 0.22f * ta }, tc[4] = { 1, 1, 1, 0.95f * ta };
            float lw, lh, cw, ch;
            label_set(&l_port, g_font_caps, tr("badge.port"));
            lw = l_port.w * 0.86f * k; lh = l_port.h * 0.86f * k; cw = lw + 24.0f * k; ch = 26.0f * k;
            draw_shape(x + 10 * k, ty + 10 * k, cw, ch, ch * 0.5f, 0, chip);
            draw_shape(x + 10 * k, ty + 10 * k, cw, ch, ch * 0.5f, 1.0f, edge);
            draw_2d(l_port.tex, x + 10 * k + (cw - lw) * 0.5f, ty + 10 * k + (ch - lh) * 0.5f, lw, lh, tc, 0);
         }
         if (idx == sel)
         {  /* focus ring (fades in with the title) and the media badge */
            const float badge[4] = { 0.04f, 0.04f, 0.06f, 0.70f * ta };
            float ring[4];
            const float bedge[4] = { 1, 1, 1, 0.22f * ta };
            const char *mg = pl->kind == KIND_CART ? "cartridge" : (pl->kind == KIND_ARCADE ? "coin" : "disc");
            const float gc[4] = { 1, 1, 1, 0.95f * ta };
            theme_color(TC_FOCUS, ring); ring[3] = 0.95f * ta * info_a * gamef;
            draw_shape(x - 7, ty - 7, sw + 14, sz + 14, 20, 3.0f, ring);
            draw_shape(x + sw - 50, ty + sz - 50, 40, 40, 20, 0, badge);
            draw_shape(x + sw - 50, ty + sz - 50, 40, 40, 20, 1.0f, bedge);
            glyph_draw(mg, x + sw - 43, ty + sz - 43, 26, gc);
         }
      }
   }

   /* the hero block: the game said once and large, with its facts as chips */
   if (n)
   {
      const game_t *cg = &g_games[pl->first + sel];
      const game_stats_t *gs = stats_get(cg->content);
      const float ia = info_a * ua;
      const float c1[4] = { 0.99f, 0.99f, 1.0f, ia }, ctx[4] = { 0.93f, 0.94f, 0.96f, 0.92f * ia }, cgl[4] = { 1, 1, 1, 0.62f * ia };
      static char cb[3][96];
      const char *cgly[3];
      float lit[4], ink[4], edge[4], sc, x, y = INFO_Y + info_y;
      int nc = 0;
      lit_color(cg, lit); lit[3] = ia;
      theme_color(TC_CARD, ink); ink[3] = 0.86f * ia;
      theme_color(TC_GLASS, edge); edge[3] = 0.14f * ia;
      /* kicker: the light's bar, then system, year, publisher */
      draw_shape(DASH_X, y + 7, 28, 3, 1.5f, 0, lit);
      snprintf(s, sizeof(s), "%s", pl->name);
      if (cg->year[0]) { size_t l = strlen(s); snprintf(s + l, sizeof(s) - l, " \xC2\xB7 %s", cg->year); }
      if (cg->publisher[0]) { size_t l = strlen(s); snprintf(s + l, sizeof(s) - l, " \xC2\xB7 %s", cg->publisher); }
      ui_kicker(&l_kick, s, DASH_X + 40, y, ia);
      y += 24;
      label_set(&l_title, g_font_hero, cg->title);
      sc = l_title.w > W - 2 * DASH_X ? (W - 2 * DASH_X) / (float)l_title.w : 1.0f;
      if (sc < 1.0f) draw_2d(l_title.tex, DASH_X - 2, y, l_title.w * sc, l_title.h * sc, c1, 0);
      else draw_text(&l_title, DASH_X - 2, y, c1);
      y += l_title.h * sc + 10 + info_y * 0.4f;
      /* chips: players, time played, last played (or "not played yet") */
      if (cg->players[0])
      { trf(cb[nc], sizeof(cb[0]), strcmp(cg->players, "1") ? "card.players" : "card.players_one", "n", cg->players, NULL); cgly[nc++] = "players"; }
      if (gs && gs->seconds > 0)
      {
         snprintf(cb[nc], sizeof(cb[0]), "%s", stats_format_duration(a1, sizeof(a1), gs->seconds)); cgly[nc++] = "clock";
         if (gs->last_epoch > 0) { snprintf(cb[nc], sizeof(cb[0]), "%s", stats_format_date(a2, sizeof(a2), gs->last_epoch)); cgly[nc++] = "calendar"; }
      }
      else { snprintf(cb[nc], sizeof(cb[0]), "%s", tr("dash.never")); cgly[nc++] = "clock"; }
      x = DASH_X;
      for (i = 0; i < nc; i++)
      {
         float cw;
         label_set(&l_chip[i], g_font_small, cb[i]);
         cw = 12 + 18 + 8 + l_chip[i].w + 15;
         draw_shape(x, y, cw, 34, 17, 0, ink);
         draw_shape(x, y, cw, 34, 17, 1.0f, edge);
         glyph_draw(cgly[i], x + 12, y + 8, 18, cgl);
         draw_text(&l_chip[i], x + 38, y + (34 - l_chip[i].h) * 0.5f, ctx);
         x += cw + 10;
      }
      if (coll_is_fav(cg->content))
      {  /* a favourite: the star, alone in its chip */
         const float star[4] = { 0.95f, 0.76f, 0.0f, ia };
         draw_shape(x, y, 42, 34, 17, 0, ink);
         draw_shape(x, y, 42, 34, 17, 1.0f, edge);
         glyph_draw("star", x + 12, y + 8, 18, star);
      }
      if (n > 1)
      {  /* where we are in the list: bottom left, on the help bar's line */
         /* it leaves while the game menu is open: the help bar is centred left of the sheet */
         const float pa = ua * (1.0f - clampf(gamemenu_amount() * 2.0f, 0.0f, 1.0f));
         const float tr_[4] = { 1, 1, 1, 0.14f * pa }, th[4] = { 1, 1, 1, 0.92f * pa }, pc[4] = { 0.87f, 0.89f, 0.93f, 0.66f * pa };
         const float py = 672, tw = 120;
         float px;
         snprintf(num, sizeof(num), "%d / %d", sel + 1, n);
         label_set(&l_pos, g_font_caps, num);
         draw_text(&l_pos, DASH_X, floorf(py - l_pos.h * 0.5f), pc);
         px = DASH_X + l_pos.w + 12;
         draw_shape(px, py - 1.5f, tw, 3, 1.5f, 0, tr_);
         draw_shape(px, py - 1.5f, fmaxf(6.0f, tw * (sel + 1) / (float)n), 3, 1.5f, 0, th);
      }
   }
   else
   {
      label_set(&l_empty, g_font_hero, tr("empty.library"));
      draw_text(&l_empty, DASH_X, INFO_Y + 24, white);
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
   {
      char pp[300];
      snprintf(pp, sizeof(pp), "%s/debug/perf", g_home);
      g_perf_on = g_cost_on || file_exists(pp);
   }
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

static void rec_start(void)
{
   const char *e = getenv("LUMEN_RECORD");
   if (!e || !*e) return;
   if (!(g_rec = fopen(e, "wb"))) { logf_("record: cannot open %s", e); return; }
   if ((e = getenv("LUMEN_RECORD_FPS")) && atof(e) > 0) g_rec_fps = atof(e);
   if ((e = getenv("LUMEN_RECORD_SECONDS"))) g_rec_secs = atof(e);
   g_rec_base = real_now_s(); g_rec_on = 1;
   SDL_GL_SetSwapInterval(0);   /* virtual time: never wait for a vblank */
   logf_("record: %dx%d RGBA at %.0f fps, %.1f s", g_dw, g_dh, g_rec_fps, g_rec_secs);
}

/* the finished back buffer, before the swap; returns 0 once the recording is complete */
static int rec_frame(void)
{
   static unsigned char *px;
   size_t n = (size_t)g_dw * g_dh * 4;
   if (!g_rec_on) return 1;
   if (!px && !(px = malloc(n))) return 1;
   glReadPixels(0, 0, g_dw, g_dh, GL_RGBA, GL_UNSIGNED_BYTE, px);
   if (fwrite(px, 1, n, g_rec) != n) { logf_("record: write failed at frame %u", g_rec_frames); return 0; }
   g_rec_frames++;
   if (g_rec_secs > 0 && g_rec_frames >= g_rec_secs * g_rec_fps)
   { fflush(g_rec); logf_("record: done, %u frames", g_rec_frames); return 0; }
   return 1;
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
 * 0 continue, 2 exit the game, 3 save (resume slot) and exit, 10 + k switch to disc k
 * (1-based). Circle = continue. */
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
   label_t kicker = { 0 }, title = { 0 }, sub = { 0 }, row_l[MAXROW], val = { 0 }, h_ok = { 0 }, h_back = { 0 };
   int ans[MAXROW], disc_row = -1, n = 0, sel = 0, answer = -1, i, discs = 1, cur = 1, pick = 1, shown = 0;
   char gtitle[256], cover_path[700], buf[64];
   GLuint cover = 0; int cw_ = 0, chh = 0;
   const icon_t *ic_ok = icon_get(g_home, "cross"), *ic_back = icon_get(g_home, "circle");
   spring pill = { 0 }, fade = { 0 };
   double t0 = now_s(), tp = t0;

   if (!strncmp(kind, "discs:", 6) && sscanf(kind + 6, "%d:%d", &discs, &cur) >= 1)
   { if (discs < 1) discs = 1; if (discs > 99) discs = 99; if (cur < 1 || cur > discs) cur = 1; }
   memset(row_l, 0, sizeof(row_l));
   /* rows: Continue, [Change disc], Save and exit, Exit. The discs are one row (lumen-2 mockup,
    * "In-game pause"): its value is the disc to put in, Left / Right pick it, Cross switches;
    * it starts on the next disc, the one a player is most likely to want */
   label_set(&row_l[n], g_font_name, tr("dialog.continue")); ans[n++] = 0;
   if (discs > 1)
   {
      disc_row = n; pick = cur % discs + 1;
      label_set(&row_l[n], g_font_name, tr("dialog.change_disc")); ans[n++] = 10;
   }
   label_set(&row_l[n], g_font_name, tr("dialog.save_exit")); ans[n++] = 3;
   label_set(&row_l[n], g_font_name, tr("dialog.exit")); ans[n++] = 2;

   dialog_launch_info(gtitle, sizeof(gtitle), cover_path, sizeof(cover_path));
   if (cover_path[0])
   {
      SDL_Surface *sf = IMG_Load(cover_path);
      if (sf) { cw_ = sf->w; chh = sf->h; cover = surface_to_tex(sf, 1, NULL); SDL_FreeSurface(sf); }
   }
   label_set(&title, g_font_title, gtitle[0] ? gtitle : tr("dialog.title"));
   if (discs > 1)
   {  /* which disc is in: "Disc 1 of 2" under the title */
      char n1[12], n2[12], sb[96];
      snprintf(n1, sizeof(n1), "%d", cur); snprintf(n2, sizeof(n2), "%d", discs);
      label_set(&sub, g_font_small, trf(sb, sizeof(sb), "dialog.disc_of", "n", n1, "m", n2, NULL));
   }
   label_set(&h_ok, g_font_small, tr("dialog.choose"));
   label_set(&h_back, g_font_small, tr("dialog.back"));
   logf_("dialog %s (discs %d, current %d, cover %s)", kind, discs, cur, cover ? "yes" : "no");

   while (answer < 0)
   {
      SDL_Event e;
      ui_nav_t nv;
      double t = now_s();
      float dt = clampf((float)(t - tp), 0.0f, 0.05f); tp = t;   /* a slow frame must not blow up the springs */
      memset(in_pressed, 0, sizeof(in_pressed));
      while (SDL_PollEvent(&e)) { if (e.type == SDL_QUIT) answer = 0; else in_event(&e); }
      nav_fill(&nv, t);
      if (t - t0 > 0.25)   /* ignore the press still held from the game */
      {
         if (nv.up && sel > 0) sel--;
         if (nv.down && sel < n - 1) sel++;
         if (sel == disc_row && nv.left) pick = pick > 1 ? pick - 1 : discs;
         if (sel == disc_row && nv.right) pick = pick < discs ? pick + 1 : 1;
         if (nv.confirm) answer = sel == disc_row ? (pick == cur ? 0 : 10 + pick) : ans[sel];
         else if (nv.back || nv.options) answer = 0;
      }
      spring_step(&fade, 1.0f, 60, 14, dt);
      if (disc_row >= 0 && shown != pick)
      {  /* the row's value: the disc it would put in */
         snprintf(buf, sizeof(buf), "%s %d", tr("dialog.disc"), pick);
         label_set(&val, g_font_small, buf); shown = pick;
      }
      {
         /* geometry: header 150, rows 66 */
         const float RW = 66.0f, BH_ = RW - 14, cwid = 480, head = 150, pad = 32;   /* a button and its ring fill the pitch */
         float chgt = head + n * RW + pad, cx = (W - cwid) * 0.5f, cy = (H - chgt) * 0.5f - 18;
         float a = clampf(fade.x, 0, 1), rowy[MAXROW], y = cy + head;
         float white[4] = { 1, 1, 1, a }, txt[4], dim[4], scrim[4], red[4];
         theme_color(TC_TEXT, txt); txt[3] = a;
         theme_color(TC_DIM, dim); dim[3] = a;
         theme_color(TC_BACKDROP, scrim); scrim[3] = 0.86f;
         theme_color(TC_DANGER, red); red[3] = a;
         for (i = 0; i < n; i++) { rowy[i] = y; y += RW; }
         if (t - t0 < 0.02) pill.x = rowy[sel];
         spring_step(&pill, rowy[sel], 260, 26, dt);

         crt_begin(g_dw, g_dh);
         glViewport(0, 0, g_dw, g_dh);
         glClearColor(scrim[0], scrim[1], scrim[2], 1.0f);
         glClear(GL_COLOR_BUFFER_BIT);
         glDisable(GL_DEPTH_TEST);
         glEnable(GL_BLEND);
         glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
         gl_use_2d();
         if (cover && ui_scrim_alpha() < 1.0f)   /* backdrop: the cover filling the screen, dimmed (flat black in Lumen's own look) */
         {
            float s2 = fmaxf((float)W / cw_, (float)H / chh), bw = cw_ * s2, bh = chh * s2;
            const float bc[4] = { 1, 1, 1, 0.55f };
            draw_2d(cover, (W - bw) * 0.5f, (H - bh) * 0.5f, bw, bh, bc, 0);
         }
         draw_2d(0, 0, 0, W, H, scrim, 0);
         ui_card(cx, cy, cwid, chgt, a);   /* the same glass card as every menu (ui.h) */
         /* header: cover thumbnail, kicker, title */
         {
            float tx = cx + pad;
            if (cover)
            {
               float th = 90, tw = th * cw_ / (float)chh;
               if (tw > 90) { tw = 90; th = tw * chh / (float)cw_; }
               draw_shadow(cx + pad, cy + pad, tw, th, 10, 10, 3, 0.45f * a);
               draw_tile(cover, cw_, chh, cx + pad, cy + pad, tw, th, 8, -2.0f, 0.0f, white);
               tx += tw + 20;
            }
            if (gtitle[0]) ui_kicker(&kicker, tr("dialog.title"), tx, cy + pad + 8, a);   /* no game title: the title says it */
            if (title.tex)
            {
               float maxw = cx + cwid - pad - tx, tw2 = title.w > maxw ? maxw : title.w;
               draw_2d(title.tex, tx, cy + pad + 8 + kicker.h + 4, tw2, title.h, txt, 0);
            }
            if (sub.tex) draw_2d(sub.tex, tx, cy + pad + 8 + kicker.h + 4 + title.h + 2, sub.w, sub.h, dim, 0);
         }
         /* the actions: buttons with their glyph, the first one filled; the focus is glass and
          * the ring behind the button (ui.h), the destructive answer red text, never a block */
         ui_focus(cx + pad, pill.x + 7, cwid - 2 * pad, BH_, UI_CTL_R, a);
         for (i = 0; i < n; i++)
         {
            const int primary = i == 0;
            const char *gly = primary ? "play" : i == disc_row ? "disc" : ans[i] == 3 ? "save" : ans[i] == 2 ? "exit" : NULL;
            const float by = rowy[i] + 7, lx = cx + pad + 18;
            float c[4], gc[4];
            if (primary)
            {
               float pc[4];
               theme_color(TC_PRIMARY, pc);
               ui_fill(cx + pad, by, cwid - 2 * pad, BH_, UI_CTL_R, pc, a);
               theme_color(TC_ON_PRIMARY, c); c[3] = a;
            }
            else
            {
               ui_rest(cx + pad, by, cwid - 2 * pad, BH_, UI_CTL_R, a);
               memcpy(c, ans[i] == 2 ? red : txt, sizeof(c));
            }
            memcpy(gc, c, sizeof(gc)); if (!primary && ans[i] != 2) gc[3] *= 0.70f;
            if (gly) glyph_draw(gly, lx, by + (BH_ - 24) * 0.5f, 24, gc);
            if (row_l[i].tex) draw_2d(row_l[i].tex, lx + 24 + 14, by + (BH_ - row_l[i].h) * 0.5f, row_l[i].w, row_l[i].h, c, 0);
            if (i == disc_row)
            {  /* value and chevron at the right edge, as a row that changes a value */
               const float xr = cx + cwid - pad - 18 - ui_chevron(cx + cwid - pad - 18, by + BH_ * 0.5f, a) - 8;
               if (val.tex) draw_2d(val.tex, xr - val.w, by + (BH_ - val.h) * 0.5f, val.w, val.h, dim, 0);
            }
         }
         /* help bar */
         {
            float ih = 26, hy = H - 58, wok = (ic_ok && ic_ok->tex ? ih + 8 : 0) + h_ok.w, wbk = (ic_back && ic_back->tex ? ih + 8 : 0) + h_back.w;
            float hx = (W - (wok + 36 + wbk)) * 0.5f;
            if (ic_ok && ic_ok->tex) { draw_2d(ic_ok->tex, hx, hy, ih, ih, txt, 0); hx += ih + 8; }
            if (h_ok.tex) { draw_2d(h_ok.tex, hx, hy + (ih - h_ok.h) * 0.5f, h_ok.w, h_ok.h, dim, 0); hx += h_ok.w + 36; }
            if (ic_back && ic_back->tex) { draw_2d(ic_back->tex, hx, hy, ih, ih, txt, 0); hx += ih + 8; }
            if (h_back.tex) draw_2d(h_back.tex, hx, hy + (ih - h_back.h) * 0.5f, h_back.w, h_back.h, dim, 0);
         }
         crt_end();
      }
      if (!rec_frame()) answer = 0;
      SDL_GL_SwapWindow(win); g_frame_id++;
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
   SDL_Thread *scan_th = NULL, *audio_th = NULL;   /* the start-up library scan, the audio device */
   int win_w = W, win_h = H;
   double b_draw = 0, b_swap = 0;   /* bench: time issuing frames / blocked in the swap */
   SDL_GameController *pad = NULL;
   double t0 = now_s(), last, fps_t, info_t = 0, play_t = -1;
   float play_pt = 0.0f;   /* launch clock: the sum of the frame times since Play, each clamped to
                            * 100 ms, so a stall never skips part of the animation (it was
                            * t - play_t: a 560 ms stall on the first frame jumped 0.6 s ahead)
                            * and anything from 10 fps up plays in real time (clamped to 50 ms
                            * like the springs, it ran slow under 20 fps) */
   float launch_dt = 0.0f, lt_sum = 0.0f, lt_max = 0.0f;   /* launch frame times (real, unclamped) */
   int lt_n = 0, lt_slow = 0;
   int i, sel_plat = 0, sel = 0, running = 1, exit_code = 0, frames = 0, crt_on = 0;
   int held_dir = 0, nav_dir = 1; double held_next = 0, sel_t = 0;
   int start_down = 0, select_down = 0;
   spring bg[3], info_y, info_a, plat_x, dpos;
   label_t l_start = { 0 };
   float fade = 1.0f; /* 1 = black, fades in at start */

   /* "lumen --audio-sink <device> <config> <rate>": the child that plays the dashboard's
    * sound on a named ALSA device (music.c). Before anything else: it is not a dashboard. */
   if (argc >= 5 && !strcmp(argv[1], "--audio-sink")) return music_sink_main(argv[2], argv[3], atoi(argv[4]));
   /* A DualSense / DualShock 4 on Bluetooth stays in its simple report mode: SDL's "enhanced
    * reports" (rumble, LED) switch the pad to a report only SDL reads, and the console's
    * kernel then gives RetroArch and the PS button (lumen-buttons) no input at all until the
    * pad reconnects. The environment, not SDL_SetHint: it reaches SDL 3 under sdl2-compat. */
   setenv("SDL_JOYSTICK_ENHANCED_REPORTS", "0", 0);

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
   log_start_writer();
   atexit(log_sync_atexit);
   logf_("lumen %s start", LUMEN_VERSION);
   boot_mark("");
   g_main_thread = SDL_ThreadID();

   if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK) != 0)
   { logf_("SDL_Init: %s", SDL_GetError()); return 1; }
   logf_("controller mappings: %d from %s", SDL_GameControllerAddMappingsFromFile(padb), padb);
   SDL_JoystickEventState(SDL_ENABLE);
   SDL_GameControllerEventState(SDL_ENABLE);
   logf_("joysticks at start: %d", SDL_NumJoysticks());
   for (i = 0; i < SDL_NumJoysticks(); i++) in_open(i);
   IMG_Init(IMG_INIT_PNG | IMG_INIT_JPG);
   TTF_Init();
   if (settings_load(g_home) < 0)
      settings_set_int("language_asked", 0);   /* new install: the welcome asks for the language */
   theme_init(g_home);
   g_rk = theme_radius();
   {  /* a theme may bring its own fonts (themes/<id>/fonts/<weight>.ttf) */
      theme_asset(font_b, sizeof(font_b), "fonts/bold.ttf");
      theme_asset(font_r, sizeof(font_r), "fonts/regular.ttf");
      theme_asset(font_m, sizeof(font_m), "fonts/medium.ttf");
      theme_asset(font_sb, sizeof(font_sb), "fonts/semibold.ttf");
   }
   i18n_init(g_home);
   i18n_set_language(settings_get("language", "en"));
   profiles_load(g_home);
   coll_load(g_home, profiles_active()->id);
   boot_mark("sdl+config");

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
      /* no multisampling: shapes are antialiased by their shaders, text, art and the launch's
       * illustrations are textures */
      SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 0);
      SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 0);
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
   boot_mark("window+gl");
   gl_setup();
   crt_prepare(g_dw, g_dh);   /* CRT filter on: its shader and target now, not on the first frame */
   logf_("shaders: %d from the cache, %d compiled", g_progs_cached, g_progs_built);
   boot_mark("shaders");
   g_ortho = m4_ortho(0, W, H, 0);
   if (!dialog)
   {  /* fade in, letters dark. Right after a console boot the boot payload's splash is
       * showing this very frame (boot.sh leaves /tmp/lumen-boot-splash): no fade then */
      g_font_sub = open_font(fonts_reg, 21);
      g_splash_fill = 0.0f;
      if (remove("/tmp/lumen-boot-splash") == 0) g_splash_a = 1.0f;
      g_splash_win = win;
      splash_tick(0.0f);
      /* the library is read from the stick while the rest of the start-up goes on here */
      scan_th = SDL_CreateThread(scan_worker, "scan", NULL);
   }
   make_tile_shadow();
   make_tile_glow();
   bg_mx = SDL_CreateMutex();
   bg_cv = SDL_CreateCond();
   SDL_CreateThread(bg_worker, "bg", NULL);
   cv_mx = SDL_CreateMutex();
   cv_cv = SDL_CreateCond();
   SDL_CreateThread(cover_worker, "covers", NULL);

   g_font_title = open_font(fonts_semi, 34);
   if (!g_font_sub) g_font_sub = open_font(fonts_reg, 21);
   g_font_small = open_font(fonts_reg, 17);
   splash_tick(0.1f);
   g_font_cover = open_font(fonts_bold, 22);
   g_font_hero  = open_font(fonts_semi, 68);
   g_font_tab   = open_font(fonts_med, 19);
   splash_tick(0.2f);
   g_font_name  = open_font(fonts_med, 21);
   g_font_medium = g_font_name ? g_font_name : g_font_sub;
   g_font_caps  = open_font(fonts_med, 15);
   boot_mark("fx+fonts");
   splash_tick(0.2f);
   if (dialog)   /* in-game dialog: no library, no music */
   {
      g_ortho = m4_ortho(0, W, H, 0);
      return run_dialog(win, dialog);
   }

   /* what needs no library, while the scan runs: the audio device, the help bar */
   g_ortho = m4_ortho(0, W, H, 0);
   audio_th = SDL_CreateThread(audio_worker, "audio", NULL);
   if (!audio_th && music_init() == 0) ui_sound_changed();
   splash_tick(0.3f);
   while (scan_th && !SDL_AtomicGet(&g_scan_done)) splash_tick(0.6f);   /* the splash plays on, a frame at a time */
   if (scan_th) SDL_WaitThread(scan_th, NULL);
   else { scan_library(); build_platform_index(); }
   hints_load(g_home);   /* its icons were read by the scan thread */
   boot_mark("hints");
   logf_("games: %d, platforms: %d, scan %.0f ms on its thread, done %.0f ms after the start", g_ngames, g_nplat, g_scan_ms, (now_s() - t0) * 1000);
   boot_mark("scan wait");
   platforms_load_art();
   coll_rows_build();
   boot_mark("tab art");
   splash_tick(0.8f);

   /* resume last selection */
   {
      sel_plat = g_resume_plat; sel = g_resume_sel;   /* read by the scan thread */
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

   g_ortho = m4_ortho(0, W, H, 0);

   memset(bg, 0, sizeof(bg));
   bg[0].x = 0.05f; bg[1].x = 0.05f; bg[2].x = 0.08f;
   memset(&info_y, 0, sizeof(info_y)); memset(&info_a, 0, sizeof(info_a)); memset(&plat_x, 0, sizeof(plat_x));
   memset(&dpos, 0, sizeof(dpos)); dpos.x = (float)sel;
   boot_mark("resume");
   panel_init();
   /* modules first (docs/modules.md): the wifi, bluetooth and ethernet modules tell net_init
    * which radios it may start */
   modules_init(g_home);
   net_init();   /* after settings: Wi-Fi / Bluetooth / USB Ethernet as they were left */
   update_init(); /* after settings + net: a staged update, or the leftovers of a download */
   kern_init();  /* which kernel runs (Settings, About) */
   boot_mark("panel");
   transfer_init();
   boot_mark("transfer");
   welcome_init();
   attract_init(start_attract, g_home);
   boot_mark("welcome");
   if (audio_th)
   {  /* the splash plays on while the audio device finishes opening */
      while (!SDL_AtomicGet(&g_audio_done)) splash_tick(0.9f);
      SDL_WaitThread(audio_th, NULL);
      if (g_audio_ok == 0) ui_sound_changed();
   }
   boot_mark("audio wait");
   memset(&att, 0, sizeof(att));

   splash_finish();
   {
      char sp[300];
      snprintf(sp, sizeof(sp), "%s/debug/launch-shots", g_home);
      if ((g_launch_shots = file_exists(sp)))
      {
         snprintf(sp, sizeof(sp), "%s/debug/shots", g_home);
         mkdir(sp, 0755);
         logf_("launch shots: on (%s)", sp);
      }
   }
   last = fps_t = now_s();
   boot_mark("last splash");
   logf_("startup %.0f ms, rss %ld kB", (last - t0) * 1000, rss_kb());
   test_parse();
   if (g_perf_on) stuck_start();
   g_scrim_fold = settings_get_int("scrim_fold", 1, 0, 1);
   g_card_merge = settings_get_int("card_merge", 1, 0, 1);
   g_cover_mip = settings_get_int("cover_trilinear", 0, 0, 1) ? 1 : 2;
   g_bg_mesh = settings_get_int("bg_mesh", 1, 0, 1);
   rec_start();

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
      if (g_coll_built != coll_generation() && play_t < 0 && !gamemenu_open_now() && !panel_active())
      {
         coll_rows_rebuild(&sel_plat, &sel);
         sel_t = t; info_y.x = 18.0f; info_a.x = 0.0f; bg_shown[0] = 0;
      }
      if (sel_plat >= g_nplat) { sel_plat = 0; sel = 0; }
      pl = g_nplat ? &g_plat[sel_plat] : NULL;
      if (pl && sel_plat >= g_nreal_plat && pl->count) coll_row_follow(pl, pl->first + (sel < pl->count ? sel : 0));
      int n = pl ? pl->count : 0, move = 0, plat_move = 0, page = 0, play = 0, att_swallow, restore = -1, want_menu = 0;
      ui_nav_t fnav;   /* one navigation frame shared by the panel and the game menu (auto-repeat state) */
      int fp_open;     /* the file picker was open when this frame began */
      int open_profile = 0;   /* Cross on the focused profile */
      launch_dt = dt > 0.10f ? 0.10f : dt;   /* a stall (>= 100 ms) still does not skip the animation */
      if (play_t >= 0 && t > play_t)
      {  /* how the launch ran on this machine: one line in the log when it ends */
         lt_n++; lt_sum += dt;
         if (dt > lt_max) lt_max = dt;
         if (dt > 0.025f) lt_slow++;
      }
      if (dt > 0.05f) dt = 0.05f;
      last = t;

      memset(in_pressed, 0, sizeof(in_pressed));
      while (SDL_PollEvent(&e))
      {
         if (e.type == SDL_QUIT) running = 0;
         else in_event(&e);
      }
      if (g_home_pump > 0)
      {  /* PS / Home: one Back every 8 frames until nothing is open (a keyboard, a question,
          * a sub-page each take one) */
         static int gap;
         if (!panel_active() && !gamemenu_active() && !filepicker_active() && !transfer_active() && !audit_active() && !conn_active() && !kern_active()) g_home_pump = 0;
         else if (gap > 0) gap--;
         else { in_pressed[IN_BACK] = 1; g_home_pump--; gap = 8; }
      }
      if (g_ntin && !test_input(g_rec_on ? g_rec_frames / g_rec_fps : t - t0)) running = 0;   /* recording: from its first frame */
      if (g_want_reboot) { exit_code = 12; running = 0; }
      if (g_want_update) { exit_code = 13; running = 0; }
      if (g_want_restart) { exit_code = 0; running = 0; }
      {  /* screensaver / attract scene: any input wakes it and is swallowed (10.2) */
         int was = attract_active();
         if (in_last_activity != last_activity) { last_activity = in_last_activity; attract_note_input(t); }
         attract_update(t, dt, !panel_active() && !filepicker_active() && !transfer_active() && !audit_active() && !conn_active() && !kern_active() && !welcome_active() && !padwait_open_now() && play_t < 0 && fade < 0.02f && g_ngames > 0, &att);
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
         /* Cross plays; Start alone opens the game menu (favourite, folders) */
         if (in_pressed[IN_PLAY]) play = 1;
         if (in_pressed[IN_START] && !in_down[IN_SELECT]) want_menu = 1;
         /* L1 / R1: previous / next page of games; L2 / R2: system (Up / Down move the focus, below) */
         if (in_pressed[IN_L1]) page = -1;
         if (in_pressed[IN_R1]) page = 1;
         if (in_pressed[IN_L2]) plat_move = -1;
         if (in_pressed[IN_R2]) plat_move = 1;
         if (in_pressed[IN_LEFT]) { move = -1; held_next = t + 0.32; }
         if (in_pressed[IN_RIGHT]) { move = 1; held_next = t + 0.32; }
      }
      held_dir = in_down[IN_LEFT] ? -1 : (in_down[IN_RIGHT] ? 1 : 0);
      {  /* focus: games <-> system tabs <-> profile with Up / Down. Away from the games,
          * Left / Right change the system (tabs), Cross goes back to the games (tabs) or
          * opens the profile settings (profile), Circle returns to the games */
         const int free_ = !panel_active() && !filepicker_active() && !transfer_active() && !audit_active() && !conn_active() && !kern_active() && !gamemenu_active() && !welcome_active() &&
                           !att_swallow && play_t < 0;
         open_profile = 0;
         if (attract_active() || play_t >= 0 || !g_nplat) g_focus = FOCUS_GAMES;
         else if (free_)
         {
            if (in_pressed[IN_UP] && g_focus < FOCUS_PROFILE) { g_focus++; music_sfx(SFX_FOCUS); }
            else if (in_pressed[IN_DOWN] && g_focus > FOCUS_GAMES) { g_focus--; music_sfx(SFX_FOCUS); }
            else if (g_focus != FOCUS_GAMES)
            {
               if (in_pressed[IN_BACK]) { g_focus = FOCUS_GAMES; music_sfx(SFX_BACK); }
               else if (g_focus == FOCUS_TABS && in_pressed[IN_PLAY]) { g_focus = FOCUS_GAMES; music_sfx(SFX_CONFIRM); }
               else if (g_focus == FOCUS_TABS && move) { plat_move = move; }
               else if (g_focus == FOCUS_PROFILE && in_pressed[IN_PLAY]) open_profile = 1;
               move = page = play = want_menu = 0; held_dir = 0;
            }
         }
      }
      (void)start_down; (void)select_down; (void)pad;
      if (welcome_active())
      {  /* first-run presentation: owns the input until it is closed */
         ui_nav_t wn;
         nav_fill(&wn, t);
         if (att_swallow) memset(&wn, 0, sizeof(wn));
         welcome_update(&wn, dt);
         memset(in_pressed, 0, sizeof(in_pressed));
         g_want_settings = 0; held_dir = 0;
         move = plat_move = page = play = want_menu = 0;
      }
      nav_fill(&fnav, t);
      fp_open = filepicker_open_now();
      {  /* file picker: a card over the panel (or the dashboard); owns the input while open */
         ui_nav_t pn = fnav;
         if (!fp_open || att_swallow) memset(&pn, 0, sizeof(pn));
         filepicker_update(&pn, t, dt);
         if (fp_open) { g_want_settings = 0; move = plat_move = page = play = want_menu = 0; held_dir = 0; }
      }
      {  /* USB transfer: a card over the panel or the Home; owns the input while open. On
          * the free Home it only listens for Square while its offer is up */
         ui_nav_t tn = fnav;
         const int tr_open = transfer_open_now();
         const int home = !tr_open && !fp_open && !panel_active() && !gamemenu_active() && !welcome_active() &&
                          !attract_active() && !att_swallow && play_t < 0 && g_focus == FOCUS_GAMES;
         if (att_swallow || fp_open || (!tr_open && !home)) memset(&tn, 0, sizeof(tn));
         transfer_update(&tn, t, dt, home);
         if (tr_open || transfer_open_now())
         { g_want_settings = 0; move = plat_move = page = play = want_menu = 0; held_dir = 0; fp_open = 1; }
      }
      {  /* library check: a card over the panel; owns the input while open */
         ui_nav_t an = fnav;
         const int au_open = audit_open_now();
         if (!au_open || att_swallow || fp_open) memset(&an, 0, sizeof(an));
         audit_update(&an, t, dt);
         if (au_open || audit_open_now())
         { g_want_settings = 0; move = plat_move = page = play = want_menu = 0; held_dir = 0; fp_open = 1; }
      }
      {  /* connections cards: over the panel; own the input while open (conn.h) */
         ui_nav_t cn = fnav;
         const int cn_open = conn_open_now();
         const char *nt;
         net_poll();
         if ((nt = net_take_toast())) panel_toast(nt);
         {  /* modules: their per-frame work, and the online / offline events (module.h) */
            static int was_online = -1;
            const net_state_t *ms = net_state();
            const int online = ms->wifi == NET_CONNECTED || ms->eth == NET_CONNECTED;
            modules_poll(t);
            panel_audio_route(t);   /* the sound follows the chosen output as it comes and goes */
            if (online != was_online)
            {
               if (was_online >= 0 || online) modules_emit(online ? MEV_ONLINE : MEV_OFFLINE, NULL);
               was_online = online;
            }
         }
         {  /* software update: the daily check runs while a connection is up (update.h) */
            const net_state_t *us = net_state();
            update_poll(t, us->wifi == NET_CONNECTED || us->eth == NET_CONNECTED, us->time_ok);
            if ((nt = update_take_toast())) panel_toast(nt);
         }
         net_watch(panel_connections_page(), conn_wifi_shown(), conn_bt_shown());
         if (!cn_open || att_swallow || fp_open) memset(&cn, 0, sizeof(cn));
         conn_update(&cn, t, dt);
         if (cn_open || conn_open_now())
         { g_want_settings = 0; move = plat_move = page = play = want_menu = 0; held_dir = 0; fp_open = 1; }
      }
      {  /* kernel card: over the panel; owns the input while open (kern.h) */
         ui_nav_t kn = fnav;
         const int kn_open = kern_open_now();
         if (!kn_open || att_swallow || fp_open) memset(&kn, 0, sizeof(kn));
         kern_update(&kn, t, dt);
         if (kn_open || kern_open_now())
         { g_want_settings = 0; move = plat_move = page = play = want_menu = 0; held_dir = 0; fp_open = 1; }
      }
      {  /* no controller: the receiver / pairing card, over everything; owns the input (padwait.h) */
         ui_nav_t pn = fnav;
         const int pw_open = padwait_open_now();
         if (!pw_open) memset(&pn, 0, sizeof(pn));
         padwait_update(&pn, t, dt, SDL_NumJoysticks(), welcome_active() || attract_active() || play_t >= 0 || kern_open_now());
         if (pw_open || padwait_open_now())
         { g_want_settings = 0; move = plat_move = page = play = want_menu = 0; held_dir = 0; fp_open = 1; }
      }
      {  /* settings panel: owns the input while open or closing (6.3) */
         ui_nav_t nav = fnav;
         int was_open = panel_active(), gm = gamemenu_active();
         if (!was_open || att_swallow || fp_open) memset(&nav, 0, sizeof(nav));
         if (!was_open && !gm && !fp_open && in_pressed[IN_OPTIONS] && play_t < 0 && !att_swallow) panel_open();
         if (g_want_settings)   /* Select tapped: opens the panel; while open it acts like Triangle */
         {
            g_want_settings = 0;
            if (!was_open && !gm && play_t < 0) panel_open();
            else if (was_open) nav.options = 1;
         }
         if (open_profile && !was_open && !gm) { panel_open_profile(); memset(&nav, 0, sizeof(nav)); }
         panel_update(&nav, t, dt);
         if (was_open || panel_active()) { move = plat_move = page = play = want_menu = 0; held_dir = 0; }
      }
      {  /* game menu (Start) and resume question: own the input while open; the keyboard
          * it opens for a new folder is the panel's (above) */
         ui_nav_t gn = fnav;
         int open_now = gamemenu_open_now(), r;
         if (!open_now || panel_active() || att_swallow || fp_open) memset(&gn, 0, sizeof(gn));
         gamemenu_update(&gn, t, dt);
         if (open_now || gamemenu_active()) { move = plat_move = page = play = want_menu = 0; held_dir = 0; }
         r = gamemenu_take();
         if (r == GM_PLAY || r == GM_PLAY_RESUME) { play = 1; g_play_resume = r == GM_PLAY_RESUME; g_play_asked = g_play_resume; }   /* "Play" in the menu still asks */
      }
      if (play_t >= 0) { move = plat_move = page = play = want_menu = 0; }
      if (att_swallow) { move = plat_move = page = play = want_menu = 0; held_dir = 0; held_next = t + 0.32; }
      if (n && want_menu && play_t < 0) gamemenu_open(pl->first + sel);
      if (n && play && play_t < 0 && !g_play_asked)
      {  /* left with Save and exit: ask before playing (Continue / Start over) */
         g_play_resume = 0;
         if (resume_has(g_games[pl->first + sel].content)) { gamemenu_resume_open(pl->first + sel); play = 0; }
      }
      g_play_asked = 0;
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

      if (n && move)
      {
         sel = (sel + move + n) % n; info_y.x = 18.0f; info_a.x = 0.0f; sel_t = t; nav_dir = move;
         if (!attract_active() && n > 1) music_sfx(move > 0 ? SFX_MOVE_NEXT : SFX_MOVE_PREV);
      }
      if (n > 1 && page)
      {  /* a page is what the row shows to the right of the selection; from the last
          * (first) game the next (previous) page wraps to the other end */
         int ns = page > 0 ? (sel == n - 1 ? 0 : sel + dash_page(pl->tile_ar)) : (sel == 0 ? n - 1 : sel - dash_page(pl->tile_ar));
         ns = ns < 0 ? 0 : (ns > n - 1 ? n - 1 : ns);
         if (ns != sel) { sel = ns; info_y.x = 18.0f; info_a.x = 0.0f; sel_t = t; music_sfx(page > 0 ? SFX_PAGE_NEXT : SFX_PAGE_PREV); }
      }
      if (g_nplat > 1 && plat_move)
      {
         int np = restore >= 0 ? restore : (sel_plat + plat_move + g_nplat) % g_nplat;
         if (restore < 0 && !attract_active()) music_sfx(plat_move > 0 ? SFX_SYSTEM_NEXT : SFX_SYSTEM_PREV);
         sel_plat = np; sel = 0; pl = &g_plat[sel_plat]; n = pl->count; sel_t = t;
         if (restore >= 0 && home_sel < n) sel = home_sel;
         info_y.x = 18.0f; info_a.x = 0.0f; plat_x.v = plat_move * 900.0f;
         dpos.x = (float)sel - 0.6f * plat_move; dpos.v = 0;   /* the new row slides in a little */
      }
      if (n && play && play_t < 0)
      {
         game_t *pg = &g_games[pl->first + sel];
         play_t = t;
         music_sfx(SFX_PLAY);
         logf_("play: %s", pg->content);
         play_pt = 0.0f;
         flat_begin(pl, pg);
      }
      /* the launch: the system's illustration and the game's painted cover, ready before Play */
      if (n && play_t < 0 && t - sel_t > 0.4) flat_want(pl, &g_games[pl->first + sel], 0);
      flat_poll();
      if (n && play_t < 0 && t - sel_t > 0.4) flat_ahead(pl, &g_games[pl->first + sel]);

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
      { double ps = now_s(); bg_poll_upload(); perf_stall("background upload", ps); }
      if (n && play_t < 0 && t - sel_t > 0.6)
      {  /* settled: the background of the next game in the direction of travel, decoded ahead */
         static char pre_last[512];
         const game_t *ng = &g_games[pl->first + (sel + (nav_dir < 0 ? -1 : 1) + n) % n];
         if (ng->bg[0] && strcmp(ng->bg, pre_last)) { snprintf(pre_last, sizeof(pre_last), "%s", ng->bg); bg_prefetch(ng->bg); }
      }
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
      { double ps = now_s(); status_update(t, &g_status); perf_stall("status", ps); }

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
            double ps = now_s();
            game_finish_cover(&g_games[done], cs, avg, acc);
            if (cs) SDL_FreeSurface(cs);
            perf_stall("cover upload", ps);
         }
         if (n)
         {
            /* what is on screen first (the selection, the row to its right, the tile
             * leaving on the left), then the page R1 would show and the one L1 would:
             * a page turn finds its covers decoded instead of filling in one by one */
            static const signed char order[] = { 0, 1, -1, 2, 3, 4, 5, 6, 7, -2,
                                                 8, 9, 10, 11, 12, 13, -3, -4, -5, -6, -7, -8 };
            int placed = 0, oldest = -1;
            double bytes = 0.0;
            for (k = 0; k < (int)sizeof(order); k++)
            {
               int gi = pl->first + ((sel + order[k]) % n + n) % n;
               game_t *g = &g_games[gi];
               g->tex_used = t;
               if (placed || g->tex_state) continue;
               if (g->has_cover) { if (!cv_busy) { g->tex_state = 2; cover_request(gi, g->cover); } placed = 1; }
               else { double ps = now_s(); game_placeholder(g); placed = 1; perf_stall("placeholder", ps); }
            }
            /* covers away from the selection (and other systems' rows) stay on the GPU,
             * so coming back does not decode them again, up to COVER_CACHE_BYTES: past
             * that the one that has been away longest goes, one a frame */
            for (k = 0; k < g_ngames; k++)
            {
               game_t *g = &g_games[k];
               if (g->tex_state != 1 && g->tex_state != -1) continue;
               bytes += (g->tex_state == 1 ? (double)g->tex_w * g->tex_h : (double)SP(256) * SP(256)) * 5.34;   /* RGBA with its mipmaps */
               if (g->tex_used < t && (oldest < 0 || g->tex_used < g_games[oldest].tex_used)) oldest = k;
            }
            if (bytes > COVER_CACHE_BYTES && oldest >= 0) game_free_tex(&g_games[oldest]);
         }
      }

      /* animation targets */
      {
         int covered, blackout, panel_cleared = 0, fold = 0;
         float fold_a = 0.0f, fold_c[4] = { 0, 0, 0, 0 };
         float launch_ui;   /* dashboard UI and help bar: fade out in 0.35 s as the launch starts */
         float pdt = 0.0f;   /* launch clock step this frame */
         /* the launch runs on real time (launch_dt): at 15 fps it lasts what it lasts at
          * 60, it does not stretch with the frame time like the dashboard's springs */
         if (play_t >= 0 && t > play_t) { pdt = g_launch_shots ? 1.0f / 30.0f : launch_dt; play_pt += pdt; }
         if (play_t >= 0)
         {
            /* the flat launch (flat.inc) is drawn from pt alone; here: the click, the fade, the end */
            const float pt = play_pt, end = flat_end_t(), fl_t = flat_fade_t();
            if (pdt > 0.0f && play_pt - pdt < FLAT_SEAT && play_pt >= FLAT_SEAT && (g_flat.medium != FM_NONE || g_flat.port))
               music_click(g_flat.medium == FM_FRONT ? 1 : (g_flat.medium == FM_BEHIND || (g_flat.port && g_flat.generic) ? 2 : 0));   /* a port's card switches on with the small snap */
            fade = clampf((pt - fl_t) / 0.30f, 0, 1);
            if (pt > end)
            {
               exit_code = 10; running = 0;
               if (lt_n) logf_("launch: %d frames in %.2f s (%.1f fps), worst %.0f ms, %d over 25 ms; animation %.2f s",
                               lt_n, lt_sum, lt_n / lt_sum, lt_max * 1000.0f, lt_slow, end);
            }
         }
         else fade = fade > 0 ? fade - dt * 1.6f : 0;
         launch_ui = play_t >= 0 ? 1.0f - clampf(play_pt / 0.35f, 0.0f, 1.0f) : 1.0f;
         /* the launch has faded to black: nothing under the black is drawn */
         blackout = play_t >= 0 && fade >= 0.999f;

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
         crt_on = crt_begin(g_dw, g_dh);   /* CRT filter: the frame goes to its target */
         glViewport(0, 0, g_dw, g_dh);
         /* settings fully open: an opaque full-screen section. The clear takes its colour
          * (free on a tile-based GPU) instead of a full-screen quad drawn over the clear */
         panel_cleared = 0;   /* the settings sit on the art now (lumen-design): nothing is cleared to a slab */
         {  /* the scrim of the menu that is open, folded into everything under it (dim_fold):
             * the settings' (drawn after the dashboard) or, with no settings, the game menu's
             * (drawn after the help bar) */
            const float pa0 = panel_amount(), ga0 = gamemenu_amount();
            fold = 0; fold_a = 0.0f;
            theme_color(TC_BACKDROP, fold_c);
            if (!blackout && play_t < 0 && g_scrim_fold)
            {
               if (pa0 > 0.003f) { fold = 1; fold_a = ui_scrim_settings() * pa0; }
               else if (ga0 > 0.003f) { fold = 2; fold_a = ui_scrim_alpha() * ga0; }
            }
            if (fold) dim_fold(fold_c, fold_a); else dim_none();
         }
         if (blackout) glClearColor(0, 0, 0, 1);
         else if (padwait_amount() > 0.995f) glClearColor(0.02f, 0.02f, 0.03f, 1);   /* the "no controller" card's backdrop, for free (padwait.c) */
         else glClearColor(bg[0].x * (1 - fold_a) + fold_c[0] * fold_a, bg[1].x * (1 - fold_a) + fold_c[1] * fold_a,
                           bg[2].x * (1 - fold_a) + fold_c[2] * fold_a, 1);
         glClearDepthf(1.0f);
         glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
         glEnable(GL_BLEND);
         glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

         glUseProgram(g_prog_2d);
         glUniform1i(u2_tex, 0);
         covered = welcome_amount() > 0.995f || blackout ||   /* a full-screen section is open */
                   padwait_amount() > 0.995f ||              /* the "no controller" card's backdrop is opaque */
                   (play_t >= 0 && play_pt > 0.44f);   /* or the launch's opaque field covers it all */
         if (!covered)
         {  /* full-screen art (current layer, next one crossfading over it), then the
             * legibility overlay (left side, top and bottom bands) in one pass */
            float e = bg_mix * bg_mix * (3 - 2 * bg_mix);
            const float ov[4] = { 1, 1, 1, 1 };
            (void)ov;
            g_bg_dim = settings_get_int("bg_dim", 45, 0, 90) / 100.0f;
            bg_draw_layers(e, t);   /* opaque: the scrim is baked into both kinds of layer */
         }
         if (g_fx_text && !covered)
         {  /* dashboard UI: dimmed under the panel, gone in the attract scene, fading out
             * as the launch starts (its field of colour wipes over it) */
            const float pa = panel_amount();
            const float ua = (1 - fade) * (1 - att.amount) * launch_ui;
            g_dash_body = 1.0f - pa;
            dash_draw(pl, sel_plat, n, sel, dpos.x, sel_t, t, ua, info_a.x, info_y.x);
         }

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
            if (fold == 1) dim_none();   /* scrim.menu, folded: what follows is over it */
            else if (pa > 0.003f && !panel_cleared)
            {
               ui_scrim(pa * ui_scrim_settings() / ui_scrim_alpha());   /* scrim.menu: the art and the status row stay, much quieter */
            }
            {  /* help bar: the set follows the panel, crossfading over 0.15 s; while the
                * panel is open it is centred in the free space left of it */
               static int h_cur = HINTS_CAROUSEL, h_prev = -1;
               static float h_mix = 1.0f;
               int want = welcome_active() ? (welcome_language_page() ? HINTS_WELCOME_LANG : welcome_last_page() ? HINTS_WELCOME_LAST : HINTS_WELCOME) :
                          filepicker_open_now() ? HINTS_FILEPICKER :
                          transfer_open_now() ? transfer_hint_set() :
                          audit_open_now() ? HINTS_AUDIT :
                          conn_open_now() ? conn_hint_set() :
                          kern_open_now() ? kern_hint_set() :
                          panel_active() ? panel_hint_set() :
                          gamemenu_open_now() ? (gamemenu_folders_open() ? HINTS_GAMEMENU_FOLDERS : HINTS_GAMEMENU) :
                          (attract_active() ? HINTS_ATTRACT : g_focus == FOCUS_TABS ? HINTS_DASH_TABS :
                           g_focus == FOCUS_PROFILE ? HINTS_DASH_PROFILE : HINTS_CAROUSEL);
               const float ha = (1 - fade) * (1 - att.amount) * launch_ui * (1 - padwait_amount());   /* the "no controller" card has its own bar */
               if (want != h_cur) { h_prev = h_cur; h_cur = want; h_mix = 0.0f; }
               h_mix = clampf(h_mix + dt / 0.15f, 0.0f, 1.0f);
               /* the game menu is a sheet on the right edge (gamemenu.c, 468 px): its help
                * bar is at the foot of the sheet (lumen-2 mockup), inside its 40 px margins */
#define HINT_GM(set) ((set) == HINTS_GAMEMENU || (set) == HINTS_GAMEMENU_FOLDERS)
#define HINT_AT(set) (g_hint_cx = HINT_GM(set) ? W - 468.0f * 0.5f : W * 0.5f, g_hint_w = HINT_GM(set) ? 468.0f - 80.0f : 0.0f)
               if (h_prev >= 0 && h_mix < 1.0f) { HINT_AT(h_prev); hints_draw(h_prev, 672, ha * (1.0f - h_mix)); }
               HINT_AT(h_cur);
               hints_draw(h_cur, 672, ha * h_mix);
               g_hint_cx = W * 0.5f; g_hint_w = 0.0f;
#undef HINT_AT
#undef HINT_GM
            }
            if (fold == 2) { dim_none(); ui_scrim_skip(1); }   /* its scrim is in the frame already */
            gamemenu_draw(t);
            ui_scrim_skip(0);
            if (gamemenu_active())
            {  /* its scrim covered the help bar: the hints describe the focused layer, never dimmed */
               glUseProgram(g_prog_2d);
               g_hint_cx = W - 468.0f * 0.5f; g_hint_w = 468.0f - 80.0f;   /* at the foot of the sheet */
               hints_draw(gamemenu_folders_open() ? HINTS_GAMEMENU_FOLDERS : HINTS_GAMEMENU, 672, gamemenu_amount() * (1 - fade));
               g_hint_cx = W * 0.5f; g_hint_w = 0.0f;
            }
            panel_draw(t);
            filepicker_draw(t);
            if (filepicker_active())
            {  /* its backdrop covered the help bar: draw the picker's set again on top */
               glUseProgram(g_prog_2d);
               hints_draw(HINTS_FILEPICKER, 672, filepicker_amount() * (1 - fade));
            }
            transfer_draw(t);
            if (transfer_active())
            {  /* same as the picker: its scrim covered the help bar */
               glUseProgram(g_prog_2d);
               hints_draw(transfer_hint_set(), 672, transfer_amount() * (1 - fade));
            }
            audit_draw(t);
            if (audit_active())
            {
               glUseProgram(g_prog_2d);
               hints_draw(HINTS_AUDIT, 672, audit_amount() * (1 - fade));
            }
            conn_draw(t);
            if (conn_active() && conn_open_now())
            {
               glUseProgram(g_prog_2d);
               hints_draw(conn_hint_set(), 672, conn_amount() * (1 - fade));
            }
            kern_draw(t);
            if (kern_open_now())
            {
               glUseProgram(g_prog_2d);
               hints_draw(kern_hint_set(), 672, kern_amount() * (1 - fade));
            }
            padwait_draw(t);
            if (padwait_open_now())
            {
               glUseProgram(g_prog_2d);
               hints_draw(HINTS_PADWAIT, 672, padwait_amount() * (1 - fade));
            }
            panel_toast_draw(t);
            glUseProgram(g_prog_2d);
            attract_draw(t);
            if (welcome_active())
            {  /* over everything; the help bar is drawn again on top of it */
               int hs = welcome_language_page() ? HINTS_WELCOME_LANG : welcome_last_page() ? HINTS_WELCOME_LAST : HINTS_WELCOME;
               welcome_draw(t);
               hints_draw(hs, 672, welcome_amount());
            }
            if (n && play_t >= 0 && !blackout) flat_draw(play_pt, &g_games[pl->first + sel], pl);
            if (fade > 0.001f && !blackout)
            {  /* the fade to black */
               const float over[4] = { 0, 0, 0, fade };
               draw_2d(0, 0, 0, W, H, over, 0);
            }
            if (fade > 0.001f)
            {
               if (play_t >= 0)
               {
                  /* the machine is in the middle of the screen until the black has it */
                  const float st[4] = { 0.93f, 0.92f, 0.97f, clampf((fade - 0.6f) / 0.4f, 0, 1) };
                  label_set(&l_start, g_font_sub, tr("launch.starting"));
                  draw_2d(l_start.tex, (W - l_start.w) * 0.5f, (H - l_start.h) * 0.5f, l_start.w, l_start.h, st, 0);
               }
            }
         }
      }

      if (g_splash_fill >= 0.0f)
      {  /* the startup splash (full logo on black) dissolves into the dashboard */
         /* The splash stays whole over the first frames of the dashboard, until the covers
          * on screen and the background are in (0.7 s at most): those frames carry the
          * dashboard's first texts, uploads and shader states, and drawn under the splash
          * they do not stutter the dissolve or the logo's entrance. Then the dissolve
          * starts and the entrances play from their beginning. */
         static double s0 = -1.0, h0 = -1.0;
         float sa;
         if (h0 < 0.0) h0 = t;
         if (s0 < 0.0)
         {
            int q, ready = !n || t - h0 > 0.7;
            if (!ready && t - h0 > 0.05)
            {
               ready = !bg_empty(&bg_cur) || !bg_empty(&bg_next);
               for (q = 0; q <= 6 && q < n && ready; q++)
               {
                  const int st = g_games[pl->first + (sel + q) % n].tex_state;
                  if (st != 1 && st != -1) ready = 0;
               }
            }
            if (ready)
            {
               s0 = t; g_logo_t0 = -1.0; info_y.x = 18.0f; info_a.x = 0.0f;
               if (g_perf_on) logf_("perf: dashboard ready under the splash after %.0f ms", (t - h0) * 1000.0);
            }
         }
         sa = s0 < 0.0 ? 1.0f : 1.0f - (float)(t - s0) / 0.35f;
         if (sa > 0.0f)
         {
            const float black[4] = { 0, 0, 0, sa };
            glDisable(GL_DEPTH_TEST); glDisable(GL_SCISSOR_TEST);
            glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glUseProgram(g_prog_2d);
            draw_2d(0, 0, 0, W, H, black, 0);
            splash_logo_draw(1.0f, sa);
            splash_loading_draw(t, sa);
         }
         else
         {
            glDeleteTextures(4, g_logo_big.tex);
            memset(g_logo_big.tex, 0, sizeof(g_logo_big.tex));
            g_splash_fill = -1.0f;
         }
      }
      crt_end();   /* the CRT pass puts the frame on the window (shots below include it) */
      if (crt_on && g_cost_on)
      {  /* one opaque full-screen pass, counted as a heavy shader */
         g_cost_px_opaque += (double)g_dw * g_dh; g_cost_px_heavy += (double)g_dw * g_dh; g_cost_draws += 1;
      }
      if (g_ntshot) test_shots(g_rec_on ? g_rec_frames / g_rec_fps : t - t0);
      if (g_launch_shots && play_t >= 0)
      {
         static int lf;
         if (lf % 3 == 0) launch_shot(lf / 3);
         lf++;
      }
      {
         double pre = now_s();
         t_pre = pre;
         if (!rec_frame()) running = 0;
         SDL_GL_SwapWindow(win);
         b_draw += pre - t; b_swap += now_s() - pre;
         perf_frame(t, t_upd, pre, now_s());
         if (frames == 30)
         {  /* boot.sh: a picture is on screen (clears the compositor's one-shot guard) */
            FILE *pf = fopen("/tmp/lumen-presented", "w");
            if (pf) fclose(pf);
         }
      }
      frames++; g_frame_id++;
      if (g_cost_on && play_t >= 0)
      {  /* launch trace: one line per frame (pt, frame time, CPU issuing, swap, cost) */
         double fpx = g_cost_px_opaque + g_cost_px_blend, fh = g_cost_px_heavy;
         logf_("launch frame: pt %.3f dt %.1f ms, update %.1f ms, draw %.1f ms, swap %.1f ms, %.2f Mpx (heavy %.2f)",
               play_pt, dt * 1000.0, (t_upd - t) * 1000.0, (t_pre - t_upd) * 1000.0, (now_s() - t_pre) * 1000.0,
               (fpx - lp_px) / 1e6, (fh - lp_heavy) / 1e6);
      }
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
   if ((exit_code == 10 || exit_code == 11) && g_nplat)
   {  /* modules: a game or app starts next (docs/modules.md, MEV_LAUNCH) */
      const int gi = exit_code == 11 ? attract_demo_game() : g_plat[sel_plat].first + sel;
      if (gi >= 0) modules_emit(MEV_LAUNCH, &gi);
   }
   modules_emit(MEV_EXIT, NULL);
   music_shutdown();   /* release the audio device before RetroArch starts */
   transfer_shutdown();   /* stop a copy, unmount the second USB drive */
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
      req.app = !strcmp(lp->layout, "apps") || g->port;
      if (req.app) snprintf(core, sizeof(core), "app");   /* no libretro core: run.sh runs the program */
      req.core = core; req.content = g->content; req.system = lp->id; req.title = g->title;
      req.profile = profiles_active()->id;
      req.demo = demo;
      req.resume = !demo && g_play_resume;
      if (!req.app && crt_level() != CRT_OFF)
      {  /* same filter in the game; crt_shader=quality in settings.ini: the heavier maths */
         static char crt[24];
         snprintf(crt, sizeof(crt), "%s%s", crt_id(crt_level()),
                  strcmp(settings_get("crt_shader", "fast"), "quality") ? "" : "-quality");
         req.crt = crt;
      }
      req.demo_seconds = demo && lp->demo_seconds > 0 ? lp->demo_seconds : settings_get_int("demo_seconds", 60, 20, 300);
      if (lp->has_gfx && !req.app)   /* a native port has no core to configure */
      {
         char gkey[48];
         const char *gv, *nat;
         int enh;
         snprintf(gkey, sizeof(gkey), "graphics.%s", lp->id);
         gv = settings_get(gkey, "original");
         enh = !strcmp(gv, "enhanced");
         /* native: the list for this output (1080p, else 720p), the other one if only
          * one is declared; a system without it falls back to enhanced */
         nat = g_dh >= 1080 ? (lp->gfx_nat1080[0] ? lp->gfx_nat1080 : lp->gfx_nat720)
                            : (lp->gfx_nat720[0] ? lp->gfx_nat720 : lp->gfx_nat1080);
         if (!strcmp(gv, "native")) { if (lp->has_native) enh = 2; else enh = 1; }
         req.graphics = enh == 2 ? "native" : (enh ? "enhanced" : "original");
         req.opt_core_name = lp->gfx_core;
         req.opt_list = enh == 2 ? nat : (enh ? lp->gfx_enh : lp->gfx_orig);
         req.ra_list = enh == 2 ? lp->gfx_ra_nat : (enh ? lp->gfx_ra_enh : lp->gfx_ra_orig);
      }
      if (demo)
      {  /* --focus-demo reads it back when a button interrupts the demo */
         FILE *f = fopen("/tmp/lumen-demo-game", "w");
         if (f) { fprintf(f, "system=%s\ncontent=%s\n", lp->id, g->content); fclose(f); }
      }
      if (launch_write("/tmp/lumen-launch", &req) == 0)
         logf_("launch request: %s (%s %ds, profile %s, graphics %s%s%s)", g->content, demo ? "demo" : (req.app ? "app" : "normal"),
               req.demo_seconds, req.profile, req.graphics ? req.graphics : "-", req.resume ? ", resume" : "", req.app ? ", app" : "");
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
