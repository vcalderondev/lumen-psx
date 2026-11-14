/* Settings panel, on-screen keyboard, confirm dialog and toast (docs/settings-design.md 6).
 *
 * Everything lives in static storage: the rows of the current page are rebuilt every
 * frame into fixed buffers (plain string copies, no heap), and every text on screen is a
 * label_t that re-renders only when its string changes. The keyboard renders its key
 * glyphs once when it opens and frees them when it has faded out. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include "lumen.h"
#include "panel.h"
#include "i18n.h"
#include "settings.h"
#include "version.h"

/* ------------------------------------------------------------------------ */
/* geometry and colours                                                      */
#define PW      520.0f
#define PH      672.0f
#define PX0     (W - 24.0f - PW)
#define PY0     24.0f
#define ROW_H   52.0f
#define ROWS_Y  124.0f       /* first row, relative to the panel top */
#define PAD     32.0f
#define MAX_ROWS 20

static const float C_TXT[3] = { 0.93f, 0.92f, 0.97f };
static const float C_DIM[3] = { 0.55f, 0.54f, 0.64f };
static const float C_ACC[3] = { 0.36f, 0.55f, 1.00f };
static const float C_RED[3] = { 1.00f, 0.42f, 0.42f };

static void col(float out[4], const float rgb[3], float a) { out[0] = rgb[0]; out[1] = rgb[1]; out[2] = rgb[2]; out[3] = a; }

/* ------------------------------------------------------------------------ */
/* labels                                                                    */
/* wrapped variant of label_set(): re-renders only when the text changes */
static void wlabel_set(label_t *l, TTF_Font *f, const char *s, int wrap)
{
   size_t n = sizeof(l->key) - 1;
   int same = strlen(s) >= n ? !strncmp(l->key, s, n) : !strcmp(l->key, s);   /* key keeps n bytes */
   if (same && (l->tex || !*s)) return;
   if (l->tex) glDeleteTextures(1, &l->tex);
   snprintf(l->key, sizeof(l->key), "%s", s);
   l->tex = text_tex_wrapped(f, s, wrap, &l->w, &l->h);
}

static void draw_label(const label_t *l, float x, float y, const float c[4])
{
   if (l->tex && c[3] > 0.003f) draw_2d(l->tex, floorf(x), floorf(y), l->w, l->h, c, 0);
}

/* ------------------------------------------------------------------------ */
/* state                                                                     */
enum { P_ROOT, P_LANGUAGE, P_PROFILE, P_SCREENSAVER, P_GRAPHICS, P_ABOUT, P_COUNT };
static const char *const k_page_key[P_COUNT] = {
   "panel.title", "section.language", "section.profile", "section.screensaver", "section.graphics", "section.about"
};

enum { R_ACTION, R_CHOICE, R_INFO };
enum { ID_LANG = 100, ID_PROFILE_ACTIVE = 200, ID_SS_AFTER = 300, ID_SS_DEMOS, ID_SS_LENGTH, ID_SS_PREVIEW, ID_INFO = 900 };

typedef struct {
   int type, id, enabled, dim, check, enter, at_lo, at_hi;
   char label[96], value[96];
} row_t;

static row_t   s_rows[MAX_ROWS];
static int     s_nrows;
static const char *s_footer_key;       /* tr key of the page footer, or NULL */

static int     s_open, s_page, s_sel;
static spring  s_slide, s_fade, s_px, s_pa, s_hl, s_scroll;
static char    s_free[32];             /* About: free space, measured when the page opens */

static label_t s_title, s_ptitle, s_footer, s_lt, s_gt;
static label_t s_rl[MAX_ROWS], s_rv[MAX_ROWS];

/* screensaver choices */
static const int k_minutes[] = { 0, 1, 2, 3, 5, 10, 15, 30 };
static const int k_seconds[] = { 30, 45, 60, 90, 120, 180 };
#define NELEM(a) ((int)(sizeof(a) / sizeof((a)[0])))

static int nearest(const int *list, int n, int v)
{
   int i, best = 0;
   for (i = 1; i < n; i++) if (abs(list[i] - v) < abs(list[best] - v)) best = i;
   return best;
}

/* ------------------------------------------------------------------------ */
/* modal: on-screen keyboard / confirm dialog                                */
enum { M_NONE, M_OSK, M_CONFIRM };
static int     s_modal, s_modal_drawn;  /* open modal / modal still fading out */
static spring  s_mod;

/* keyboard (6.6) */
typedef const char *(*osk_accept_fn)(const char *text);  /* NULL = accepted, else a tr key to show */
#define OSK_W 760.0f
#define OSK_H 330.0f
#define OSK_MAX_CHARS 16
#define OSK_MAX_BYTES 48
static const char *const k_keys[40] = {
   "1", "2", "3", "4", "5", "6", "7", "8", "9", "0",
   "A", "B", "C", "D", "E", "F", "G", "H", "I", "J",
   "K", "L", "M", "N", "O", "P", "Q", "R", "S", "T",
   "U", "V", "W", "X", "Y", "Z", "\xC3\x91", "-", ".", "_",
};
static char        s_klower[40][4];
enum { OK_SHIFT, OK_SPACE, OK_ERASE, OK_OK };
static const int   k_act_col[4] = { 0, 2, 6, 8 }, k_act_span[4] = { 2, 4, 2, 2 };
static const char *const k_act_key[4] = { "osk.shift", "osk.space", "osk.erase", "osk.ok" };
static label_t     s_kup[40], s_klo[40], s_kact[4], s_ktitle, s_ktext;
static char        s_text[OSK_MAX_BYTES + 4];
static int         s_orow, s_ocol, s_lower;
static osk_accept_fn s_osk_accept;
static spring      s_kx, s_ky, s_kw;

/* confirm (6.7) */
typedef void (*confirm_fn)(void);
#define DLG_W 560.0f
#define DLG_H 220.0f
static label_t     s_cmsg, s_cnote, s_cbtn[2];
static int         s_csel;              /* 0 cancel (default), 1 delete */
static confirm_fn  s_con_yes;

/* toast */
static label_t     s_toast;
static const char *s_toast_key;
static double      s_toast_t = -100.0;

/* ------------------------------------------------------------------------ */
/* rows of the current page                                                  */
static row_t *row_add(int type, int id, const char *label, const char *value)
{
   row_t *r;
   if (s_nrows >= MAX_ROWS) return NULL;
   r = &s_rows[s_nrows++];
   memset(r, 0, sizeof(*r));
   r->type = type; r->id = id; r->enabled = 1;
   snprintf(r->label, sizeof(r->label), "%s", label);
   snprintf(r->value, sizeof(r->value), "%s", value ? value : "");
   return r;
}

static int lang_active_index(void)
{
   int i;
   for (i = 0; i < i18n_count(); i++) if (!strcmp(i18n_code(i), i18n_language())) return i;
   return -1;
}

static void minutes_text(char *buf, size_t n, int m)
{
   char v[16];
   if (m <= 0) { snprintf(buf, n, "%s", tr("ss.never")); return; }
   snprintf(v, sizeof(v), "%d", m);
   trf(buf, n, "ss.minutes", "n", v, NULL);
}

static void page_build(void)
{
   char v[96], num[32];
   row_t *r;
   int i;
   s_nrows = 0; s_footer_key = NULL;
   switch (s_page)
   {
   case P_ROOT:
      i = lang_active_index();
      r = row_add(R_ACTION, P_LANGUAGE, tr("section.language"), i >= 0 ? i18n_name(i) : i18n_language());
      if (r) r->enter = 1;
      r = row_add(R_ACTION, P_PROFILE, tr("section.profile"), tr("profile.main"));    /* profiles: step 3 */
      if (r) r->enter = 1;
      minutes_text(v, sizeof(v), settings_get_int("screensaver_minutes", 5, 0, 60));
      r = row_add(R_ACTION, P_SCREENSAVER, tr("section.screensaver"), v);
      if (r) r->enter = 1;
      r = row_add(R_ACTION, P_GRAPHICS, tr("section.graphics"), NULL);
      if (r) r->enter = 1;
      r = row_add(R_ACTION, P_ABOUT, tr("section.about"), NULL);
      if (r) r->enter = 1;
      break;
   case P_LANGUAGE:
      for (i = 0; i < i18n_count(); i++)
         if ((r = row_add(R_ACTION, ID_LANG + i, i18n_name(i), NULL)))
            r->check = !strcmp(i18n_code(i), i18n_language());
      break;
   case P_PROFILE:
      /* until profiles exist (step 3): only the active profile, no actions */
      if ((r = row_add(R_CHOICE, ID_PROFILE_ACTIVE, tr("profile.active"), tr("profile.main"))))
         r->at_lo = r->at_hi = 1;
      break;
   case P_SCREENSAVER:
   {
      int m = nearest(k_minutes, NELEM(k_minutes), settings_get_int("screensaver_minutes", 5, 0, 60));
      int demos = settings_get_int("demo_enabled", 1, 0, 1);
      int s = nearest(k_seconds, NELEM(k_seconds), settings_get_int("demo_seconds", 60, 20, 300));
      minutes_text(v, sizeof(v), k_minutes[m]);
      if ((r = row_add(R_CHOICE, ID_SS_AFTER, tr("ss.after"), v)))
      { r->at_lo = m == 0; r->at_hi = m == NELEM(k_minutes) - 1; }
      if ((r = row_add(R_CHOICE, ID_SS_DEMOS, tr("ss.demos"), tr(demos ? "value.on" : "value.off"))))
      { r->at_lo = !demos; r->at_hi = demos; }
      snprintf(num, sizeof(num), "%d", k_seconds[s]);
      trf(v, sizeof(v), "ss.seconds", "n", num, NULL);
      if ((r = row_add(R_CHOICE, ID_SS_LENGTH, tr("ss.demo_length"), v)))
      { r->at_lo = s == 0; r->at_hi = s == NELEM(k_seconds) - 1; r->dim = !demos; r->enabled = demos; }
      row_add(R_ACTION, ID_SS_PREVIEW, tr("ss.preview"), NULL);
      s_footer_key = "ss.note";
      break;
   }
   case P_GRAPHICS:
      /* per-system presets arrive with step 4 */
      s_footer_key = "gfx.none";
      break;
   case P_ABOUT:
      row_add(R_INFO, ID_INFO, tr("about.version"), LUMEN_VERSION);
      snprintf(num, sizeof(num), "%d", g_ngames);
      row_add(R_INFO, ID_INFO, tr("about.games"), num);
      snprintf(num, sizeof(num), "%d", g_nplat);
      row_add(R_INFO, ID_INFO, tr("about.systems"), num);
      if (s_free[0])
      {
         const char *unit = strchr(s_free, '.') ? "about.gb" : "about.mb";
         row_add(R_INFO, ID_INFO, tr("about.free"), trf(v, sizeof(v), unit, "n", s_free, NULL));
      }
      break;
   }
}

static int row_selectable(int i) { return i >= 0 && i < s_nrows && s_rows[i].type != R_INFO; }

static int first_selectable(void)
{
   int i;
   for (i = 0; i < s_nrows; i++) if (row_selectable(i)) return i;
   return -1;
}

static void sel_move(int dir)
{
   int i;
   if (s_sel < 0) return;
   for (i = s_sel + dir; i >= 0 && i < s_nrows; i += dir)
      if (row_selectable(i)) { s_sel = i; return; }
}

static void measure_free(void)
{
   struct statvfs st;
   s_free[0] = 0;
   if (statvfs(g_root, &st) == 0)
   {
      double b = (double)st.f_bavail * (double)st.f_frsize;
      if (b >= 1024.0 * 1024.0 * 1024.0) snprintf(s_free, sizeof(s_free), "%.1f", b / (1024.0 * 1024.0 * 1024.0));
      else snprintf(s_free, sizeof(s_free), "%.0f", b / (1024.0 * 1024.0));
   }
}

/* place the highlight and the scroll without animation (page entry) */
static void rows_snap(void)
{
   s_hl.x = s_sel > 0 ? s_sel * ROW_H : 0.0f; s_hl.v = 0;
   s_scroll.x = 0; s_scroll.v = 0;
}

static void page_enter(int page, int dir)
{
   int i;
   s_page = page;
   if (page == P_ABOUT) measure_free();
   page_build();
   s_sel = first_selectable();
   if (page == P_LANGUAGE && (i = lang_active_index()) >= 0) s_sel = i;
   rows_snap();
   s_px.x = dir * 80.0f; s_px.v = 0;
   s_pa.x = 0.0f; s_pa.v = 0;
}

static void panel_close(void)
{
   if (!s_open) return;
   s_open = 0;
   logf_("panel: close");
   if (settings_dirty() && settings_save() != 0) panel_toast("error.save");
}

static void row_action(const row_t *r)
{
   if (r->id >= ID_LANG && r->id < ID_LANG + I18N_MAX_LANGS)
   {
      const char *code = i18n_code(r->id - ID_LANG);
      if (!code || !strcmp(code, i18n_language())) return;
      if (i18n_set_language(code) == 0)
      {
         char keep[16];
         snprintf(keep, sizeof(keep), "%s", code);
         settings_set("language", keep);
         ui_language_changed();
         logf_("panel: language %s", keep);
      }
   }
   else if (r->id == ID_SS_PREVIEW)
   {
      panel_close();
      ui_start_attract();
   }
}

static void row_change(const row_t *r, int dir)
{
   int i;
   if (!r->enabled || (dir < 0 && r->at_lo) || (dir > 0 && r->at_hi)) return;
   switch (r->id)
   {
   case ID_SS_AFTER:
      i = nearest(k_minutes, NELEM(k_minutes), settings_get_int("screensaver_minutes", 5, 0, 60)) + dir;
      if (i >= 0 && i < NELEM(k_minutes)) settings_set_int("screensaver_minutes", k_minutes[i]);
      break;
   case ID_SS_DEMOS:
      settings_set_int("demo_enabled", dir > 0);
      break;
   case ID_SS_LENGTH:
      i = nearest(k_seconds, NELEM(k_seconds), settings_get_int("demo_seconds", 60, 20, 300)) + dir;
      if (i >= 0 && i < NELEM(k_seconds)) settings_set_int("demo_seconds", k_seconds[i]);
      break;
   }
}

static void page_input(const ui_nav_t *nav)
{
   const row_t *r = row_selectable(s_sel) ? &s_rows[s_sel] : NULL;
   if (nav->up) sel_move(-1);
   if (nav->down) sel_move(1);
   if (s_page == P_ROOT)
   {
      if (nav->confirm && r) page_enter(r->id, 1);
      else if (nav->back || nav->options) panel_close();
      return;
   }
   if (r && r->type == R_CHOICE && (nav->left || nav->right)) row_change(r, nav->right ? 1 : -1);
   if (nav->confirm && r && r->type == R_ACTION && r->enabled) row_action(r);
   else if (nav->back || nav->options)
   {
      int from = s_page;
      page_enter(P_ROOT, -1);
      s_sel = from - 1;   /* root rows are the pages in order */
      rows_snap();
   }
}

/* ------------------------------------------------------------------------ */
/* on-screen keyboard                                                        */
static int utf8_chars(const char *s)
{
   int n = 0;
   for (; *s; s++) if (((unsigned char)*s & 0xC0) != 0x80) n++;
   return n;
}

static void osk_key_rect(int row, int col, float *x, float *y, float *w)
{
   const float pitch = (OSK_W - 48.0f) / 10.0f;
   float ox = (W - OSK_W) * 0.5f, oy = (H - OSK_H) * 0.5f;
   int span = 1;
   if (row == 4) { span = k_act_span[col]; col = k_act_col[col]; }
   *x = ox + 24.0f + col * pitch + 3.5f;
   *y = oy + 112.0f + row * 42.0f;
   *w = span * pitch - 7.0f;
}

static int osk_act_of_col(int c) { return c < 2 ? OK_SHIFT : (c < 6 ? OK_SPACE : (c < 8 ? OK_ERASE : OK_OK)); }

static void osk_snap(void)
{
   float x, y, w;
   osk_key_rect(s_orow, s_orow == 4 ? osk_act_of_col(s_ocol) : s_ocol, &x, &y, &w);
   s_kx.x = x; s_ky.x = y; s_kw.x = w; s_kx.v = s_ky.v = s_kw.v = 0;
}

static void osk_open(const char *title_key, const char *initial, osk_accept_fn accept)
{
   int k;
   if (s_modal != M_NONE) return;
   if (s_modal_drawn == M_CONFIRM) { label_free(&s_cmsg); label_free(&s_cnote); label_free(&s_cbtn[0]); label_free(&s_cbtn[1]); }
   for (k = 0; k < 40; k++)
   {
      label_set(&s_kup[k], g_font_sub, k_keys[k]);
      if (strcmp(s_klower[k], k_keys[k])) label_set(&s_klo[k], g_font_sub, s_klower[k]);
   }
   for (k = 0; k < 4; k++) label_set(&s_kact[k], g_font_small, tr(k_act_key[k]));
   label_set(&s_ktitle, g_font_sub, tr(title_key));
   snprintf(s_text, sizeof(s_text), "%s", initial ? initial : "");
   s_lower = s_text[0] != 0;
   s_orow = 1; s_ocol = 0;
   s_osk_accept = accept;
   s_modal = s_modal_drawn = M_OSK;
   s_mod.x = 0; s_mod.v = 0;
   osk_snap();
}

static void osk_free(void)
{
   int k;
   for (k = 0; k < 40; k++) { label_free(&s_kup[k]); label_free(&s_klo[k]); }
   for (k = 0; k < 4; k++) label_free(&s_kact[k]);
   label_free(&s_ktitle); label_free(&s_ktext);
}

static void osk_type(const char *s)
{
   size_t len = strlen(s_text), add = strlen(s);
   int was_empty = len == 0;
   if (utf8_chars(s_text) >= OSK_MAX_CHARS || len + add > OSK_MAX_BYTES) return;
   memcpy(s_text + len, s, add + 1);
   if (was_empty && !s_lower && strcmp(s, " ")) s_lower = 1;   /* first letter upper case, then lower */
}

static void osk_erase(void)
{
   size_t len = strlen(s_text);
   while (len > 0 && ((unsigned char)s_text[len - 1] & 0xC0) == 0x80) len--;
   if (len > 0) len--;
   s_text[len] = 0;
   if (!len) s_lower = 0;
}

static void osk_ok(void)
{
   char name[OSK_MAX_BYTES + 4];
   const char *b = s_text, *err;
   size_t n;
   while (*b == ' ') b++;
   snprintf(name, sizeof(name), "%s", b);
   n = strlen(name);
   while (n > 0 && name[n - 1] == ' ') name[--n] = 0;
   if (!n) { panel_toast("profile.name_empty"); return; }
   err = s_osk_accept ? s_osk_accept(name) : NULL;
   if (err) { panel_toast(err); return; }
   s_modal = M_NONE;
}

static void osk_input(const ui_nav_t *nav)
{
   if (nav->options) { s_modal = M_NONE; return; }
   if (nav->start) { osk_ok(); return; }
   if (nav->back) osk_erase();
   if (nav->up) s_orow = (s_orow + 4) % 5;
   if (nav->down) s_orow = (s_orow + 1) % 5;
   if (nav->left || nav->right)
   {
      int d = nav->right ? 1 : -1;
      if (s_orow == 4) s_ocol = k_act_col[(osk_act_of_col(s_ocol) + d + 4) % 4];
      else s_ocol = (s_ocol + d + 10) % 10;
   }
   if (nav->confirm)
   {
      if (s_orow < 4)
      {
         int k = s_orow * 10 + s_ocol;
         osk_type(s_lower ? s_klower[k] : k_keys[k]);
      }
      else switch (osk_act_of_col(s_ocol))
      {
      case OK_SHIFT: s_lower = !s_lower; break;
      case OK_SPACE: osk_type(" "); break;
      case OK_ERASE: osk_erase(); break;
      case OK_OK:    osk_ok(); break;
      }
   }
}

/* draw helpers scaled about the screen centre (scale-in of the modals) */
static float s_mk = 1.0f;
static float mx(float x) { return W * 0.5f + (x - W * 0.5f) * s_mk; }
static float my(float y) { return H * 0.5f + (y - H * 0.5f) * s_mk; }
static void m_rect(float x, float y, float w, float h, float r, const float c[4], float b)
{
   draw_round_rect(mx(x), my(y), w * s_mk, h * s_mk, r * s_mk, c, b);
}
static void m_label(const label_t *l, float x, float y, const float c[4])
{
   if (l->tex && c[3] > 0.003f) draw_2d(l->tex, floorf(mx(x)), floorf(my(y)), l->w * s_mk, l->h * s_mk, c, 0);
}

static void osk_draw(double t, float a)
{
   float ox = (W - OSK_W) * 0.5f, oy = (H - OSK_H) * 0.5f, x, y, w, cx;
   float c[4], f[4];
   int k, r;
   col(f, (const float[3]){ 0.11f, 0.11f, 0.16f }, 0.94f * a);
   m_rect(ox, oy, OSK_W, OSK_H, 22, f, 0.14f * a);
   col(c, C_DIM, a);
   m_label(&s_ktitle, ox + 28, oy + 18, c);
   col(f, (const float[3]){ 0, 0, 0 }, 0.28f * a);
   m_rect(ox + 24, oy + 52, OSK_W - 48, 44, 10, f, 0.10f * a);
   label_set(&s_ktext, g_font_sub, s_text);
   col(c, C_TXT, a);
   m_label(&s_ktext, ox + 40, oy + 74 - s_ktext.h * 0.5f, c);
   if (fmod(t, 1.0) < 0.55)
   {
      col(c, C_ACC, a);
      m_rect(ox + 41 + s_ktext.w, oy + 61, 2, 26, 1, c, 0);
   }
   /* keys */
   for (r = 0; r < 5; r++)
      for (k = 0; k < (r < 4 ? 10 : 4); k++)
      {
         const label_t *l;
         osk_key_rect(r, k, &x, &y, &w);
         col(f, (const float[3]){ 1, 1, 1 }, ((r == 4 && k == OK_SHIFT && s_lower) ? 0.05f : 0.07f) * a);
         m_rect(x, y, w, 36, 9, f, 0.05f * a);
         if (r < 4) l = (s_lower && s_klo[r * 10 + k].tex) ? &s_klo[r * 10 + k] : &s_kup[r * 10 + k];
         else l = &s_kact[k];
         col(c, (r == 4 && k == OK_OK) ? C_ACC : C_TXT, a);
         if (r == 4 && k == OK_SHIFT && !s_lower) col(c, C_ACC, a);   /* upper case engaged */
         m_label(l, x + (w - l->w) * 0.5f, y + (36 - l->h) * 0.5f, c);
      }
   /* moving highlight */
   col(f, (const float[3]){ 1, 1, 1 }, 0.22f * a);
   cx = s_kx.x;
   m_rect(cx, s_ky.x, s_kw.x, 36, 9, f, 0.35f * a);
}

/* ------------------------------------------------------------------------ */
/* confirm dialog                                                            */
static void confirm_open(const char *message, confirm_fn on_yes)
{
   if (s_modal != M_NONE) return;
   if (s_modal_drawn == M_OSK) osk_free();
   wlabel_set(&s_cmsg, g_font_sub, message, (int)DLG_W - 60);
   wlabel_set(&s_cnote, g_font_small, tr("profile.delete_note"), (int)DLG_W - 60);
   label_set(&s_cbtn[0], g_font_sub, tr("confirm.cancel"));
   label_set(&s_cbtn[1], g_font_sub, tr("confirm.delete"));
   s_csel = 0; s_con_yes = on_yes;
   s_modal = s_modal_drawn = M_CONFIRM;
   s_mod.x = 0; s_mod.v = 0;
}

static void confirm_input(const ui_nav_t *nav)
{
   if (nav->left) s_csel = 0;
   if (nav->right) s_csel = 1;
   if (nav->back || nav->options) { s_modal = M_NONE; return; }
   if (nav->confirm)
   {
      s_modal = M_NONE;
      if (s_csel == 1 && s_con_yes) s_con_yes();
   }
}

static void confirm_draw(float a)
{
   float ox = (W - DLG_W) * 0.5f, oy = (H - DLG_H) * 0.5f, bw = (DLG_W - 80) * 0.5f, by = oy + DLG_H - 24 - 46;
   float c[4], f[4];
   int b;
   col(f, (const float[3]){ 0.11f, 0.11f, 0.16f }, 0.95f * a);
   m_rect(ox, oy, DLG_W, DLG_H, 22, f, 0.14f * a);
   col(c, C_TXT, a);
   m_label(&s_cmsg, ox + 30, oy + 26, c);
   col(c, C_DIM, a);
   m_label(&s_cnote, ox + 30, oy + 34 + s_cmsg.h, c);
   for (b = 0; b < 2; b++)
   {
      float bx = ox + 30 + b * (bw + 20);
      const float *tint = b ? C_RED : (const float[3]){ 1, 1, 1 };
      col(f, b ? (const float[3]){ 0.90f, 0.25f, 0.25f } : (const float[3]){ 1, 1, 1 },
          (s_csel == b ? (b ? 0.38f : 0.22f) : 0.07f) * a);
      m_rect(bx, by, bw, 46, 12, f, (s_csel == b ? 0.30f : 0.06f) * a);
      col(c, b ? tint : C_TXT, a);
      m_label(&s_cbtn[b], bx + (bw - s_cbtn[b].w) * 0.5f, by + (46 - s_cbtn[b].h) * 0.5f, c);
   }
}

/* ------------------------------------------------------------------------ */
/* public API                                                                */
void panel_init(void)
{
   int k;
   for (k = 0; k < 40; k++)
   {
      const char *u = k_keys[k];
      if (!strcmp(u, "\xC3\x91")) snprintf(s_klower[k], sizeof(s_klower[k]), "\xC3\xB1");
      else if (u[0] >= 'A' && u[0] <= 'Z' && !u[1]) { s_klower[k][0] = (char)(u[0] - 'A' + 'a'); s_klower[k][1] = 0; }
      else snprintf(s_klower[k], sizeof(s_klower[k]), "%s", u);
   }
   label_set(&s_lt, g_font_sub, "\xE2\x80\xB9");   /* ‹ */
   label_set(&s_gt, g_font_sub, "\xE2\x80\xBA");   /* › */
   s_slide.x = PW + 40.0f;
   s_page = P_ROOT;
   (void)osk_open; (void)confirm_open;   /* first reached from the Profile page (step 3) */
}

void panel_open(void)
{
   if (s_open) return;
   s_open = 1;
   if (s_slide.x > 540.0f) { s_slide.x = PW + 40.0f; s_slide.v = 0; }
   page_enter(P_ROOT, 0);
   logf_("panel: open");
}

int panel_active(void) { return s_open || s_slide.x <= 540.0f; }

float panel_amount(void) { return clampf(s_fade.x, 0.0f, 1.0f); }

int panel_hint_set(void)
{
   if (!s_open) return HINTS_CAROUSEL;
   if (s_modal == M_OSK) return HINTS_OSK;
   if (s_modal == M_CONFIRM) return HINTS_CONFIRM;
   return s_page == P_ROOT ? HINTS_PANEL_ROOT : HINTS_PANEL_PAGE;
}

void panel_update(const ui_nav_t *nav, double t, float dt)
{
   (void)t;
   if (s_open)
   {
      if (s_modal == M_OSK) osk_input(nav);
      else if (s_modal == M_CONFIRM) confirm_input(nav);
      else page_input(nav);
   }
   else if (s_modal != M_NONE) s_modal = M_NONE;

   spring_step(&s_slide, s_open ? 0.0f : PW + 40.0f, 170, 18, dt);
   spring_step(&s_fade, s_open ? 1.0f : 0.0f, 90, 14, dt);
   if (!s_open && s_slide.x > 540.0f && s_fade.x < 0.003f && s_modal_drawn == M_NONE) return;   /* fully closed */

   page_build();
   if (s_sel >= s_nrows || (s_sel >= 0 && !row_selectable(s_sel))) s_sel = first_selectable();
   spring_step(&s_px, 0.0f, 170, 18, dt);
   spring_step(&s_pa, 1.0f, 120, 16, dt);
   {
      /* keep the selected row inside the visible rows area */
      float view = PH - ROWS_Y - 24.0f - (s_footer.tex ? s_footer.h + 16.0f : 0.0f);
      float y = s_sel >= 0 ? s_sel * ROW_H : 0.0f, target = s_scroll.x;
      if (s_sel >= 0)
      {
         if (y < target) target = y;
         if (y + ROW_H > target + view) target = y + ROW_H - view;
      }
      if (target < 0) target = 0;
      spring_step(&s_hl, y, 220, 22, dt);
      spring_step(&s_scroll, target, 220, 22, dt);
   }

   /* modal */
   spring_step(&s_mod, s_modal != M_NONE ? 1.0f : 0.0f, 200, 20, dt);
   if (s_modal == M_OSK)
   {
      float x, y, w;
      osk_key_rect(s_orow, s_orow == 4 ? osk_act_of_col(s_ocol) : s_ocol, &x, &y, &w);
      spring_step(&s_kx, x, 320, 28, dt);
      spring_step(&s_ky, y, 320, 28, dt);
      spring_step(&s_kw, w, 320, 28, dt);
   }
   if (s_modal == M_NONE && s_modal_drawn != M_NONE && s_mod.x < 0.02f)
   {
      if (s_modal_drawn == M_OSK) osk_free();
      else { label_free(&s_cmsg); label_free(&s_cnote); label_free(&s_cbtn[0]); label_free(&s_cbtn[1]); }
      s_modal_drawn = M_NONE;
   }
}

void panel_draw(double t)
{
   float a = panel_amount(), px = PX0 + s_slide.x, py = PY0;
   float ca, cx, top, bottom, c[4], f[4];
   int i;
   if (s_slide.x > PW + 30.0f && a < 0.003f && s_modal_drawn == M_NONE) return;
   gl_use_2d();
   ca = a * clampf(s_pa.x, 0.0f, 1.0f);
   cx = px + s_px.x;

   col(f, (const float[3]){ 0.10f, 0.10f, 0.14f }, 0.78f * a);
   draw_round_rect(px, py, PW, PH, 22, f, 0.12f * a);

   label_set(&s_title, g_font_title, tr("panel.title"));
   col(c, C_TXT, a);
   draw_label(&s_title, px + PAD, py + 26, c);
   if (s_page != P_ROOT)
   {
      label_set(&s_ptitle, g_font_sub, tr(k_page_key[s_page]));
      col(c, C_DIM, ca);
      draw_label(&s_ptitle, cx + PAD, py + 76, c);
   }

   /* footer note */
   if (s_footer_key) wlabel_set(&s_footer, g_font_small, tr(s_footer_key), (int)(PW - 2 * PAD));
   else label_free(&s_footer);
   bottom = py + PH - 24.0f - (s_footer.tex ? s_footer.h + 16.0f : 0.0f);
   if (s_footer.tex)
   {
      col(c, C_DIM, ca);
      draw_label(&s_footer, cx + PAD, py + PH - 24.0f - s_footer.h, c);
   }

   /* rows, clipped to their area */
   top = py + ROWS_Y;
   {
      int sx = (int)floorf(px), sy = (int)floorf(H - bottom), sh = (int)(bottom - top);
      glEnable(GL_SCISSOR_TEST);
      glScissor(sx < 0 ? 0 : sx, sy, (int)PW, sh > 0 ? sh : 0);
   }
   if (s_sel >= 0)
   {
      col(f, (const float[3]){ 1, 1, 1 }, 0.10f * ca);
      draw_round_rect(cx + 16, top + s_hl.x - s_scroll.x + 3, PW - 32, ROW_H - 6, 12, f, 0.0f);
   }
   for (i = 0; i < s_nrows; i++)
   {
      const row_t *r = &s_rows[i];
      float y = top + i * ROW_H - s_scroll.x, xr = cx + PW - PAD, la = ca * (r->dim ? 0.4f : 1.0f);
      label_set(&s_rl[i], g_font_sub, r->label);
      label_set(&s_rv[i], g_font_sub, r->value);
      if (y + ROW_H < top || y > bottom) continue;
      col(c, C_TXT, la);
      draw_label(&s_rl[i], cx + PAD, y + (ROW_H - s_rl[i].h) * 0.5f, c);
      if (r->type == R_CHOICE)
      {
         col(c, C_TXT, la * (r->at_hi ? 0.25f : 0.8f));
         draw_label(&s_gt, xr - s_gt.w, y + (ROW_H - s_gt.h) * 0.5f, c);
         xr -= s_gt.w + 12;
         col(c, C_TXT, la);
         draw_label(&s_rv[i], xr - s_rv[i].w, y + (ROW_H - s_rv[i].h) * 0.5f, c);
         xr -= s_rv[i].w + 12;
         col(c, C_TXT, la * (r->at_lo ? 0.25f : 0.8f));
         draw_label(&s_lt, xr - s_lt.w, y + (ROW_H - s_lt.h) * 0.5f, c);
      }
      else
      {
         if (r->enter)
         {
            col(c, C_DIM, la);
            draw_label(&s_gt, xr - s_gt.w, y + (ROW_H - s_gt.h) * 0.5f, c);
            xr -= s_gt.w + 12;
         }
         if (r->check)
         {
            col(f, C_ACC, la);
            draw_round_rect(xr - 12, y + ROW_H * 0.5f - 6, 12, 12, 6, f, 0.0f);
            xr -= 12 + 12;
         }
         col(c, C_DIM, la);
         draw_label(&s_rv[i], xr - s_rv[i].w, y + (ROW_H - s_rv[i].h) * 0.5f, c);
      }
   }
   glDisable(GL_SCISSOR_TEST);

   /* modal over the panel */
   if (s_modal_drawn != M_NONE)
   {
      float m = clampf(s_mod.x, 0.0f, 1.0f);
      const float shade[4] = { 0, 0, 0, 0.35f * m };
      draw_2d(0, 0, 0, W, H, shade, 0);
      s_mk = 0.92f + 0.08f * s_mod.x;
      if (s_modal_drawn == M_OSK) osk_draw(t, m);
      else confirm_draw(m);
      s_mk = 1.0f;
   }
}

void panel_toast(const char *key)
{
   s_toast_key = key;
   s_toast_t = now_s();
   logf_("toast: %s", key);
}

void panel_toast_draw(double t)
{
   float e = (float)(t - s_toast_t), a, w, h = 44, x, y;
   float c[4], f[4];
   if (!s_toast_key || e < 0 || e > 2.5f) return;
   a = clampf(e / 0.15f, 0, 1) * clampf((2.5f - e) / 0.3f, 0, 1);
   gl_use_2d();
   label_set(&s_toast, g_font_sub, tr(s_toast_key));
   w = s_toast.w + 48;
   x = floorf((W - w) * 0.5f);
   y = 610 + (1.0f - a) * 8.0f;
   col(f, (const float[3]){ 0.10f, 0.10f, 0.14f }, 0.88f * a);
   draw_round_rect(x, y, w, h, h * 0.5f, f, 0.14f * a);
   col(c, C_TXT, a);
   draw_label(&s_toast, x + 24, y + (h - s_toast.h) * 0.5f, c);
}
