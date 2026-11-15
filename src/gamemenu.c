/* Game menu and resume question, see gamemenu.h. */
#include <stdio.h>
#include <string.h>
#include "lumen.h"
#include "gamemenu.h"
#include "collections.h"
#include "i18n.h"
#include "panel.h"
#include "theme.h"

#define CW      600.0f   /* card width                 */
#define HEAD    136.0f   /* header: cover, kicker, title */
#define RH      54.0f    /* main row height            */
#define SH      46.0f    /* folder row height (dropped) */
#define PADX    28.0f
#define MAXR    (6 + COLL_MAX_FOLDERS + 1)

enum { R_PLAY, R_FAV, R_FOLDERS, R_FOLDER, R_NEWFOLDER, R_RESUME, R_RESTART };
typedef struct { int type, folder, sub; } mrow_t;

static int     s_open, s_mode, s_gi = -1, s_result, s_sel, s_expanded;
static char    s_content[512], s_title[128];
static spring  s_fade, s_exp, s_pill;
static double  s_t0;
static float   s_dt = 1.0f / 60.0f;   /* last frame time: the pill spring steps in draw */
static mrow_t  s_rows[MAXR];
static int     s_n;
static label_t s_kicker, s_ltitle, s_rl[MAXR], s_rv[MAXR];

static void col4(float o[4], int tc, float a) { theme_color(tc, o); o[3] = a; }

static void rows_build(void)
{
   int f;
   s_n = 0;
   if (s_mode == 1)
   {
      s_rows[s_n++] = (mrow_t){ R_RESUME, -1, 0 };
      s_rows[s_n++] = (mrow_t){ R_RESTART, -1, 0 };
      return;
   }
   s_rows[s_n++] = (mrow_t){ R_PLAY, -1, 0 };
   s_rows[s_n++] = (mrow_t){ R_FAV, -1, 0 };
   s_rows[s_n++] = (mrow_t){ R_FOLDERS, -1, 0 };
   for (f = 0; f < coll_nfolders() && s_n < MAXR - 1; f++) s_rows[s_n++] = (mrow_t){ R_FOLDER, f, 1 };
   s_rows[s_n++] = (mrow_t){ R_NEWFOLDER, -1, 1 };
}

static int navigable(int i) { return i >= 0 && i < s_n && (!s_rows[i].sub || s_expanded); }

static void open_common(int gi, int mode)
{
   if (gi < 0 || gi >= g_ngames) return;
   s_gi = gi; s_mode = mode; s_open = 1; s_result = GM_NONE; s_sel = 0; s_expanded = 0;
   s_exp.x = s_exp.v = 0;
   snprintf(s_content, sizeof(s_content), "%s", g_games[gi].content);
   snprintf(s_title, sizeof(s_title), "%s", g_games[gi].title);
   if (s_fade.x < 0.02f) { s_fade.x = 0; s_fade.v = 0; }
   s_t0 = now_s();
   rows_build();
   logf_("game menu: %s (%s)", mode ? "resume" : "options", s_content);
}

void gamemenu_open(int gi) { open_common(gi, 0); }
void gamemenu_resume_open(int gi) { open_common(gi, 1); }
int  gamemenu_active(void) { return s_open || s_fade.x > 0.01f; }
int  gamemenu_open_now(void) { return s_open; }
float gamemenu_amount(void) { return clampf(s_fade.x, 0, 1); }
int  gamemenu_folders_open(void) { return s_open && s_expanded; }
int  gamemenu_take(void) { int r = s_result; s_result = GM_NONE; return r; }

static void close_menu(int result) { s_open = 0; s_result = result; }

static const char *accept_new_folder(const char *name)
{
   int rc = coll_create_folder(name, s_content);
   if (rc == -2) return "gm.folder_taken";
   if (rc == -1) return "gm.folder_full";
   if (rc == -3) return "profile.name_empty";
   panel_toast("gm.folder_created");
   rows_build();
   return NULL;
}

static void activate(const mrow_t *r)
{
   switch (r->type)
   {
   case R_PLAY:    close_menu(GM_PLAY); break;
   case R_RESUME:  close_menu(GM_PLAY_RESUME); break;
   case R_RESTART: resume_forget(s_content); close_menu(GM_PLAY); break;
   case R_FAV:
      coll_toggle_fav(s_content);
      panel_toast(coll_is_fav(s_content) ? "gm.fav_added" : "gm.fav_removed");
      break;
   case R_FOLDERS:
      s_expanded = !s_expanded;
      if (s_expanded && s_sel + 1 < s_n && s_rows[s_sel + 1].sub) s_sel++;
      break;
   case R_FOLDER:
   {
      int was = coll_in_folder(r->folder, s_content), last = was && coll_folder_count(r->folder) == 1;
      coll_toggle_folder(r->folder, s_content);
      panel_toast(was ? (last ? "gm.folder_removed_last" : "gm.folder_out") : "gm.folder_in");
      rows_build();   /* a folder left empty is gone */
      if (s_sel >= s_n) s_sel = s_n - 1;
      break;
   }
   case R_NEWFOLDER: panel_osk("osk.folder_name", "", accept_new_folder); break;
   }
}

void gamemenu_update(const ui_nav_t *nav, double t, float dt)
{
   s_dt = dt > 0.0f ? dt : s_dt;
   spring_step(&s_fade, s_open ? 1.0f : 0.0f, 120, 18, dt);
   spring_step(&s_exp, s_expanded ? 1.0f : 0.0f, 200, 22, dt);
   if (!s_open || !nav || t - s_t0 < 0.15) return;   /* the Start that opened it */
   if (nav->up)   { int i = s_sel - 1; while (i >= 0 && !navigable(i)) i--; if (i >= 0) s_sel = i; }
   if (nav->down) { int i = s_sel + 1; while (i < s_n && !navigable(i)) i++; if (i < s_n) s_sel = i; }
   if (s_sel < s_n && s_rows[s_sel].type == R_FOLDERS && nav->right && !s_expanded) { s_expanded = 1; return; }
   if (nav->left && s_expanded && s_sel < s_n && (s_rows[s_sel].sub || s_rows[s_sel].type == R_FOLDERS))
   { s_expanded = 0; while (s_sel > 0 && s_rows[s_sel].sub) s_sel--; return; }
   if (nav->confirm && s_sel < s_n) activate(&s_rows[s_sel]);
   else if (nav->back || nav->start || nav->options)
   {
      if (s_expanded && nav->back && !nav->start) { s_expanded = 0; while (s_sel > 0 && s_rows[s_sel].sub) s_sel--; }
      else close_menu(GM_NONE);
   }
}

static const char *row_text(const mrow_t *r, const char **value)
{
   static char buf[96];
   *value = NULL;
   switch (r->type)
   {
   case R_PLAY:      return tr("gm.play");
   case R_FAV:       return tr(coll_is_fav(s_content) ? "gm.fav_remove" : "gm.fav_add");
   case R_FOLDERS:
      if (coll_nfolders()) { snprintf(buf, sizeof(buf), "%d", coll_nfolders()); *value = buf; }
      return tr("gm.folders");
   case R_FOLDER:    return coll_folder_name(r->folder);
   case R_NEWFOLDER: return tr("gm.folder_new");
   case R_RESUME:    return tr("gm.resume_continue");
   case R_RESTART:   return tr("gm.resume_restart");
   }
   return "";
}

void gamemenu_draw(double t)
{
   float a = gamemenu_amount(), ex = clampf(s_exp.x, 0, 1);
   float rowy[MAXR], rowh[MAXR], y, chgt, cx, cy, sub_h = 0;
   float txt[4], dim[4], acc[4], pill[4], card[4], back[4];
   const game_t *g;
   int i;
   (void)t;
   if (a <= 0.003f || s_gi < 0 || s_gi >= g_ngames) return;
   g = &g_games[s_gi];
   col4(txt, TC_TEXT, a); col4(dim, TC_DIM, a); col4(acc, TC_ACCENT, a);
   col4(pill, TC_PILL, a); col4(card, TC_CARD, theme_card_alpha() * a); col4(back, TC_BACKDROP, 0.62f * a);

   for (i = 0; i < s_n; i++) if (s_rows[i].sub) sub_h += SH;
   chgt = HEAD + (s_n - (s_mode ? 0 : (s_n - 3))) * RH + sub_h * ex + 24;
   if (s_mode == 1) chgt = HEAD + s_n * RH + 24;
   cx = (W - CW) * 0.5f;
   cy = (H - (HEAD + 3 * RH + 24)) * 0.5f - 30 + (1 - a) * 16;   /* the top stays put while it drops down */
   if (s_mode == 1) cy = (H - chgt) * 0.5f - 20 + (1 - a) * 16;
   y = cy + HEAD;
   for (i = 0; i < s_n; i++)
   {
      rowh[i] = s_rows[i].sub ? SH * ex : RH;
      rowy[i] = y; y += rowh[i];
   }
   if (s_sel >= s_n) s_sel = s_n - 1;
   if (fabsf(s_pill.x - rowy[s_sel]) > 400 || a < 0.05f) { s_pill.x = rowy[s_sel]; s_pill.v = 0; }
   spring_step(&s_pill, rowy[s_sel], 260, 26, s_dt);

   gl_use_2d();
   draw_2d(0, 0, 0, W, H, back, 0);
   ui_shadow(cx, cy, CW, chgt, 26, 40, 14, 0.55f * a);
   draw_round_rect(cx, cy, CW, chgt, 26, card, 0.10f * a);

   {  /* header: the game's cover, the kicker, the title */
      float tx = cx + PADX;
      if (g->tex && (g->tex_state == 1) && g->tex_w > 0 && g->tex_h > 0)
      {
         float th = 88, tw = th * g->tex_w / (float)g->tex_h;
         const float wh[4] = { 1, 1, 1, a };
         if (tw > 88) { tw = 88; th = tw * g->tex_h / (float)g->tex_w; }
         ui_shadow(cx + PADX, cy + PADX, tw, th, 10, 10, 3, 0.45f * a);
         draw_2d(g->tex, cx + PADX, cy + PADX, tw, th, wh, 0);
         tx += tw + 22;
      }
      label_set(&s_kicker, g_font_small, tr(s_mode ? "gm.resume_title" : "gm.title"));
      label_set(&s_ltitle, g_font_title, s_title);
      if (s_kicker.tex) draw_2d(s_kicker.tex, tx, cy + PADX + 8, s_kicker.w, s_kicker.h, acc, 0);
      if (s_ltitle.tex)
      {
         float maxw = cx + CW - PADX - tx, tw2 = s_ltitle.w > maxw ? maxw : s_ltitle.w;
         draw_2d(s_ltitle.tex, tx, cy + PADX + 12 + s_kicker.h, tw2, s_ltitle.h, txt, 0);
      }
      {
         float sep[4]; col4(sep, TC_TEXT, 0.08f * a);
         draw_2d(0, cx + PADX, cy + HEAD - 14, CW - 2 * PADX, 1, sep, 0);
      }
   }

   /* the dropped-down folders are clipped to the card */
   glEnable(GL_SCISSOR_TEST);
   gl_scissor(cx, cy + HEAD - 6, CW, chgt - HEAD);
   {
      float ph = (s_rows[s_sel].sub ? SH : RH) - 8;
      ui_shadow(cx + PADX - 8, s_pill.x + 4, CW - 2 * PADX + 16, ph, ph * 0.5f, 10, 3, 0.30f * a);
      draw_round_rect(cx + PADX - 8, s_pill.x + 4, CW - 2 * PADX + 16, ph, ph * 0.5f, pill, 0.0f);
   }
   for (i = 0; i < s_n; i++)
   {
      const mrow_t *r = &s_rows[i];
      const char *val, *lbl = row_text(r, &val);
      float ra = r->sub ? ex : 1.0f, h = r->sub ? SH : RH, x = cx + PADX + 16 + (r->sub ? 26 : 0), xr = cx + CW - PADX - 16;
      float c[4];
      if (ra < 0.02f) continue;
      memcpy(c, i == s_sel ? txt : dim, sizeof(c));
      if (i != s_sel && !r->sub) memcpy(c, txt, sizeof(c)), c[3] *= 0.86f;
      c[3] *= ra;
      label_set(&s_rl[i], r->sub ? g_font_small : g_font_sub, lbl);
      if (s_rl[i].tex) draw_2d(s_rl[i].tex, x, rowy[i] + (h - s_rl[i].h) * 0.5f, s_rl[i].w, s_rl[i].h, c, 0);
      if (r->type == R_FOLDERS)
      {  /* chevron: > folded, v dropped down (2 px steps) */
         float cv[4] = { c[0], c[1], c[2], c[3] * 0.85f }, ccx = xr - 8, ccy = rowy[i] + h * 0.5f;
         int k;
         for (k = 0; k < 5; k++)
         {
            if (ex > 0.5f)
            {
               draw_2d(0, ccx - 8 + k * 2, ccy - 4 + k * 2, 2.5f, 2.5f, cv, 0);
               draw_2d(0, ccx + 8 - k * 2 - 2.5f, ccy - 4 + k * 2, 2.5f, 2.5f, cv, 0);
            }
            else
            {
               draw_2d(0, ccx - 4 + k * 2, ccy - 8 + k * 2, 2.5f, 2.5f, cv, 0);
               draw_2d(0, ccx - 4 + k * 2, ccy + 8 - k * 2 - 2.5f, 2.5f, 2.5f, cv, 0);
            }
         }
         xr -= 30;
      }
      if (r->type == R_FOLDER && coll_in_folder(r->folder, s_content))
      {
         float d[4]; col4(d, i == s_sel ? TC_TEXT : TC_ACCENT, ra * a);
         draw_round_rect(xr - 12, rowy[i] + h * 0.5f - 6, 12, 12, 6, d, 0.0f);
         xr -= 26;
      }
      if (r->type == R_FAV && coll_is_fav(s_content))
      {
         float d[4]; col4(d, i == s_sel ? TC_TEXT : TC_ACCENT, a);
         draw_round_rect(xr - 12, rowy[i] + h * 0.5f - 6, 12, 12, 6, d, 0.0f);
         xr -= 26;
      }
      if (r->type == R_NEWFOLDER)
      {  /* a plus before the label */
         float pc[4] = { c[0], c[1], c[2], c[3] };
         draw_2d(0, x - 22, rowy[i] + h * 0.5f - 1, 12, 2, pc, 0);
         draw_2d(0, x - 17, rowy[i] + h * 0.5f - 6, 2, 12, pc, 0);
      }
      if (val)
      {
         label_set(&s_rv[i], g_font_small, val);
         if (s_rv[i].tex) { float vc[4] = { c[0], c[1], c[2], c[3] * 0.7f }; draw_2d(s_rv[i].tex, xr - s_rv[i].w, rowy[i] + (h - s_rv[i].h) * 0.5f, s_rv[i].w, s_rv[i].h, vc, 0); }
      }
   }
   glDisable(GL_SCISSOR_TEST);
}
