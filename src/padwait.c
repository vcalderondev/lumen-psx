/* "No controller": a two-step card, see padwait.h.
 *
 * Layout (one glass card over the scrim, the connections cards' family): on the left a
 * controller drawn with shapes (crisp at 720p and 1080p) inside a glass disc, with rings
 * rippling out while the console searches (faster while pairing; gone, and the controller
 * in the accent, once paired); on the right the kicker, the title, one line on what to do
 * and how the usual controllers enter pairing mode; at the foot, the live status. */
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

#define CW 960.0f
#define CH 520.0f
#define DISC_R 124.0f
#define TEXT_X 452.0f      /* the right column, from the card's left edge */
#define TEXT_W 468.0f
#define NBRAND 4

static int     s_open, s_dismissed;
static double  s_nopad_since = -1, s_cooldown, s_t0, s_done_t = -1;
static spring  s_fade, s_ok;   /* card in/out; 0..1 once paired */
static spring  s_step;         /* 0 = step 1 (plug in an adapter), 1 = step 2 (pair a controller) */
static spring  s_txt;          /* 0..1: the texts of the right column fading in after a change of step */
/* what the card shows: taken from the network state while the card is open and frozen when it
 * closes, so nothing changes under the fade-out (the worker resets its state at once) */
static int     v_step2, v_ap, v_paired, v_btup, v_key = -1;
static char    v_name[64];
static int     s_armed;        /* a "done" left over from the previous card is not this card's */
/* the illustrations (tools/make-illustrations.py, assets/ui/): white, tinted when drawn */
typedef struct { GLuint tex; float w, h; int tried; } ill_t;
static ill_t s_pad, s_recv;
static void ill_load(ill_t *il, const char *name)
{
   char p[400];
   SDL_Surface *sf;
   const int hd = g_ui_scale > 1.2f;
   if (il->tried) return;
   il->tried = 1;
   snprintf(p, sizeof(p), "%s/assets/ui/%s%s.png", g_home, name, hd ? "-1080" : "");
   if (!(sf = IMG_Load(p))) { logf_("padwait: no %s (shapes instead)", p); return; }
   il->tex = tex_from_surface(sf);
   il->w = sf->w / (hd ? 1.5f : 1.0f); il->h = sf->h / (hd ? 1.5f : 1.0f);
   SDL_FreeSurface(sf);
}
static void ill_free(ill_t *il) { if (il->tex) glDeleteTextures(1, &il->tex); memset(il, 0, sizeof(*il)); }

static label_t s_kick, s_title, s_sub, s_status, s_bname[NBRAND], s_bhow[NBRAND], s_stepl[2], s_stepn[2];

/* the controllers people pair most (names are product names, kept as they are) */
static const char *const k_brand[NBRAND] = { "DualSense", "DualShock 4", "Switch Pro Controller", "8BitDo" };
static const char *const k_how[NBRAND] = { "padwait.how_dualsense", "padwait.how_ds4", "padwait.how_switch", "padwait.how_8bitdo" };

static void col4(float o[4], int tc, float a) { theme_color(tc, o); o[3] = a; }
static void wrap_set(label_t *l, TTF_Font *f, const char *s, int wrap)
{
   size_t n = sizeof(l->key) - 1;
   int same = strlen(s) >= n ? !strncmp(l->key, s, n) : !strcmp(l->key, s);
   if (same && (l->tex || !*s)) return;
   label_free(l);
   snprintf(l->key, sizeof(l->key), "%s", s);
   if (*s) l->tex = text_tex_wrapped(f, s, wrap, &l->w, &l->h);
}
static void draw_label(const label_t *l, float x, float y, const float c[4])
{
   if (l->tex && c[3] > 0.003f) draw_2d(l->tex, floorf(x), floorf(y), l->w, l->h, c, 0);
}

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
      s_step.x = ns->bt_present ? 1.0f : 0.0f; s_step.v = 0;
      s_armed = ns->ap_state != NET_AP_DONE; v_key = -1;
      if (s_fade.x < 0.02f) { s_fade.x = 0; s_fade.v = 0; }
      music_sfx(SFX_OPEN);
      logf_("padwait: no controller (Bluetooth adapter %s)", ns->bt_present ? "present" : "absent");
   }
   if (s_open)
   {
      /* step 2 as soon as an adapter is plugged in: Bluetooth on, the search starts */
      if (ns->bt_present && !ns->bt_on) net_bt_set(1);
      int key;
      if (!s_armed && ns->ap_state != NET_AP_DONE) s_armed = 1;
      v_step2 = ns->bt_present; v_paired = ns->paired; v_btup = ns->bt == NET_CONNECTED;
      v_ap = !s_armed && ns->ap_state == NET_AP_DONE ? NET_AP_IDLE : ns->ap_state;
      if (v_ap == NET_AP_PAIRING || v_ap == NET_AP_DONE || v_ap == NET_AP_FAILED) snprintf(v_name, sizeof(v_name), "%s", ns->ap_name);
      if (v_ap == NET_AP_DONE && s_done_t < 0) { s_done_t = t; music_sfx(SFX_CONFIRM); }
      key = v_step2 * 2 + (s_done_t >= 0);
      if (key != v_key) { if (v_key >= 0) { s_txt.x = 0; s_txt.v = 0; } else s_txt.x = 1; v_key = key; }
      if (s_done_t >= 0 && (t - s_done_t >= 2.4 || (njoy > 0 && t - s_done_t >= 1.2))) close_card(t, 0);
      else if (njoy > 0 && s_done_t < 0) close_card(t, 0);         /* a wired one was plugged in */
      else if (nav && nav->back && t - s_t0 > 0.4) close_card(t, 1);
   }
   spring_step(&s_fade, s_open ? 1.0f : 0.0f, 120, 18, dt);
   spring_step(&s_ok, s_done_t >= 0 ? 1.0f : 0.0f, 140, 16, dt);
   spring_step(&s_step, v_step2 ? 1.0f : 0.0f, 120, 18, dt);
   spring_step(&s_txt, 1.0f, 150, 20, dt);
   if (!s_open && s_fade.x < 0.003f && s_title.tex)
   {
      label_t *all[] = { &s_kick, &s_title, &s_sub, &s_status };
      int i;
      for (i = 0; i < 4; i++) label_free(all[i]);
      for (i = 0; i < NBRAND; i++) { label_free(&s_bname[i]); label_free(&s_bhow[i]); }
      for (i = 0; i < 2; i++) { label_free(&s_stepl[i]); label_free(&s_stepn[i]); }
      ill_free(&s_pad); ill_free(&s_recv);
   }
}

/* a controller from rounded shapes, centred at (x, y), w wide */
static void draw_pad(float x, float y, float w, const float body[4], const float ink[4])
{
   const float h = w * 0.54f, l = x - w * 0.5f, top = y - h * 0.5f, r = h * 0.5f;
   const float s = w / 180.0f;
   draw_round_rect(l, top, w, h, r, body, 0.0f);                                  /* body */
   draw_round_rect(l + 12 * s, top + h * 0.42f, 46 * s, h * 0.70f, 23 * s, body, 0.0f);   /* grips */
   draw_round_rect(x + w * 0.5f - 58 * s, top + h * 0.42f, 46 * s, h * 0.70f, 23 * s, body, 0.0f);
   /* d-pad */
   draw_round_rect(l + 30 * s, y - 16 * s, 34 * s, 12 * s, 3 * s, ink, 0.0f);
   draw_round_rect(l + 41 * s, y - 27 * s, 12 * s, 34 * s, 3 * s, ink, 0.0f);
   /* face buttons */
   {
      const float bx = x + w * 0.5f - 47 * s, by = y - 10 * s, d = 12 * s, g = 14 * s;
      draw_round_rect(bx - d * 0.5f, by - g - d * 0.5f, d, d, d * 0.5f, ink, 0.0f);
      draw_round_rect(bx - d * 0.5f, by + g - d * 0.5f, d, d, d * 0.5f, ink, 0.0f);
      draw_round_rect(bx - g - d * 0.5f, by - d * 0.5f, d, d, d * 0.5f, ink, 0.0f);
      draw_round_rect(bx + g - d * 0.5f, by - d * 0.5f, d, d, d * 0.5f, ink, 0.0f);
   }
   /* sticks */
   draw_round_rect(x - 38 * s, y + 6 * s, 26 * s, 26 * s, 13 * s, ink, 0.0f);
   draw_round_rect(x + 12 * s, y + 6 * s, 26 * s, 26 * s, 13 * s, ink, 0.0f);
}

/* a USB Bluetooth receiver, upright, centred at (x, y): the plug on top, the body below */
static void draw_dongle(float x, float y, const float body[4], const float metal[4], const float ink[4])
{
   draw_round_rect(x - 22, y - 62, 44, 40, 4, metal, 0.0f);                /* plug */
   draw_round_rect(x - 12, y - 52, 9, 9, 1, ink, 0.0f);
   draw_round_rect(x + 3, y - 52, 9, 9, 1, ink, 0.0f);
   draw_round_rect(x - 34, y - 26, 68, 92, 16, body, 0.0f);                /* body */
   draw_round_rect(x - 5, y + 44, 10, 10, 5, ink, 0.0f);                   /* its light */
}

static void draw_dots(float x, float y, double t, const float c[4])
{
   int i;
   for (i = 0; i < 3; i++)
   {
      float ph = (float)fmod(t * 1.6 - i * 0.18, 1.0), a = 0.35f + 0.65f * (ph < 0.5f ? 1.0f - ph * 2.0f : 0.0f);
      const float d[4] = { c[0], c[1], c[2], c[3] * a };
      draw_round_rect(x + i * 14, y - 4, 8, 8, 4, d, 0.0f);
   }
}

void padwait_draw(double t)
{
   const float a = padwait_amount(), ok = clampf(s_ok.x, 0.0f, 1.0f);
   const float cx = (W - CW) * 0.5f, cy = (H - CH) * 0.5f - 8.0f + (1.0f - a) * 18.0f;
   const float dx = cx + 232.0f, dy = cy + CH * 0.5f - 18.0f;   /* the disc's centre */
   float txt[4], dim[4], acc[4], sep[4], ink[4], body[4];
   char buf[200];
   int i;
   if (a <= 0.003f) return;
   col4(txt, TC_TEXT, a); col4(dim, TC_DIM, a); col4(acc, TC_ACCENT, a); col4(sep, TC_TEXT, 0.08f * a);
   gl_use_2d();
   {  /* a screen of its own: the dashboard and its help bar go almost dark behind the card */
      const float bd[4] = { 0.02f, 0.02f, 0.03f, 0.97f * a };
      draw_2d(0, 0, 0, W, H, bd, 0);
   }
   ui_card(cx, cy, CW, CH, a);

   /* the disc, its ripples and the controller (step 2), or the receiver to plug in (step 1) */
   {
      const float st = clampf(s_step.x, 0.0f, 1.0f);
      const int searching = st > 0.5f && ok < 0.5f && v_ap != NET_AP_NOADAPTER;
      const float speed = v_ap == NET_AP_PAIRING ? 1.9f : 1.0f;
      if (searching)
         for (i = 0; i < 3; i++)
         {  /* rings leave the disc and fade 60 px out, one every 0.8 s */
            float ph = (float)fmod((t - s_t0) * 0.42 * speed + i / 3.0, 1.0), rr = DISC_R + 8.0f + ph * 62.0f;
            ui_ring(dx - rr, dy - rr, rr * 2, rr * 2, rr, 0.0f, a * 0.55f * (1.0f - ph) * (1.0f - ph));
         }
      ui_rest(dx - DISC_R, dy - DISC_R, DISC_R * 2, DISC_R * 2, DISC_R, a);
      if (ok > 0.01f) ui_fill(dx - DISC_R, dy - DISC_R, DISC_R * 2, DISC_R * 2, DISC_R, acc, a * 0.38f * ok);
      for (i = 0; i < 3; i++) body[i] = txt[i] + (acc[i] - txt[i]) * ok;
      body[3] = a * 0.94f;
      ink[0] = 0.10f; ink[1] = 0.11f; ink[2] = 0.14f; ink[3] = a * 0.90f * st;
      body[3] = a * 0.94f * st;
      ill_load(&s_pad, "pad"); ill_load(&s_recv, "receiver");
      if (st > 0.01f)
      {
         const float py = dy - 4.0f + (1.0f - st) * 14.0f;
         if (s_pad.tex)
         {  /* in colour, as drawn: the disc behind turns the accent once paired */
            const float w4[4] = { 1, 1, 1, a * st };
            const float pw = s_pad.w * 0.86f, ph = s_pad.h * 0.86f;   /* air between the drawing and the disc's edge */
            draw_2d(s_pad.tex, dx - pw * 0.5f, py - ph * 0.5f, pw, ph, w4, 0);
         }
         else draw_pad(dx, py, 150.0f, body, ink);
      }
      if (st < 0.99f)
      {  /* the receiver, bobbing gently, as if going into a port */
         const float k = 1.0f - st, bob = (float)sin((t - s_t0) * 2.2) * 5.0f;
         float b[4], m[4], n[4];
         col4(b, TC_TEXT, a * 0.94f * k); col4(m, TC_DIM, a * 0.9f * k);
         n[0] = 0.10f; n[1] = 0.11f; n[2] = 0.14f; n[3] = a * 0.9f * k;
         if (s_recv.tex)
         {
            const float w4[4] = { 1, 1, 1, a * k };
            draw_2d(s_recv.tex, dx - s_recv.w * 0.5f, dy + bob - st * 14.0f - s_recv.h * 0.5f, s_recv.w, s_recv.h, w4, 0);
         }
         else draw_dongle(dx, dy + bob - st * 14.0f, b, m, n);
      }
   }
   draw_2d(0, cx + TEXT_X - 34.0f, cy + 40.0f, 1, CH - 132.0f, sep, 0);

   /* the right column */
   {
      const float x = cx + TEXT_X, ta = clampf(s_txt.x, 0.0f, 1.0f), rise = (1.0f - ta) * 8.0f;
      const int step2 = v_step2;
      float y = cy + 40.0f;
      float ttxt[4], tdim[4];
      col4(ttxt, TC_TEXT, a * ta); col4(tdim, TC_DIM, a * ta);
      ui_kicker(&s_kick, tr("net.bt"), x, y, a);
      y += 22.0f;
      wrap_set(&s_title, g_font_title, tr(ok > 0.5f ? "padwait.title_done" : step2 ? "padwait.title" : "padwait.title_plug"), (int)TEXT_W);
      draw_label(&s_title, x, y + rise, ttxt);
      y += s_title.h + 10.0f;
      wrap_set(&s_sub, g_font_small, tr(!step2 ? "padwait.sub_plug" : v_paired > 0 ? "padwait.sub_paired" : "padwait.sub"), (int)TEXT_W);
      draw_label(&s_sub, x, y + rise, tdim);
      y += s_sub.h + 22.0f;
      for (i = 0; i < 2; i++)
      {  /* the steps: done (accent, filled), current (ring), next (dim) */
         const int done = i == 0 ? step2 : ok > 0.5f, cur = i == (step2 ? 1 : 0) && !done;
         const float d = 26.0f;
         float fc[4];
         char n[4];
         snprintf(n, sizeof(n), "%d", i + 1);
         label_set(&s_stepn[i], g_font_small, n);
         label_set(&s_stepl[i], g_font_sub, tr(i == 0 ? "padwait.step1" : "padwait.step2"));
         if (done) draw_round_rect(x, y, d, d, d * 0.5f, acc, 0.0f);
         else if (cur) { ui_ring(x, y, d, d, d * 0.5f, 0.0f, a); }
         else { col4(fc, TC_TEXT, a * 0.10f); draw_round_rect(x, y, d, d, d * 0.5f, fc, 0.0f); }
         col4(fc, TC_TEXT, a * (done || cur ? 1.0f : 0.5f));
         if (done) { fc[0] = 0.08f; fc[1] = 0.09f; fc[2] = 0.11f; }   /* dark on the accent */
         draw_label(&s_stepn[i], x + (d - s_stepn[i].w) * 0.5f, y + (d - s_stepn[i].h) * 0.5f, fc);
         col4(fc, TC_TEXT, a * (done || cur ? 1.0f : 0.5f));
         draw_label(&s_stepl[i], x + d + 14.0f, y + (d - s_stepl[i].h) * 0.5f, fc);
         y += d + 12.0f;
      }
      y += 10.0f;
      if (step2)
      {
         float nx = 0.0f;   /* the names' column: as wide as the longest name */
         for (i = 0; i < NBRAND; i++)
         {
            label_set(&s_bname[i], g_font_small, k_brand[i]);
            if (s_bname[i].w > nx) nx = s_bname[i].w;
         }
         nx += 24.0f;
         for (i = 0; i < NBRAND; i++)
         {  /* each controller on one line: its name, then how it enters pairing mode */
            float rh;
            wrap_set(&s_bhow[i], g_font_small, tr(k_how[i]), (int)(TEXT_W - nx));
            draw_label(&s_bname[i], x, y + rise, ttxt);
            draw_label(&s_bhow[i], x + nx, y + rise, tdim);
            rh = s_bhow[i].h > s_bname[i].h ? s_bhow[i].h : s_bname[i].h;
            y += rh + 8.0f;
         }
      }
   }

   /* the status, at the foot */
   {
      const float fy = cy + CH - 64.0f;
      const char *st;
      const float *sc = dim;
      draw_2d(0, cx + 32.0f, fy - 18.0f, CW - 64.0f, 1, sep, 0);
      if (!v_step2) { st = tr("padwait.status_plug"); }
      else switch (v_ap)
      {
      case NET_AP_PAIRING:   st = trf(buf, sizeof(buf), "padwait.pairing", "name", v_name, NULL); sc = txt; break;
      case NET_AP_DONE:      st = trf(buf, sizeof(buf), "padwait.done", "name", v_name, NULL); sc = acc; break;
      case NET_AP_FAILED:    st = trf(buf, sizeof(buf), "padwait.failed", "name", v_name, NULL); break;
      case NET_AP_NOADAPTER: st = tr("padwait.noadapter"); break;
      case NET_AP_SEARCHING: st = tr("padwait.searching"); break;
      default:               st = tr(v_btup ? "padwait.searching" : "padwait.starting"); break;
      }
      wrap_set(&s_status, g_font_sub, st, (int)(CW - 140.0f));
      if (v_step2 && v_ap == NET_AP_DONE) draw_round_rect(cx + 40.0f, fy + s_status.h * 0.5f - 6.0f, 12, 12, 6, acc, 0.0f);
      else if (v_ap != NET_AP_NOADAPTER) draw_dots(cx + 36.0f, fy + s_status.h * 0.5f, t, sc);
      draw_label(&s_status, cx + 90.0f, fy, sc);
   }
}
