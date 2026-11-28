/* "No controller": a screen of its own, see padwait.h.
 *
 * Minimal (lumen-design, "Controller"): the theme's backdrop, the LPSX logo at the top, one
 * pill that says what to do (press PS on a paired controller, pair a new one, or plug one
 * in by cable; while pairing, its name and three dots), and a controller drawn large in
 * outline at the foot of the screen, faint, its lower half off the screen. Bluetooth's own
 * state (no adapter, off, searching) is the status bar's Bluetooth glyph, not this screen. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL2/SDL_image.h>
#include "lumen.h"
#include "padwait.h"
#include "i18n.h"
#include "music.h"
#include "net.h"
#include "theme.h"
#include "ui.h"

#define PILL_H   56.0f
#define PILL_Y   262.0f    /* the pill's top */
#define LOGO_Y   150.0f    /* the logo's centre */
#define PAD_Y    408.0f    /* the controller's top: its lower part leaves the screen */

static int     s_open, s_dismissed;
static double  s_nopad_since = -1, s_cooldown, s_t0, s_done_t = -1;
static spring  s_fade, s_ok;   /* screen in/out; 0..1 once paired */
static spring  s_txt;          /* 0..1: the pill's text fading in after a change */
/* what the screen shows: taken from the network state while it is open and frozen when it
 * closes, so nothing changes under the fade-out (the worker resets its state at once) */
static int     v_bt, v_ap, v_paired, v_key = -1;
static char    v_name[64];
static int     s_armed;        /* a "done" left over from the previous screen is not this one's */
/* white art from the stick, tinted when drawn: <dir>/<name>[-1080].png */
typedef struct { GLuint tex; float w, h; int tried; } ill_t;
static ill_t   s_pad, s_logo;
static label_t s_pill;

/* the controller fades out toward its top and its sides: the alpha times a radial falloff
 * centred on the drawing's sticks, baked once at load (no pass of its own,
 * whatever the theme's backdrop) */
static SDL_Surface *fade_out(SDL_Surface *sf)
{
   int x, y;
   SDL_Surface *c = SDL_ConvertSurfaceFormat(sf, SDL_PIXELFORMAT_ABGR8888, 0);   /* bytes R G B A */
   if (!c) return sf;
   SDL_FreeSurface(sf);
   SDL_LockSurface(c);
   for (y = 0; y < c->h; y++)
      for (x = 0; x < c->w; x++)
      {
         Uint8 *px = (Uint8 *)c->pixels + y * c->pitch + x * 4;
         const float dx = ((x + 0.5f) / c->w - 0.5f) * 1.15f, dy = ((y + 0.5f) / c->h - 0.62f) * 1.20f;
         const float d = sqrtf(dx * dx + dy * dy), u = clampf((d - 0.22f) / 0.48f, 0.0f, 1.0f);
         px[3] = (Uint8)(px[3] * (1.0f - 0.86f * u * u * (3.0f - 2.0f * u)) + 0.5f);   /* the edges keep a trace */
      }
   SDL_UnlockSurface(c);
   return c;
}

static void ill_load(ill_t *il, const char *dir, const char *name)
{
   char p[400];
   SDL_Surface *sf;
   const int hd = g_ui_scale > 1.2f;
   if (il->tried) return;
   il->tried = 1;
   snprintf(p, sizeof(p), "%s/assets/%s/%s%s.png", g_home, dir, name, hd ? "-1080" : "");
   if (!(sf = IMG_Load(p))) { logf_("padwait: no %s", p); return; }
   if (!strcmp(name, "padwait")) sf = fade_out(sf);
   il->tex = tex_from_surface(sf);
   il->w = sf->w / (hd ? 1.5f : 1.0f); il->h = sf->h / (hd ? 1.5f : 1.0f);
   SDL_FreeSurface(sf);
}
static void ill_free(ill_t *il) { if (il->tex) glDeleteTextures(1, &il->tex); memset(il, 0, sizeof(*il)); }
static void col4(float o[4], int tc, float a) { theme_color(tc, o); o[3] = a; }

int   padwait_open_now(void) { return s_open; }
float padwait_amount(void) { return clampf(s_fade.x, 0.0f, 1.0f); }

static void close_card(double t, int dismissed)
{
   if (!s_open) return;
   s_open = 0; s_cooldown = t + 20.0;   /* s_done_t stays: the card fades out as it was */
   if (dismissed) s_dismissed = 1;   /* "not now": not again this session */
   music_sfx(dismissed ? SFX_BACK : SFX_CONFIRM);
   logf_("padwait: closed (%s)", dismissed ? "not now" : "controller connected");
}

void padwait_update(const ui_nav_t *nav, double t, float dt, int njoy, int blocked)
{
   static int off = -1;
   const net_state_t *ns = net_state();
   if (njoy > 0) s_nopad_since = -1;
   else if (s_nopad_since < 0) s_nopad_since = t;
   net_pad_watch(njoy == 0, s_open);
   if (off < 0) { const char *e = getenv("LUMEN_PADWAIT"); off = e && *e == '0'; }   /* psc-sim: no controllers there */
   if (!s_open && !off && !s_dismissed && !blocked && njoy == 0 && t >= s_cooldown &&
       s_nopad_since >= 0 && t - s_nopad_since >= 2.5)
   {
      s_open = 1; s_t0 = t; s_done_t = -1; s_ok.x = s_ok.v = 0;
      s_armed = ns->ap_state != NET_AP_DONE; v_key = -1;
      if (s_fade.x < 0.02f) { s_fade.x = 0; s_fade.v = 0; }
      music_sfx(SFX_OPEN);
      logf_("padwait: no controller (Bluetooth adapter %s)", ns->bt_present ? "present" : "absent");
   }
   if (s_open)
   {
      int key;
      /* an adapter (already, or plugged in now): Bluetooth on, the search starts */
      if (ns->bt_present && !ns->bt_on) net_bt_set(1);
      if (!s_armed && ns->ap_state != NET_AP_DONE) s_armed = 1;
      v_bt = ns->bt_present && ns->ap_state != NET_AP_NOADAPTER; v_paired = ns->paired;
      v_ap = !s_armed && ns->ap_state == NET_AP_DONE ? NET_AP_IDLE : ns->ap_state;
      if (v_ap == NET_AP_PAIRING || v_ap == NET_AP_DONE || v_ap == NET_AP_FAILED) snprintf(v_name, sizeof(v_name), "%s", ns->ap_name);
      if (v_ap == NET_AP_DONE && s_done_t < 0) { s_done_t = t; music_sfx(SFX_CONFIRM); }
      key = (v_bt ? 1 : 0) + (v_paired > 0 ? 2 : 0) + v_ap * 4 + (s_done_t >= 0 ? 64 : 0);
      if (key != v_key) { if (v_key >= 0) { s_txt.x = 0; s_txt.v = 0; } else s_txt.x = 1; v_key = key; }
      if (s_done_t >= 0 && (t - s_done_t >= 2.4 || (njoy > 0 && t - s_done_t >= 1.2))) close_card(t, 0);
      else if (njoy > 0 && s_done_t < 0) close_card(t, 0);         /* a wired one was plugged in */
      else if (nav && nav->back && t - s_t0 > 0.4) close_card(t, 1);
   }
   spring_step(&s_fade, s_open ? 1.0f : 0.0f, 120, 18, dt);
   spring_step(&s_ok, s_done_t >= 0 ? 1.0f : 0.0f, 140, 16, dt);
   spring_step(&s_txt, 1.0f, 150, 20, dt);
   if (!s_open && s_fade.x < 0.003f && (s_pill.tex || s_pad.tried))
   {  /* closed: its textures go */
      label_free(&s_pill); ill_free(&s_pad); ill_free(&s_logo);
   }
}

/* the pill's text for what is going on */
static const char *pill_text(char *buf, size_t n)
{
   if (s_done_t >= 0) return trf(buf, n, "padwait.pill_done", "name", v_name, NULL);
   if (v_bt && v_ap == NET_AP_PAIRING) return trf(buf, n, "padwait.pairing", "name", v_name, NULL);
   if (v_bt && v_ap == NET_AP_FAILED) return trf(buf, n, "padwait.failed", "name", v_name, NULL);
   if (!v_bt) return tr("padwait.pill_cable");
   return tr(v_paired > 0 ? "padwait.pill_ps" : "padwait.pill_new");
}

void padwait_draw(double t)
{
   const float a = padwait_amount(), ok = clampf(s_ok.x, 0.0f, 1.0f), ta = clampf(s_txt.x, 0.0f, 1.0f);
   const int busy = s_done_t < 0 && v_bt && v_ap == NET_AP_PAIRING;
   float bd[4], txt[4];
   char buf[200];
   if (a <= 0.003f) return;
   gl_use_2d();
   /* the theme's backdrop, the whole screen (fully open, main.c clears to it instead) */
   col4(bd, TC_BACKDROP, a);
   if (a <= 0.995f) draw_2d(0, 0, 0, W, H, bd, 0);

   ill_load(&s_pad, "ui", "padwait");
   ill_load(&s_logo, "brand", "logo-front");
   if (s_pad.tex)
   {  /* the controller, large and faint, rising a little as the screen comes in; it lights
       * up a touch once paired */
      const float pw = s_pad.w, ph = s_pad.h, y = PAD_Y + (1.0f - a) * 24.0f;
      const float c[4] = { 1, 1, 1, a * (0.46f + 0.30f * ok) };
      draw_2d(s_pad.tex, floorf((W - pw) * 0.5f), floorf(y), pw, ph, c, 0);
   }
   if (s_logo.tex)
   {
      const float lh = 34.0f, lw = lh * s_logo.w / s_logo.h, c[4] = { 1, 1, 1, a };
      draw_2d(s_logo.tex, floorf((W - lw) * 0.5f), floorf(LOGO_Y - lh * 0.5f), lw, lh, c, 0);
   }
   {  /* the pill: glass at rest, its text centred; three dots before it while pairing, the
       * accent's dot once done */
      const float lead = busy ? 42.0f : ok > 0.01f ? 30.0f : 0.0f;
      float pw, px, g[4];
      label_set(&s_pill, g_font_sub, pill_text(buf, sizeof(buf)));
      pw = s_pill.w + 64.0f + lead;
      px = floorf((W - pw) * 0.5f);
      theme_color(TC_GLASS, g); g[3] = UI_EDGE * a;
      ui_fill(px, PILL_Y, pw, PILL_H, PILL_H * 0.5f, g, UI_REST * a);
      ui_shape(px, PILL_Y, pw, PILL_H, PILL_H * 0.5f, 1.0f, g);
      col4(txt, TC_TEXT, a * ta);
      if (busy)
      {
         int i;
         for (i = 0; i < 3; i++)
         {
            const float ph = (float)fmod((t - s_t0) * 1.6 - i * 0.18, 1.0), da = 0.35f + 0.65f * (ph < 0.5f ? 1.0f - ph * 2.0f : 0.0f);
            const float d[4] = { txt[0], txt[1], txt[2], txt[3] * da };
            draw_round_rect(px + 32.0f + i * 12.0f, PILL_Y + PILL_H * 0.5f - 3.5f, 7, 7, 3.5f, d, 0.0f);
         }
      }
      else if (ok > 0.01f)
      {
         float ac[4];
         col4(ac, TC_ACCENT, a * ok);
         draw_round_rect(px + 32.0f, PILL_Y + PILL_H * 0.5f - 5.0f, 10, 10, 5, ac, 0.0f);
      }
      if (s_pill.tex) draw_2d(s_pill.tex, px + 32.0f + lead, floorf(PILL_Y + (PILL_H - s_pill.h) * 0.5f + (1.0f - ta) * 4.0f), s_pill.w, s_pill.h, txt, 0);
   }
}
