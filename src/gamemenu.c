/* Game menu and resume question, see gamemenu.h. */
#include <stdio.h>
#include <string.h>
#include "lumen.h"
#include "module.h"
#include "music.h"
#include "gamemenu.h"
#include "collections.h"
#include "i18n.h"
#include "panel.h"
#include "stats.h"
#include "theme.h"
#include "ui.h"

/* lumen-design, proposals/lumen-2 "Game menu": a sheet on the right edge, full height, so
 * the game stays visible on the left; the facts are a table, every action has its glyph and
 * the main one is filled (the theme's `primary`, white in Lumen's own look) */
#define CW      468.0f   /* sheet width                  */
#define CX      (W - CW)
#define HEAD    326.0f   /* header: cover, kicker, title, the facts */
#define HEAD_NF 236.0f   /* ... with no fact known       */
#define RH      66.0f    /* button pitch (52 + 14)       */
#define BH      52.0f
#define SH      46.0f    /* folder row pitch (dropped)   */
#define PADX    40.0f
#define VIEW_B  632.0f   /* the rows end here: the help bar is under them */
#define MAXR    (6 + MOD_MENU_MAX + COLL_MAX_FOLDERS + 1)

enum { R_PLAY, R_FAV, R_FOLDERS, R_FOLDER, R_NEWFOLDER, R_RESUME, R_RESTART, R_MOD };
/* folder: the folder (R_FOLDER) or the module entry's handle (R_MOD, module.h "menu.game") */
typedef struct { int type, folder, sub; } mrow_t;

static int     s_open, s_mode, s_gi = -1, s_result, s_sel, s_expanded;
static char    s_content[512], s_title[128];
static spring  s_fade, s_exp, s_pill;
static double  s_t0;
static float   s_dt = 1.0f / 60.0f;   /* last frame time: the pill spring steps in draw */
static mrow_t  s_rows[MAXR];
static int     s_n;
static label_t s_kicker, s_ltitle, s_facts, s_rl[MAXR], s_rv[MAXR], s_fk[3], s_fv[3];
static spring  s_view;   /* rows scrolled up, when the dropped folders do not fit */

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
   {  /* the modules' entries for this game, after Lumen's own (docs/modules.md, "menu.game") */
      int h[MOD_MENU_MAX], n = mod_menu_list(s_gi, h, MOD_MENU_MAX), k;
      for (k = 0; k < n; k++) s_rows[s_n++] = (mrow_t){ R_MOD, h[k], 0 };
   }
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
   case R_MOD:
   {  /* the menu closes first: the module may open a card of its own */
      const int gi = s_gi;
      close_menu(GM_NONE);
      mod_menu_invoke(r->folder, gi);
      break;
   }
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
   case R_MOD:       return mod_menu_label(r->folder);
   }
   return "";
}

static const char *row_glyph(const mrow_t *r)
{
   switch (r->type)
   {
   case R_PLAY: case R_RESUME: return "play";
   case R_FAV:     return "star";
   case R_FOLDERS: return "folder";
   }
   return NULL;
}

void gamemenu_draw(double t)
{
   float a = gamemenu_amount(), ex = clampf(s_exp.x, 0, 1);
   float rowy[MAXR], rowh[MAXR], y, cx = CX + (1 - a) * 40, cy = 0, head, want, edge[4];
   float txt[4], dim[4], acc[4], white[4], ink[4], card[4];
   const char *fk[3], *fv[3];
   char buf[160], dur[48];
   const game_t *g;
   const game_stats_t *gs;
   int i, nf = 0;
   (void)t;
   if (a <= 0.003f || s_gi < 0 || s_gi >= g_ngames) return;
   g = &g_games[s_gi];
   gs = stats_get(g->content);
   col4(txt, TC_TEXT, a); col4(dim, TC_DIM, a); col4(acc, TC_ACCENT, a);
   col4(white, TC_PRIMARY, a); col4(ink, TC_ON_PRIMARY, a);   /* the main button and its text */
   col4(card, TC_CARD, theme_card_alpha() * a); col4(edge, TC_GLASS, 0.20f * a);

   /* the facts that are known (guidelines/08: never "unknown") */
   if (g->year[0]) { fk[nf] = "fact.year"; fv[nf++] = g->year; }
   if (g->players[0]) { fk[nf] = "fact.players"; fv[nf++] = g->players; }
   if (gs && gs->seconds > 0) { fk[nf] = "fact.played"; fv[nf++] = stats_format_duration(dur, sizeof(dur), gs->seconds); }
   head = nf ? HEAD : HEAD_NF;

   y = head;
   for (i = 0; i < s_n; i++)
   {
      rowh[i] = s_rows[i].sub ? SH * ex : RH;
      rowy[i] = y; y += rowh[i];
   }
   if (s_sel >= s_n) s_sel = s_n - 1;
   /* the selection stays above the help bar: the rows scroll under the header */
   want = rowy[s_sel] + (s_rows[s_sel].sub ? SH : BH) + 12 > VIEW_B ? rowy[s_sel] + (s_rows[s_sel].sub ? SH : BH) + 12 - VIEW_B : 0.0f;
   if (a < 0.05f) { s_view.x = want; s_view.v = 0; }
   spring_step(&s_view, want, 220, 26, s_dt);
   for (i = 0; i < s_n; i++) rowy[i] -= s_view.x;
   if (fabsf(s_pill.x - rowy[s_sel]) > 400 || a < 0.05f) { s_pill.x = rowy[s_sel]; s_pill.v = 0; }
   spring_step(&s_pill, rowy[s_sel], 260, 26, s_dt);

   gl_use_2d();
   ui_scrim(a);   /* the theme's scrim: flat black in Lumen's own look */
   /* the sheet: one flat ink shape to the edge of the screen, lit along its left edge */
   draw_2d(0, cx, 0, W - cx + 1, H, card, 0);
   draw_2d(0, cx, 0, 1, H, edge, 0);

   {  /* header: the game's cover in its own proportions, the kicker, the title */
      float tx = cx + PADX;
      if (g->tex && (g->tex_state == 1) && g->tex_w > 0 && g->tex_h > 0)
      {
         const float wh[4] = { 1, 1, 1, a }, ch = 140, cw = clampf(ch * g->tex_w / (float)g->tex_h, 100.0f, 130.0f);
         ui_shadow(cx + PADX, cy + 56, cw, ch, 12, 10, 3, 0.45f * a);
         draw_image(g->tex, g->tex_w, g->tex_h, cx + PADX, cy + 56, cw, ch, 12, wh);
         gl_use_2d();
         tx += cw + 24;
      }
      ui_kicker(&s_kicker, tr(s_mode ? "gm.resume_title" : "gm.title"), tx, cy + 80, a);
      {  /* the title at its size when it fits beside the cover, else smaller before it is cut */
         int tw = 0, th = 0;
         TTF_Font *tf = g_font_title && TTF_SizeUTF8(g_font_title, s_title, &tw, &th) == 0 && tw <= (W - PADX - tx) * g_ui_scale ? g_font_title : g_font_medium;
         label_set(&s_ltitle, tf, ui_fit(tf, s_title, W - PADX - tx, buf, sizeof(buf)));
      }
      if (s_ltitle.tex) draw_2d(s_ltitle.tex, floorf(tx), floorf(cy + 100), s_ltitle.w, s_ltitle.h, txt, 0);
      label_set(&s_facts, g_font_small, ui_fit(g_font_small, g->publisher, W - PADX - tx, buf, sizeof(buf)));
      if (s_facts.tex) draw_2d(s_facts.tex, floorf(tx), floorf(cy + 146), s_facts.w, s_facts.h, dim, 0);
   }
   if (nf)
   {  /* the facts: a table between two hairlines */
      const float fy = cy + 226, fw = (CW - 2 * PADX) / 3.0f;
      float hl[4];
      col4(hl, TC_TEXT, 0.10f * a);
      draw_2d(0, cx + PADX, fy, CW - 2 * PADX, 1, hl, 0);
      draw_2d(0, cx + PADX, fy + 72, CW - 2 * PADX, 1, hl, 0);
      for (i = 0; i < nf; i++)
      {
         ui_kicker(&s_fk[i], tr(fk[i]), cx + PADX + i * fw, fy + 14, a);
         label_set(&s_fv[i], g_font_medium, fv[i]);
         if (s_fv[i].tex) draw_2d(s_fv[i].tex, floorf(cx + PADX + i * fw), floorf(fy + 36), s_fv[i].w, s_fv[i].h, txt, 0);
      }
   }

   /* the rows are clipped between the header and the help bar */
   glEnable(GL_SCISSOR_TEST);
   gl_scissor(cx, cy + head - 10, CW, VIEW_B - head + 20);
   {  /* focus: glass and ring behind the buttons, travelling with a spring */
      const float fh = s_rows[s_sel].sub ? SH - 6 : BH, fx = cx + PADX + (s_rows[s_sel].sub ? 16 : 0);
      ui_focus(fx, s_pill.x + (s_rows[s_sel].sub ? 3 : 0), cx + CW - PADX - fx, fh, fh * 0.5f, a);
   }
   for (i = 0; i < s_n; i++)
   {
      const mrow_t *r = &s_rows[i];
      const char *val, *lbl = row_text(r, &val);
      const int primary = r->type == R_PLAY || r->type == R_RESUME;
      const char *gly = row_glyph(r);
      float ra = r->sub ? ex : 1.0f, h = r->sub ? SH : BH, x = cx + PADX + 18 + (r->sub ? 32 : 0), xr = cx + CW - PADX - 18;
      float c[4];
      if (ra < 0.02f) continue;
      memcpy(c, txt, sizeof(c));
      if (r->sub && i != s_sel) memcpy(c, dim, sizeof(c));
      c[3] *= ra;
      if (!r->sub)
      {  /* a button: the main action is the white pill, the others glass */
         if (primary) ui_fill(cx + PADX, rowy[i], CW - 2 * PADX, BH, UI_CTL_R, white, a);
         else ui_rest(cx + PADX, rowy[i], CW - 2 * PADX, BH, BH * 0.5f, a);
         if (primary) memcpy(c, ink, sizeof(c));
      }
      if (gly)
      {  /* the action's glyph; the star is filled in the accent once the game is a favourite */
         float gc[4] = { c[0], c[1], c[2], c[3] * (primary ? 1.0f : 0.70f) };
         glyph_draw(gly, x, rowy[i] + h * 0.5f - 12, 24, gc);
         x += 24 + 14;
      }
      label_set(&s_rl[i], r->sub ? g_font_sub : g_font_medium, lbl);
      if (s_rl[i].tex) draw_2d(s_rl[i].tex, floorf(x), floorf(rowy[i] + (h - s_rl[i].h) * 0.5f), s_rl[i].w, s_rl[i].h, c, 0);
      if (r->type == R_FOLDERS)
      {  /* chevron: > folded, v dropped down */
         const float cv[4] = { c[0], c[1], c[2], c[3] * 0.85f };
         glyph_draw(ex > 0.5f ? "chevron-down" : "chevron-right", xr - 22, rowy[i] + h * 0.5f - 12, 24, cv);
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
