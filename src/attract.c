/* Screensaver / attract scene and demo picker (docs/settings-design.md 10).
 * No allocation and no file I/O per frame: settings are re-read once a second while
 * waiting, the demo history (/tmp, a few lines) only when a demo is picked or launched. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "lumen.h"
#include "attract.h"
#include "i18n.h"
#include "settings.h"

#define HISTORY_PATH "/tmp/lumen-demo-history"
#define HISTORY_N    4
#define ENTER_S      1.2
#define DEMO_OUT_S   1.5
#define LEAVE_S      0.6
#define MOVE_EVERY_S 4.0
#define MOVES_PER_PLATFORM 6

enum { A_OFF, A_ENTER, A_IDLE, A_DEMO_OUT, A_LEAVE };

static int    s_state = A_OFF, s_pending, s_moves, s_pick = -1, s_launched, s_no_demo, s_candidates;
static double s_proc_start, s_state_t, s_start_t, s_last_input, s_next_move, s_grace_until, s_cfg_t = -1;
static int    s_minutes = 5, s_demo_on = 1, s_demo_delay = 20;
static spring s_amt;
static label_t s_l_press, s_l_badge, s_l_title;

static float smooth(float e0, float e1, float x)
{
   float u = clampf((x - e0) / (e1 - e0), 0.0f, 1.0f);
   return u * u * (3.0f - 2.0f * u);
}

static int is_candidate(int i)
{
   const game_t *g = &g_games[i];
   const platform_t *p;
   if (i >= g_nreal_games) return 0;   /* collections repeat library games */
   if (g->platform < 0 || g->platform >= g_nplat || !g->content[0] || g->port) return 0;   /* a port is a program: it cannot run as an ephemeral session */
   p = &g_plat[g->platform];
   return p->demo_allowed && p->core[0];
}

static void read_config(double t, int force)
{
   if (!force && s_cfg_t >= 0 && t - s_cfg_t < 1.0) return;
   s_cfg_t = t;
   s_minutes = settings_get_int("screensaver_minutes", 5, 0, 60);
   s_demo_on = settings_get_int("demo_enabled", 1, 0, 1);
   s_demo_delay = settings_get_int("attract_demo_delay", 20, 5, 120);
}

static void set_state(int st, double t) { s_state = st; s_state_t = t; }

void attract_init(int start_active, const char *home)
{
   int i;
   (void)home;
   srand((unsigned)time(NULL) ^ (unsigned)getpid());
   s_proc_start = s_last_input = now_s();
   s_pending = start_active;
   for (i = 0, s_candidates = 0; i < g_ngames; i++) s_candidates += is_candidate(i);
   logf_("attract: %d demo candidates%s", s_candidates, start_active ? ", starting active" : "");
}

void attract_library_changed(void)
{
   int i;
   for (i = 0, s_candidates = 0; i < g_ngames; i++) s_candidates += is_candidate(i);
   s_pick = -1;          /* old g_games index */
   s_no_demo = 0;
   s_launched = 0;
   logf_("attract: library changed, %d demo candidates", s_candidates);
}

void attract_start(double t)
{
   read_config(t, 1);
   s_pending = 0;
   s_moves = 0;
   s_pick = -1;
   s_launched = 0;
   s_no_demo = 0;
   s_start_t = t;
   s_next_move = t + MOVE_EVERY_S;
   s_grace_until = t + 0.3;   /* the press that asked for it ("Preview now") */
   set_state(A_ENTER, t);
   logf_("attract: start");
}

void attract_note_input(double t)
{
   s_last_input = t;
   if (t < s_proc_start + 0.4 || t < s_grace_until) return;   /* a button still held from a demo */
   if (s_state == A_ENTER || s_state == A_IDLE || s_state == A_DEMO_OUT)
   {
      if (s_launched) return;
      set_state(A_LEAVE, t);
      s_pick = -1;
      logf_("attract: leave (input)");
   }
   s_pending = 0;
}

int attract_active(void) { return s_state == A_ENTER || s_state == A_IDLE || s_state == A_DEMO_OUT; }

int attract_demo_game(void) { return s_launched ? s_pick : -1; }

/* last HISTORY_N contents demoed (one per line, oldest first) */
static int history_read(char hist[HISTORY_N][512])
{
   char line[512];
   int n = 0, i;
   FILE *f = fopen(HISTORY_PATH, "r");
   if (!f) return 0;
   while (fgets(line, sizeof(line), f))
   {
      line[strcspn(line, "\r\n")] = 0;
      if (!line[0]) continue;
      if (n == HISTORY_N) { for (i = 1; i < HISTORY_N; i++) memcpy(hist[i - 1], hist[i], 512); n--; }
      snprintf(hist[n++], 512, "%s", line);
   }
   fclose(f);
   return n;
}

static int in_history(const char *c, char hist[HISTORY_N][512], int n)
{
   int i;
   for (i = 0; i < n; i++) if (!strcmp(hist[i], c)) return 1;
   return 0;
}

int attract_pick_demo(void)
{
   static char hist[HISTORY_N][512];
   int nh = history_read(hist), i, count = 0, r;
   for (i = 0; i < g_ngames; i++) count += is_candidate(i) && !in_history(g_games[i].content, hist, nh);
   if (!count && nh > 1)
   {  /* fewer candidates than the history: only avoid repeating the last one */
      memcpy(hist[0], hist[nh - 1], 512); nh = 1;
      for (i = 0; i < g_ngames; i++) count += is_candidate(i) && !in_history(g_games[i].content, hist, nh);
   }
   if (!count)
   {  /* a single candidate: it may repeat */
      nh = 0;
      for (i = 0; i < g_ngames; i++) count += is_candidate(i);
   }
   if (!count) return -1;
   r = rand() % count;
   for (i = 0; i < g_ngames; i++)
      if (is_candidate(i) && !in_history(g_games[i].content, hist, nh) && r-- == 0) return i;
   return -1;
}

static void history_add(const char *content)
{
   static char hist[HISTORY_N][512];
   int n = history_read(hist), i;
   FILE *f = fopen(HISTORY_PATH ".tmp", "w");
   if (!f) return;
   for (i = n >= HISTORY_N ? 1 : 0; i < n; i++) fprintf(f, "%s\n", hist[i]);
   fprintf(f, "%s\n", content);
   if (fclose(f) == 0) rename(HISTORY_PATH ".tmp", HISTORY_PATH);
}

void attract_update(double t, float dt, int can_start, attract_out_t *out)
{
   double st = t - s_state_t;
   float target;
   memset(out, 0, sizeof(*out));
   switch (s_state)
   {
   case A_OFF:
      read_config(t, 0);
      if (can_start && (s_pending || (s_minutes > 0 && t - s_last_input >= s_minutes * 60.0)))
         attract_start(t);
      break;
   case A_ENTER:
      if (st >= ENTER_S) set_state(A_IDLE, t);
      break;
   case A_IDLE:
      if (s_demo_on && s_candidates && !s_no_demo && t - s_start_t >= s_demo_delay)
      {
         s_pick = attract_pick_demo();
         if (s_pick >= 0) { set_state(A_DEMO_OUT, t); logf_("attract: demo of %s", g_games[s_pick].content); }
         else s_no_demo = 1;
      }
      break;
   case A_DEMO_OUT:
      if (st >= DEMO_OUT_S && !s_launched && s_pick >= 0)
      {
         s_launched = 1;
         out->launch_demo = 1;
         history_add(g_games[s_pick].content);
      }
      break;
   case A_LEAVE:
      if (st >= LEAVE_S) { set_state(A_OFF, t); s_last_input = t; }
      break;
   }
   /* auto-rotation through the normal carousel path */
   if ((s_state == A_ENTER || s_state == A_IDLE) && t >= s_next_move)
   {
      s_next_move = t + MOVE_EVERY_S;
      if (s_moves >= MOVES_PER_PLATFORM && g_nplat > 1) { out->plat_move = 1; s_moves = 0; }
      else { out->move = 1; s_moves++; }
   }
   target = attract_active() ? 1.0f : 0.0f;
   if (s_state == A_LEAVE) spring_step(&s_amt, target, 50, 14, dt);
   else spring_step(&s_amt, target, 12, 7, dt);
   if (s_state == A_OFF && s_amt.x < 0.002f) { s_amt.x = 0; s_amt.v = 0; }
   out->amount = clampf(s_amt.x, 0.0f, 1.0f);
   out->dim = 0.35f * out->amount;
}

/* slow hue rotation (period 24 s) of the ambient colour around the grey axis */
void attract_bg_target(const float base[3], double t, float out[3])
{
   const float is3 = 0.57735027f;
   float a = clampf(s_amt.x, 0.0f, 1.0f), g, d[3], cx[3], th, c, s;
   int i;
   if (a <= 0.0f) { memcpy(out, base, sizeof(float) * 3); return; }
   g = (base[0] + base[1] + base[2]) / 3.0f;
   for (i = 0; i < 3; i++) d[i] = (base[i] - g) * 1.6f;
   /* a minimum tint so grey covers cycle too */
   d[0] += 0.030f; d[1] -= 0.015f; d[2] -= 0.015f;
   th = (float)(2.0 * M_PI * fmod(t, 24.0) / 24.0);
   c = cosf(th); s = sinf(th);
   cx[0] = (d[2] - d[1]) * is3; cx[1] = (d[0] - d[2]) * is3; cx[2] = (d[1] - d[0]) * is3;
   for (i = 0; i < 3; i++)
   {
      float v = g + d[i] * c + cx[i] * s;
      out[i] = lerpf(base[i], v < 0.0f ? 0.0f : v, a);
   }
}

void attract_draw(double t)
{
   float a = clampf(s_amt.x, 0.0f, 1.0f), out = 0.0f;
   if (s_state == A_OFF && a < 0.003f) return;
   if (s_state == A_DEMO_OUT) out = smooth(0.0f, 0.7f, (float)(t - s_state_t));
   {  /* PRESS START, blinking 0.25 <-> 1.0 over 1.6 s */
      float p = (float)fmod(t - s_start_t, 1.6) / 1.6f;
      float v = smooth(0.10f, 0.30f, p) * (1.0f - smooth(0.70f, 0.90f, p));
      float al = (0.25f + 0.75f * v) * a * (1.0f - out);
      const float shadow[4] = { 0, 0, 0, 0.45f * al }, col[4] = { 1.0f, 0.95f, 0.82f, al };
      if (al > 0.003f)
      {
         float x, y = 600.0f;
         label_set(&s_l_press, g_font_title, tr("attract.press_start"));
         x = floorf((W - s_l_press.w) * 0.5f);
         draw_2d(s_l_press.tex, x + 2, y + 3, s_l_press.w, s_l_press.h, shadow, 0);
         draw_2d(s_l_press.tex, x, y, s_l_press.w, s_l_press.h, col, 0);
      }
   }
   if (s_state == A_DEMO_OUT && s_pick >= 0)
   {
      const float black[4] = { 0, 0, 0, out };
      float ta = smooth(0.35f, 0.85f, (float)(t - s_state_t));
      const float badge[4] = { 1.0f, 0.82f, 0.30f, ta }, tc[4] = { 0.93f, 0.92f, 0.97f, ta };
      char s[200];
      draw_2d(0, 0, 0, W, H, black, 0);
      label_set(&s_l_badge, g_font_small, tr("attract.demo"));
      label_set(&s_l_title, g_font_sub, trf(s, sizeof(s), "attract.demo_of", "title", g_games[s_pick].title, NULL));
      draw_2d(s_l_badge.tex, floorf((W - s_l_badge.w) * 0.5f), H * 0.5f - 30 - s_l_badge.h, s_l_badge.w, s_l_badge.h, badge, 0);
      draw_2d(s_l_title.tex, floorf((W - s_l_title.w) * 0.5f), H * 0.5f - s_l_title.h * 0.5f, s_l_title.w, s_l_title.h, tc, 0);
   }
}
