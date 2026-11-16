/* Game menu and resume question, see gamemenu.h. */
#include <stdio.h>
#include <string.h>
#include "lumen.h"
#include "music.h"
#include "gamemenu.h"
#include "collections.h"
#include "i18n.h"
#include "panel.h"
#include "theme.h"
#include "ui.h"

/* lumen-design, game-menu.html: a glass card on the right half, so the focused tile and
 * its title stay visible on the left; the actions are buttons, the main one white */
#define CW      520.0f   /* card width                   */
#define CX      676.0f
#define HEAD    168.0f   /* header: cover, kicker, title */
#define RH      64.0f    /* button pitch (52 + 12)       */
#define BH      52.0f
#define SH      46.0f    /* folder row pitch (dropped)   */
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
static label_t s_kicker, s_ltitle, s_facts, s_rl[MAXR], s_rv[MAXR];

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
   music_sfx(SFX_OPEN);
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

static void gm_input(const ui_nav_t *nav);

void gamemenu_update(const ui_nav_t *nav, double t, float dt)
{
   s_dt = dt > 0.0f ? dt : s_dt;
   spring_step(&s_fade, s_open ? 1.0f : 0.0f, 120, 18, dt);
   spring_step(&s_exp, s_expanded ? 1.0f : 0.0f, 200, 22, dt);
   if (!s_open || !nav || t - s_t0 < 0.15) return;   /* the Start that opened it */
   {  /* interface sounds (guidelines/06 "Sound"): what this frame's input changed */
      int sel = s_sel, exp = s_expanded, id = -1;
      gm_input(nav);
      if (!s_open) id = s_result == GM_NONE ? SFX_BACK : -1;   /* Play: the dashboard sounds the launch */
      else if (nav->confirm || exp < s_expanded) id = SFX_CONFIRM;
      else if (exp > s_expanded) id = SFX_BACK;
      else if (sel != s_sel) id = SFX_FOCUS;
      if (id >= 0) music_sfx(id);
   }
}

static void gm_input(const ui_nav_t *nav)
{
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
   float rowy[MAXR], rowh[MAXR], y, chgt, cx = CX, cy;
   float txt[4], dim[4], acc[4];
   const float white[3] = { 1, 1, 1 }, ink[4] = { 0.043f, 0.047f, 0.063f, a };
   char buf[160];
   const game_t *g;
   int i;
   (void)t;
   if (a <= 0.003f || s_gi < 0 || s_gi >= g_ngames) return;
   g = &g_games[s_gi];
   col4(txt, TC_TEXT, a); col4(dim, TC_DIM, a); col4(acc, TC_ACCENT, a);

   y = HEAD;
   for (i = 0; i < s_n; i++)
   {
      rowh[i] = s_rows[i].sub ? SH * ex : RH;
      rowy[i] = y; y += rowh[i];
   }
   chgt = y + 16;
   cy = 150.0f;
   if (cy + chgt > 636.0f) cy = 636.0f - chgt;   /* many folders dropped down: the card grows upwards */
   if (cy < 104.0f) cy = 104.0f;
   cy += (1 - a) * 16;
   for (i = 0; i < s_n; i++) rowy[i] += cy;
   if (s_sel >= s_n) s_sel = s_n - 1;
   if (fabsf(s_pill.x - rowy[s_sel]) > 400 || a < 0.05f) { s_pill.x = rowy[s_sel]; s_pill.v = 0; }
   spring_step(&s_pill, rowy[s_sel], 260, 26, s_dt);

   gl_use_2d();
   ui_scrim(a);
   ui_card(cx, cy, CW, chgt, a);

   {  /* header: the game's cover, the kicker, the title, one line of facts */
      float tx = cx + PADX;
      if (g->tex && (g->tex_state == 1) && g->tex_w > 0 && g->tex_h > 0)
      {
         const float wh[4] = { 1, 1, 1, a };
         ui_shadow(cx + PADX, cy + PADX, 112, 112, 12, 10, 3, 0.45f * a);
         draw_image(g->tex, g->tex_w, g->tex_h, cx + PADX, cy + PADX, 112, 112, 12, wh);
         gl_use_2d();
         tx += 112 + 22;
      }
      ui_kicker(&s_kicker, tr(s_mode ? "gm.resume_title" : "gm.title"), tx, cy + 44, a);
      label_set(&s_ltitle, g_font_title, ui_fit(g_font_title, s_title, cx + CW - PADX - tx, buf, sizeof(buf)));
      if (s_ltitle.tex) draw_2d(s_ltitle.tex, floorf(tx), floorf(cy + 62), s_ltitle.w, s_ltitle.h, txt, 0);
      label_set(&s_facts, g_font_small, g->year);
      if (s_facts.tex) draw_2d(s_facts.tex, floorf(tx), floorf(cy + 108), s_facts.w, s_facts.h, dim, 0);
   }

   /* the dropped-down folders are clipped to the card */
   glEnable(GL_SCISSOR_TEST);
   gl_scissor(cx, cy + HEAD - 10, CW, chgt - HEAD + 10);
   {  /* focus: glass and ring behind the buttons, travelling with a spring */
      const float fh = s_rows[s_sel].sub ? SH - 6 : BH, fx = cx + PADX + (s_rows[s_sel].sub ? 16 : 0);
      ui_focus(fx, s_pill.x + (s_rows[s_sel].sub ? 3 : 0), cx + CW - PADX - fx, fh, fh * 0.5f, a);
   }
   for (i = 0; i < s_n; i++)
   {
      const mrow_t *r = &s_rows[i];
      const char *val, *lbl = row_text(r, &val);
      const int primary = r->type == R_PLAY || r->type == R_RESUME;
      float ra = r->sub ? ex : 1.0f, h = r->sub ? SH : BH, x = cx + PADX + 24 + (r->sub ? 26 : 0), xr = cx + CW - PADX - 24;
      float c[4];
      if (ra < 0.02f) continue;
      memcpy(c, txt, sizeof(c));
      if (r->sub && i != s_sel) memcpy(c, dim, sizeof(c));
      c[3] *= ra;
      if (!r->sub)
      {  /* a button: the main action is the white pill, the others glass */
         if (primary) ui_fill(cx + PADX, rowy[i], CW - 2 * PADX, BH, BH * 0.5f, white, a);
         else ui_rest(cx + PADX, rowy[i], CW - 2 * PADX, BH, BH * 0.5f, a);
         if (primary) memcpy(c, ink, sizeof(c));
      }
      label_set(&s_rl[i], r->sub ? g_font_sub : g_font_medium, lbl);
      if (s_rl[i].tex) draw_2d(s_rl[i].tex, floorf(x), floorf(rowy[i] + (h - s_rl[i].h) * 0.5f), s_rl[i].w, s_rl[i].h, c, 0);
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
      if ((r->type == R_FOLDER && coll_in_folder(r->folder, s_content)) || (r->type == R_FAV && coll_is_fav(s_content)))
      {  /* the accent as an indicator: a check dot */
         ui_fill(xr - 12, rowy[i] + h * 0.5f - 6, 12, 12, 6, acc, ra * a);
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
         if (s_rv[i].tex) { float vc[4] = { dim[0], dim[1], dim[2], c[3] }; draw_2d(s_rv[i].tex, xr - s_rv[i].w, rowy[i] + (h - s_rv[i].h) * 0.5f, s_rv[i].w, s_rv[i].h, vc, 0); }
      }
   }
   glDisable(GL_SCISSOR_TEST);
}
