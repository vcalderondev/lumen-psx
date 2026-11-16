/* First-run presentation: what Lumen changes compared with the stock PlayStation
 * Classic, in a few full-screen pages. Shown at start while the setting welcome_hide
 * is 0; the "don't show again" box (Triangle) sets it when the presentation ends.
 * Pages: Cross next / finish, Circle back, Triangle toggles the box.
 * On a new install (setting language_asked = 0, set by main.c when settings.ini was
 * missing) a language page comes first: Up / Down pick, Cross applies and goes on. It is
 * shown even when the presentation itself is hidden, and never again once answered. */
#include <stdio.h>
#include <string.h>
#include "lumen.h"
#include "welcome.h"
#include "i18n.h"
#include "settings.h"
#include "ui.h"

#define PAGES   4
#define LINES   4
#define COL_W   900.0f

static int    s_on, s_page, s_hide;
static int    s_lang, s_lang_sel, s_intro;   /* language page up; its focus; presentation follows */
static spring s_ring;                         /* focus ring position (row index) */
static label_t s_lang_row[I18N_MAX_LANGS];
static spring s_fade, s_slide;
static label_t s_kicker, s_title, s_line[LINES], s_box;
/* per page: kicker, title, up to LINES lines (tr keys) */
static const char *const k_text[PAGES][2 + LINES] = {
   { "welcome.p1.kicker", "welcome.p1.title", "welcome.p1.l1", "welcome.p1.l2", "welcome.p1.l3", "welcome.p1.l4" },
   { "welcome.p2.kicker", "welcome.p2.title", "welcome.p2.l1", "welcome.p2.l2", "welcome.p2.l3", "welcome.p2.l4" },
   { "welcome.p3.kicker", "welcome.p3.title", "welcome.p3.l1", "welcome.p3.l2", "welcome.p3.l3", "welcome.p3.l4" },
   { "welcome.p4.kicker", "welcome.p4.title", "welcome.p4.l1", "welcome.p4.l2", "welcome.p4.l3", "welcome.p4.l4" },
};

void welcome_init(void)
{
   int i;
   s_intro = !settings_get_int("welcome_hide", 0, 0, 1);
   s_lang = !settings_get_int("language_asked", 1, 0, 1) && i18n_count() > 1;
   s_on = s_intro || s_lang;
   s_page = 0; s_hide = 0; s_lang_sel = 0;
   for (i = 0; i < i18n_count(); i++)
      if (!strcmp(i18n_code(i), i18n_language())) s_lang_sel = i;
   memset(&s_fade, 0, sizeof(s_fade)); memset(&s_slide, 0, sizeof(s_slide)); memset(&s_ring, 0, sizeof(s_ring));
   s_ring.x = (float)s_lang_sel;
   s_fade.x = s_on ? 1.0f : 0.0f;
   if (s_on) logf_("welcome: shown (language page: %d, presentation: %d)", s_lang, s_intro);
}

int   welcome_active(void) { return s_on || s_fade.x > 0.003f; }
float welcome_amount(void) { return clampf(s_fade.x, 0.0f, 1.0f); }

static void finish(void)
{
   s_on = 0;
   if (s_hide)
   {
      settings_set_int("welcome_hide", 1);
      settings_save();
   }
   logf_("welcome: closed (hide next time: %d)", s_hide);
}

/* Cross on the language page: apply it, remember it was asked, go on */
static void choose_language(void)
{
   const char *code = i18n_code(s_lang_sel);
   if (code && strcmp(code, i18n_language()) && i18n_set_language(code) == 0)
   {
      char keep[16];
      snprintf(keep, sizeof(keep), "%s", code);
      settings_set("language", keep);
      ui_language_changed();
   }
   settings_set_int("language_asked", 1);
   settings_save();
   logf_("welcome: language %s", i18n_language());
   s_lang = 0;
   if (s_intro) s_slide.x = 60.0f;
   else finish();
}

void welcome_update(const ui_nav_t *nav, float dt)
{
   if (s_on && s_lang)
   {
      const int n = i18n_count();
      if (nav->down && s_lang_sel < n - 1) s_lang_sel++;
      else if (nav->up && s_lang_sel > 0) s_lang_sel--;
      else if (nav->confirm) choose_language();
   }
   else if (s_on)
   {
      if (nav->options) s_hide = !s_hide;
      if (nav->confirm || nav->right)
      {
         if (s_page < PAGES - 1) { s_page++; s_slide.x = 60.0f; }
         else if (nav->confirm) finish();
      }
      else if ((nav->back || nav->left) && s_page > 0) { s_page--; s_slide.x = -60.0f; }
   }
   spring_step(&s_fade, s_on ? 1.0f : 0.0f, 90, 14, dt);
   spring_step(&s_slide, 0.0f, 170, 18, dt);
   spring_step(&s_ring, (float)s_lang_sel, 260, 26, dt);
}

/* the language page: kicker, title, one row per installed language in its own name */
#define LROW_W 440.0f
#define LROW_H 46.0f
#define LROW_STEP 54.0f
static void draw_language(float x, float y, float a, const float acc[4], const float c1[4], const float c2[4])
{
   const int n = i18n_count();
   float rx = (W - LROW_W) * 0.5f + s_slide.x, ry;
   int i;
   (void)x;
   label_set(&s_kicker, g_font_small, tr("welcome.lang.kicker"));
   if (s_kicker.tex) draw_2d(s_kicker.tex, (W - s_kicker.w) * 0.5f + s_slide.x, y, s_kicker.w, s_kicker.h, acc, 0);
   y += 30;
   label_set(&s_title, g_font_title, tr("welcome.lang.title"));
   if (s_title.tex) draw_2d(s_title.tex, (W - s_title.w) * 0.5f + s_slide.x, y, s_title.w, s_title.h, c1, 0);
   y += (s_title.h ? s_title.h : 40) + 34;
   /* fit the list between the title and the help bar */
   ry = y;
   if (ry + n * LROW_STEP > 640.0f) ry = 640.0f - n * LROW_STEP;
   for (i = 0; i < n; i++) ui_rest(rx, ry + i * LROW_STEP, LROW_W, LROW_H, LROW_H * 0.5f, a);
   ui_focus(rx, ry + s_ring.x * LROW_STEP, LROW_W, LROW_H, LROW_H * 0.5f, a);
   for (i = 0; i < n && i < I18N_MAX_LANGS; i++)
   {
      label_set(&s_lang_row[i], g_font_sub, i18n_name(i));
      if (s_lang_row[i].tex)
         draw_2d(s_lang_row[i].tex, rx + (LROW_W - s_lang_row[i].w) * 0.5f, ry + i * LROW_STEP + (LROW_H - s_lang_row[i].h) * 0.5f,
                 s_lang_row[i].w, s_lang_row[i].h, i == s_lang_sel ? c1 : c2, 0);
   }
}

static void text(label_t *l, TTF_Font *f, const char *key, float x, float y, const float c[4])
{
   label_set(l, f, tr(key));
   if (l->tex) draw_2d(l->tex, x, y, l->w, l->h, c, 0);
}

void welcome_draw(double t)
{
   const float a = welcome_amount();
   const float bg[4] = { 0.016f, 0.024f, 0.031f, a };
   const float acc[4] = { 0.23f, 0.51f, 0.96f, a };
   float c1[4] = { 0.98f, 0.98f, 0.99f, a }, c2[4] = { 0.86f, 0.87f, 0.90f, 0.92f * a }, c3[4] = { 0.62f, 0.64f, 0.70f, a };
   float x = (W - COL_W) * 0.5f + s_slide.x, y = 150.0f;
   int i;
   (void)t;
   if (a <= 0.003f) return;
   gl_use_2d();
   draw_2d(0, 0, 0, W, H, bg, 0);
   if (s_lang) { draw_language(x, y, a, acc, c1, c2); return; }
   /* kicker, title, lines */
   text(&s_kicker, g_font_small, k_text[s_page][0], x, y, acc);
   y += 30;
   text(&s_title, g_font_title, k_text[s_page][1], x, y, c1);
   y += (s_title.h ? s_title.h : 40) + 26;
   for (i = 0; i < LINES; i++)
   {
      const char *s = tr(k_text[s_page][2 + i]);
      if (!s[0] || !strcmp(s, k_text[s_page][2 + i])) continue;   /* unused line */
      draw_round_rect(x, y + 12, 8, 8, 4, acc, 0.0f);
      label_set(&s_line[i], g_font_sub, s);
      if (s_line[i].tex) draw_2d(s_line[i].tex, x + 24, y, s_line[i].w, s_line[i].h, c2, 0);
      y += (s_line[i].h ? s_line[i].h : 28) + 18;
   }
   /* page dots (shapes: the UI font has no circle glyphs); the current one is a pill */
   {
      float dx = W * 0.5f - (PAGES * 10 + 14 + (PAGES - 1) * 8) * 0.5f;
      for (i = 0; i < PAGES; i++)
      {
         float w = i == s_page ? 24.0f : 10.0f;
         draw_round_rect(dx, 560, w, 10, 5, i == s_page ? acc : c3, 0.0f);
         dx += w + 8;
      }
   }
   /* don't show again: a box with a tick */
   {
      const float box[4] = { 1, 1, 1, 0.10f * a }, edge[4] = { 1, 1, 1, 0.45f * a };
      float bx = (W - COL_W) * 0.5f, by = 604;
      draw_round_rect(bx, by, 22, 22, 6, s_hide ? acc : box, s_hide ? 0.0f : 0.0f);
      if (!s_hide) draw_round_rect(bx, by, 22, 22, 6, edge, 0.0f), draw_round_rect(bx + 2, by + 2, 18, 18, 5, bg, 0.0f);
      else draw_round_rect(bx + 6, by + 6, 10, 10, 3, c1, 0.0f);
      label_set(&s_box, g_font_small, tr("welcome.dont_show"));
      if (s_box.tex) draw_2d(s_box.tex, bx + 34, by + (22 - s_box.h) * 0.5f, s_box.w, s_box.h, c2, 0);
   }
}

int welcome_last_page(void) { return !s_lang && s_page == PAGES - 1; }
int welcome_language_page(void) { return s_on && s_lang; }
