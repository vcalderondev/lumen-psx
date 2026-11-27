/* Connections cards, see conn.h.
 *
 * One card at a time (Wi-Fi or Bluetooth), the library check's geometry: a list on the
 * left (value column in words: the connected entry in the accent with the check), the
 * pane right of the rule explains the highlighted entry. For the password keyboard and the
 * "forget?" question the card steps aside: it closes, asks the panel (open underneath) for
 * the modal, and comes back by itself once the modal is gone. The password is handed to
 * net.c straight away and wiped here; net.sh keeps only its WPA hash. */
#include <stdio.h>
#include <string.h>
#include "lumen.h"
#include "conn.h"
#include "i18n.h"
#include "music.h"
#include "net.h"
#include "panel.h"
#include "theme.h"
#include "ui.h"

#define CW     940.0f
#define CH     520.0f
#define PADX   28.0f
#define LIST_X 14.0f
#define LIST_Y 110.0f
#define LIST_W 560.0f
#define ROW    46.0f
#define VIS    8
#define NMAX   32

enum { K_NONE, K_WIFI, K_BT };
static int     s_kind, s_open, s_away, s_sel, s_top;
static int     s_bt_add;                               /* Bluetooth: 1 the search (not paired), 0 the paired devices */
static int     s_map[NET_MAX_DEVS], s_nmap;            /* the Bluetooth entries shown: indexes into net_state()->dev */
static double  s_t0;
static spring  s_fade, s_pill, s_scroll;
static label_t s_kick, s_title, s_cap, s_foot, s_name, s_line, s_note, s_empty;
static label_t s_rl[NMAX], s_rv[NMAX];
static char    s_key_l[NMAX][80], s_key_v[NMAX][80];   /* what each row label holds */
static char    s_pass_ssid[64];                        /* the network the keyboard is for */
static char    s_pass[72];                             /* typed password, until handed over */
static int     s_pass_ready;
static char    s_target[64];                           /* ssid / mac of the pending question */
static char    s_sel_key[64];                          /* the highlighted entry: kept across re-sorts */
static char    s_recent[18];                           /* the search: the device chosen there stays listed once paired */
static int     s_sel_gen = -1;

static const net_dev_t *dev_at(int i) { return &net_state()->dev[s_map[i < s_nmap ? i : 0]]; }
static const char *entry_key(int i)
{
   const net_state_t *ns = net_state();
   if (s_kind == K_WIFI) return i < ns->naps ? ns->aps[i].ssid : "";
   return i < s_nmap ? dev_at(i)->mac : "";
}
/* what kind of device, from BlueZ's icon: a tr key */
static const char *dev_kind(const net_dev_t *d)
{
   if (!strcmp(d->icon, "input-gaming")) return "net.kind_pad";
   if (!strcmp(d->icon, "input-keyboard")) return "net.kind_keyboard";
   if (!strcmp(d->icon, "input-mouse") || !strcmp(d->icon, "input-tablet")) return "net.kind_mouse";
   if (!strncmp(d->icon, "audio-", 6) || !strcmp(d->icon, "multimedia-player")) return "net.kind_audio";
   if (!strcmp(d->icon, "phone")) return "net.kind_phone";
   return "net.kind_other";
}
static int dev_audio(const net_dev_t *d) { return !strcmp(dev_kind(d), "net.kind_audio"); }

static void col4(float o[4], int tc, float a) { theme_color(tc, o); o[3] = a; }
static void draw_label(const label_t *l, float x, float y, const float c[4])
{
   if (l->tex && c[3] > 0.003f) draw_2d(l->tex, floorf(x), floorf(y), l->w, l->h, c, 0);
}
static void wrap_set(label_t *l, TTF_Font *f, const char *s, int wrap)
{
   size_t n = sizeof(l->key) - 1;
   int same = strlen(s) >= n ? !strncmp(l->key, s, n) : !strcmp(l->key, s);
   if (same && (l->tex || !*s)) return;
   label_free(l);
   snprintf(l->key, sizeof(l->key), "%s", s);
   if (*s) l->tex = text_tex_wrapped(f, s, wrap, &l->w, &l->h);
}

static int count(void)
{
   const net_state_t *ns = net_state();
   if (s_kind == K_BT)
   {  /* the paired devices, or (the search) the ones that are not */
      int i;
      s_nmap = 0;
      for (i = 0; i < ns->ndev; i++)
         if ((ns->dev[i].paired == 0) == s_bt_add || (s_bt_add && s_recent[0] && !strcmp(ns->dev[i].mac, s_recent)))
            s_map[s_nmap++] = i;
      return s_nmap;
   }
   return s_kind == K_WIFI ? ns->naps : 0;
}

static void open_kind(int kind)
{
   if (s_open) return;
   s_kind = kind; s_open = 1; s_away = 0; s_sel = s_top = 0; s_t0 = now_s(); s_sel_key[0] = 0; s_recent[0] = 0;
   s_pill.x = s_pill.v = s_scroll.x = s_scroll.v = 0;
   if (s_fade.x < 0.02f) { s_fade.x = 0; s_fade.v = 0; }
   if (kind == K_WIFI) net_wifi_scan();
   music_sfx(SFX_OPEN);
}
void conn_open_wifi(void) { open_kind(K_WIFI); }
void conn_open_bt(void) { s_bt_add = 0; open_kind(K_BT); }
void conn_open_bt_add(void) { s_bt_add = 1; open_kind(K_BT); }
int   conn_active(void) { return s_open || s_away || s_fade.x > 0.01f; }
int   conn_open_now(void) { return s_open; }
int   conn_wifi_shown(void) { return (s_open || s_away) && s_kind == K_WIFI; }
int   conn_bt_shown(void) { return (s_open || s_away) && s_kind == K_BT; }
float conn_amount(void) { return clampf(s_fade.x, 0.0f, 1.0f); }
int   conn_hint_set(void) { return s_kind == K_BT ? HINTS_BT : HINTS_WIFI; }

static void step_aside(void) { s_open = 0; s_away = 1; }

/* keyboard: 8..63 characters (WPA); kept only until net.c has it */
static const char *accept_pass(const char *text)
{
   size_t n = strlen(text);
   if (n < 8 || n > 63) return "net.pass_short";
   snprintf(s_pass, sizeof(s_pass), "%s", text);
   s_pass_ready = 1;
   return NULL;
}
static void forget_yes(void) { net_wifi_forget(s_target); }
static void remove_yes(void) { net_bt_remove(s_target); }

static void wifi_confirm(void)
{
   const net_state_t *ns = net_state();
   const net_ap_t *ap;
   char t[160];
   if (s_sel >= ns->naps) return;
   ap = &ns->aps[s_sel];
   if (ns->busy) { panel_toast("net.busy"); return; }
   if (ap->connected) { panel_toast("net.wifi_already"); return; }
   if (ap->saved) { net_wifi_select(ap->ssid); return; }
   if (!strcmp(ap->sec, "open")) { net_wifi_connect(ap->ssid, ""); return; }
   if (!strcmp(ap->sec, "wep")) { panel_toast("net.wifi_wep"); return; }
   snprintf(s_pass_ssid, sizeof(s_pass_ssid), "%s", ap->ssid);
   s_pass_ready = 0;
   step_aside();
   panel_keyboard(trf(t, sizeof(t), "net.pass_title", "ssid", ap->ssid, NULL), 63, 1, accept_pass);
}

void conn_update(const ui_nav_t *nav, double t, float dt)
{
   const net_state_t *ns = net_state();
   int before = s_sel, n = count();
   spring_step(&s_fade, s_open ? 1.0f : 0.0f, 120, 18, dt);
   if (s_away && !panel_modal_open())
   {  /* the keyboard / question is gone: hand the password over, come back */
      if (s_pass_ready) { net_wifi_connect(s_pass_ssid, s_pass); memset(s_pass, 0, sizeof(s_pass)); s_pass_ready = 0; }
      s_away = 0; s_open = 1; s_t0 = t;
   }
   if (!s_open)
   {
      if (!s_away && s_fade.x <= 0.01f && s_title.tex)
      {
         label_t *all[] = { &s_kick, &s_title, &s_cap, &s_foot, &s_name, &s_line, &s_note, &s_empty };
         int i;
         for (i = 0; i < (int)(sizeof(all) / sizeof(all[0])); i++) label_free(all[i]);
         for (i = 0; i < NMAX; i++) { label_free(&s_rl[i]); label_free(&s_rv[i]); s_key_l[i][0] = s_key_v[i][0] = 0; }
         s_kind = K_NONE;
      }
      return;
   }
   if (ns->gen != s_sel_gen && s_sel_key[0])
   {  /* the list changed (a scan, a connection): keep the same entry highlighted */
      int i;
      for (i = 0; i < n; i++) if (!strcmp(entry_key(i), s_sel_key)) { s_sel = i; break; }
   }
   s_sel_gen = ns->gen;
   if (s_sel >= n) s_sel = n > 0 ? n - 1 : 0;
   if (nav && t - s_t0 >= 0.15)
   {
      if (s_kind == K_WIFI && nav->erase) { net_wifi_scan(); music_sfx(SFX_CONFIRM); }
      else if (nav->back) { s_open = 0; music_sfx(SFX_BACK); return; }
      else if (n)
      {
         if (nav->up && s_sel > 0) s_sel--;
         if (nav->down && s_sel < n - 1) s_sel++;
         if (nav->l1) s_sel = s_sel - VIS < 0 ? 0 : s_sel - VIS;
         if (nav->r1) s_sel = s_sel + VIS > n - 1 ? n - 1 : s_sel + VIS;
         if (s_sel != before) music_sfx(SFX_FOCUS);
         if (nav->confirm)
         {
            music_sfx(SFX_CONFIRM);
            if (s_kind == K_WIFI) wifi_confirm();
            else
            {
               /* the row itself shows "Pairing…" from this press on (net.h pair_mac): a
                * second press on it says nothing, one on another device says to wait */
               const net_dev_t *d = dev_at(s_sel);
               if (ns->pair_mac[0] || ns->busy) { if (strcmp(ns->pair_mac, d->mac)) panel_toast("net.busy"); }
               else if (d->connected) panel_toast("net.bt_already");
               else
               {
                  if (s_bt_add) snprintf(s_recent, sizeof(s_recent), "%s", d->mac);
                  net_bt_pair(d->mac);
               }
            }
         }
         else if (nav->options)
         {  /* Triangle: forget a saved network / a paired controller, after asking */
            char m[200];
            if (s_kind == K_WIFI && ns->aps[s_sel].saved)
            {
               snprintf(s_target, sizeof(s_target), "%s", ns->aps[s_sel].ssid);
               step_aside();
               panel_confirm(trf(m, sizeof(m), "net.forget_confirm", "name", s_target, NULL), "net.forget", forget_yes);
            }
            else if (s_kind == K_BT && dev_at(s_sel)->paired)
            {
               snprintf(s_target, sizeof(s_target), "%s", dev_at(s_sel)->mac);
               step_aside();
               panel_confirm(trf(m, sizeof(m), "net.forget_confirm", "name", dev_at(s_sel)->name, NULL), "net.forget", remove_yes);
            }
            else panel_toast(s_kind == K_WIFI ? "net.forget_none" : "net.bt_forget_none");
         }
      }
   }
   snprintf(s_sel_key, sizeof(s_sel_key), "%s", n ? entry_key(s_sel) : "");
   if (s_sel < s_top) s_top = s_sel;
   if (s_sel > s_top + VIS - 1) s_top = s_sel - VIS + 1;
   spring_step(&s_scroll, s_top * ROW, 240, 26, dt);
   spring_step(&s_pill, s_sel * ROW, 260, 26, dt);
}

static const char *strength(int dbm)
{
   return dbm >= -55 ? "net.sig_excellent" : dbm >= -67 ? "net.sig_good" : dbm >= -75 ? "net.sig_fair" : "net.sig_weak";
}

static void draw_dots(float cx, float cy, double t, const float c[4])
{
   int i;
   for (i = 0; i < 3; i++)
   {
      float ph = (float)fmod(t * 1.6 - i * 0.18, 1.0), a = 0.35f + 0.65f * (ph < 0.5f ? 1.0f - ph * 2.0f : 0.0f);
      const float d[4] = { c[0], c[1], c[2], c[3] * a };
      draw_round_rect(cx - 19 + i * 15, cy - 4, 8, 8, 4, d, 0.0f);
   }
}

/* one row's texts: label and value, rendered again only when they change */
static void row_texts(int i, const char *l, const char *v)
{
   char buf[160];
   int k = i % NMAX;
   if (strcmp(s_key_l[k], l)) { label_set(&s_rl[k], g_font_sub, ui_fit(g_font_sub, l, LIST_W - 250, buf, sizeof(buf))); snprintf(s_key_l[k], sizeof(s_key_l[k]), "%s", l); }
   if (strcmp(s_key_v[k], v)) { label_set(&s_rv[k], g_font_sub, ui_fit(g_font_sub, v, 230, buf, sizeof(buf))); snprintf(s_key_v[k], sizeof(s_key_v[k]), "%s", v); }
}

void conn_draw(double t)
{
   const net_state_t *ns = net_state();
   const float a = conn_amount(), cx = (W - CW) * 0.5f, cy = 112.0f + (1 - a) * 16;
   const float lx = cx + LIST_X, ly = cy + LIST_Y, lh = VIS * ROW, dx = lx + LIST_W + 42, dw = CW - LIST_X - LIST_W - 42 - PADX;
   float txt[4], dim[4], acc[4], sep[4];
   char buf[400], n1[16], n2[16];
   const int wifi = s_kind == K_WIFI, n = count();
   int i;
   if (a <= 0.003f || s_kind == K_NONE) return;
   col4(txt, TC_TEXT, a); col4(dim, TC_DIM, a); col4(acc, TC_ACCENT, a); col4(sep, TC_TEXT, 0.08f * a);
   gl_use_2d();
   ui_scrim(a * (1.0f - panel_amount()));
   ui_card(cx, cy, CW, CH, a);
   ui_kicker(&s_kick, tr(wifi ? "net.wifi" : "net.bt"), cx + PADX, cy + 24, a);
   label_set(&s_title, g_font_title, tr(wifi ? "net.networks" : s_bt_add ? "net.bt_add" : "net.devices"));
   draw_label(&s_title, cx + PADX, cy + 42, txt);
   /* top right: what is going on */
   {
      const int pairing = !wifi && ns->pair_mac[0], working = ns->busy || pairing || (wifi ? ns->scanning : ns->bt_scanning);
      if (ns->busy || pairing) label_set(&s_cap, g_font_small, tr(wifi ? "net.connecting" : "net.pairing"));
      else if (working) label_set(&s_cap, g_font_small, tr("net.searching"));
      else { snprintf(n1, sizeof(n1), "%d", n); label_set(&s_cap, g_font_small, trf(buf, sizeof(buf), wifi ? "net.n_networks" : "net.n_devices", "n", n1, NULL)); }
      draw_label(&s_cap, cx + CW - PADX - s_cap.w, cy + 58, working ? acc : dim);
      if (working) draw_dots(cx + CW - PADX - s_cap.w - 34, cy + 58 + s_cap.h * 0.5f, t, acc);   /* it is alive */
   }
   draw_2d(0, cx + PADX, cy + 96, CW - 2 * PADX, 1, sep, 0);

   if (!n)
   {  /* nothing (yet): the search, or what to do */
      if (wifi ? (ns->scanning || (ns->naps == 0 && now_s() - ns->scan_age > 30)) : ns->bt_scanning)
         draw_dots(cx + CW * 0.5f, ly + lh * 0.5f - 30, t, dim);
      wrap_set(&s_empty, g_font_small, tr(wifi ? (ns->scanning ? "net.searching_wifi" : "net.none_found") : s_bt_add ? "net.bt_none" : "net.bt_none_paired"), (int)(CW - 2 * PADX - 200));
      draw_label(&s_empty, cx + (CW - s_empty.w) * 0.5f, ly + lh * 0.5f, dim);
      if (!wifi && s_bt_add)
      {
         wrap_set(&s_note, g_font_small, tr("net.bt_howto"), (int)(CW - 2 * PADX - 200));
         draw_label(&s_note, cx + (CW - s_note.w) * 0.5f, ly + lh * 0.5f + s_empty.h + 18, txt);
      }
      return;
   }

   glEnable(GL_SCISSOR_TEST);
   gl_scissor(lx - 8, ly - 8, LIST_W + 16, lh + 16);
   ui_focus(lx, ly + s_pill.x - s_scroll.x + 2, LIST_W, ROW - 4, (ROW - 4) * 0.5f, a);
   for (i = (int)(s_scroll.x / ROW) - 1; i < n; i++)
   {
      float y, c[4], vx;
      int k, on, busy_row = 0;
      if (i < 0) continue;
      y = ly + i * ROW - s_scroll.x;
      if (y >= ly + lh - 1) break;
      if (y + ROW <= ly + 1) continue;
      k = i % NMAX;
      if (wifi)
      {
         const net_ap_t *ap = &ns->aps[i];
         on = ap->connected;
         if (on) snprintf(buf, sizeof(buf), "%s", tr("net.connected"));
         else if (ap->saved) snprintf(buf, sizeof(buf), "%s", tr("net.saved"));
         else snprintf(buf, sizeof(buf), "%s  \xC2\xB7  %s", tr(strength(ap->signal)), tr(!strcmp(ap->sec, "open") ? "net.open" : "net.protected"));
         row_texts(i, ap->ssid, buf);
      }
      else
      {
         const net_dev_t *d = dev_at(i);
         on = d->connected;
         busy_row = !strcmp(d->mac, ns->pair_mac);
         row_texts(i, d->name, tr(busy_row ? "net.pairing" : on ? "net.bt_connected" : d->paired ? "net.bt_paired_s" : dev_kind(d)));
         if (busy_row) on = 0;
      }
      memcpy(c, txt, sizeof(c));
      if (i != s_sel) c[3] *= 0.86f;
      draw_label(&s_rl[k], lx + 20, y + (ROW - s_rl[k].h) * 0.5f, c);
      vx = lx + LIST_W - 20 - s_rv[k].w;
      draw_label(&s_rv[k], vx, y + (ROW - s_rv[k].h) * 0.5f, on || busy_row ? acc : dim);
      if (on) draw_round_rect(vx - 24, y + ROW * 0.5f - 6, 12, 12, 6, acc, 0.0f);
      if (busy_row) draw_dots(vx - 34, y + ROW * 0.5f, t, acc);   /* this one is being paired */
   }
   glDisable(GL_SCISSOR_TEST);
   if (n > VIS)
   {
      float th = lh * VIS / (float)n, ty = ly + (lh - th) * clampf(s_scroll.x / ((n - VIS) * ROW), 0.0f, 1.0f);
      float tc[4]; col4(tc, TC_TEXT, 0.22f * a);
      draw_round_rect(lx + LIST_W + 6, ty, 4, th, 2, tc, 0.0f);
   }
   draw_2d(0, lx + LIST_W + 22, ly, 1, lh, sep, 0);
   {  /* the pane: the highlighted entry, and what Cross will do */
      float y = ly + 8;
      const char *note;
      if (wifi)
      {
         const net_ap_t *ap = &ns->aps[s_sel < n ? s_sel : 0];
         wrap_set(&s_name, g_font_medium, ap->ssid, (int)dw);
         snprintf(buf, sizeof(buf), "%s  \xC2\xB7  %s", tr(strength(ap->signal)),
                  !strcmp(ap->sec, "open") ? tr("net.open") : !strcmp(ap->sec, "wpa3") ? "WPA2 / WPA3" : !strcmp(ap->sec, "wep") ? "WEP" : "WPA2");
         note = ap->connected ? trf(buf + 200, 200, "net.note_connected", "ip", ns->ip, NULL) :
                ap->saved ? tr("net.note_saved") : !strcmp(ap->sec, "open") ? tr("net.note_open") :
                !strcmp(ap->sec, "wep") ? tr("net.wifi_wep") : tr("net.note_protected");
      }
      else
      {
         const net_dev_t *d = dev_at(s_sel < n ? s_sel : 0);
         wrap_set(&s_name, g_font_medium, d->name, (int)dw);
         snprintf(buf, sizeof(buf), "%s  \xC2\xB7  %s", tr(dev_kind(d)), d->mac);
         note = dev_audio(d) ? tr("net.note_bt_audio") : d->paired ? tr(d->connected ? "net.note_bt_connected" : "net.note_bt_paired") : tr("net.note_bt_new");
      }
      draw_label(&s_name, dx, y, txt);
      y += s_name.h + 12;
      wrap_set(&s_line, g_font_small, buf, (int)dw);
      draw_label(&s_line, dx, y, txt);
      if (buf[0]) y += s_line.h + 16;
      wrap_set(&s_note, g_font_small, note, (int)dw);
      draw_label(&s_note, dx, y, dim);
   }
   if (wifi)
   {
      snprintf(n1, sizeof(n1), "%d", (int)(now_s() - ns->scan_age));
      label_set(&s_foot, g_font_small, ns->scanning ? "" : trf(buf, sizeof(buf), "net.scanned", "n", n1, NULL));
   }
   else
   {
      snprintf(n1, sizeof(n1), "%d", n); snprintf(n2, sizeof(n2), "%d", ns->connected);
      label_set(&s_foot, g_font_small, trf(buf, sizeof(buf), s_bt_add ? "net.bt_found_n" : "net.paired_n", "n", n1, "c", n2, NULL));
   }
   draw_label(&s_foot, cx + PADX, cy + CH - 14 - s_foot.h, dim);
}
