/* Settings panel, on-screen keyboard, confirm dialog and toast (docs/settings-design.md 6;
 * look and layout: lumen-design, settings.html / keyboard.html / confirm.html).
 *
 * Everything lives in static storage: the rows of the current page are rebuilt every
 * frame into fixed buffers (plain string copies, no heap), and every text on screen is a
 * label_t that re-renders only when its string changes. The keyboard renders its key
 * glyphs once when it opens and frees them when it has faded out. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include "lumen.h"
#include "music.h"
#include "panel.h"
#include "i18n.h"
#include "settings.h"
#include "profiles.h"
#include "version.h"
#include "theme.h"
#include "crt.h"
#include "filepicker.h"
#include "transfer.h"
#include "audit.h"
#include "stats.h"
#include "ui.h"

/* ------------------------------------------------------------------------ */
/* geometry and colours                                                      */
/* a full-screen section (opaque backdrop drawn by main.c under the help bar): the
 * content is a centred column inside the TV-safe area */
/* a section over the dimmed art (lumen-design, settings.html): the title and the rail of
 * sections on the left, one glass card with the focused section on the right */
#define TITLE_X 84.0f
#define TITLE_Y 112.0f
#define RAIL_X  70.0f
#define RAIL_Y  172.0f
#define RAIL_W  320.0f
#define RAIL_H  52.0f
#define RAIL_P  56.0f        /* rail pitch */
#define CARD_X  420.0f
#define CARD_Y  108.0f
#define CARD_W  776.0f
#define CARD_H  528.0f
#define HEAD_H  138.0f       /* card header: kicker, title, caption, hairline */
#define ROW_H   54.0f        /* row pitch; the pill is ROW_H - 4 high */
#define PAD     28.0f
#define SLIDE_OFF 560.0f     /* s_slide when closed: drives the open/close motion */
#define MAX_ROWS 20

/* the active theme's (theme.h), set in panel_init; these are Lumen's own */
static float C_TXT[3] = { 0.96f, 0.96f, 0.98f };
static float C_DIM[3] = { 0.62f, 0.63f, 0.70f };
static float C_ACC[3] = { 0.36f, 0.55f, 1.00f };
static float C_RED[3] = { 1.00f, 0.42f, 0.42f };

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
/* root rows are the pages in this order (page_input() relies on it) */
enum { P_ROOT, P_LANGUAGE, P_PROFILE, P_SCREENSAVER, P_SOUND, P_DISPLAY, P_GRAPHICS, P_LIBRARY, P_ABOUT, P_COUNT };
static const char *const k_page_key[P_COUNT] = {
   "panel.title", "section.language", "section.profile", "section.screensaver", "section.sound",
   "section.display", "section.graphics", "section.library", "section.about"
};

enum { R_ACTION, R_CHOICE, R_INFO };
enum { ID_LANG = 100, ID_PROFILE_ACTIVE = 200, ID_PROFILE_CREATE, ID_PROFILE_RENAME, ID_PROFILE_COLOR, ID_PROFILE_DELETE,
       ID_PROFILE_AVATAR, ID_PROFILE_AVATAR_REMOVE,
       ID_SS_AFTER = 300, ID_SS_DEMOS, ID_SS_LENGTH, ID_SS_PREVIEW, ID_GFX = 400,
       ID_SND_MUSIC = 600, ID_SND_VOLUME, ID_SND_UI, ID_LIB_STOCK = 500, ID_LIB_TRANSFER, ID_LIB_AUDIT,
       ID_DSP_OUTPUT = 700, ID_DSP_COMP, ID_DSP_SCANOUT, ID_DSP_RESTART, ID_DSP_THEME, ID_DSP_THEME_APPLY, ID_DSP_CRT, ID_INFO = 900 };

/* ---- Display page: output resolution (and direct scanout), applied by boot.sh at the
 * next boot (settings display / scanout). The compositor is not a choice: boot.sh runs
 * the newest PSC compositor installed in <home>/compositor/ (RetroArch 1.22 needs it)
 * and falls back to Sony's only if it does not start; "In use" shows which one runs. */
static const char *const k_comp_id[] = { "stock", "weston16" };
static const char *const k_comp_key[] = { "display.comp_stock", "display.comp_w16" };
static int comp_available(int i)
{
   char p[400];
   if (i == 0) return 1;
   snprintf(p, sizeof(p), "%s/compositor/psc-weston-16.tar.gz", g_home);
   return access(p, R_OK) == 0;
}
/* the compositor boot.sh will start: Lumen's when installed */
static int comp_setting(void)
{
   return comp_available(1) ? 1 : 0;
}
/* what is running now: boot.sh exports LUMEN_COMPOSITOR and LUMEN_DISPLAY */
static int comp_in_use(void)
{
   const char *c = getenv("LUMEN_COMPOSITOR");
   int i;
   for (i = 0; c && i < 2; i++) if (!strcmp(c, k_comp_id[i])) return i;
   return 0;
}
static int out_1080(void) { return !strcmp(settings_get("display", "720p"), "1080p"); }
static int out_in_use_1080(void) { const char *d = getenv("LUMEN_DISPLAY"); return d ? !strcmp(d, "1080p") : g_ui_scale > 1.2f; }
static int display_pending(void)
{
   return out_1080() != out_in_use_1080() || comp_setting() != comp_in_use();
}
static void restart_yes(void) { ui_request_reboot(); }
static void theme_apply_yes(void) { ui_request_restart(); }
/* index of the theme chosen in the settings (may differ from the one in use until a restart) */
static int theme_setting_index(void)
{
   const char *want = settings_get("theme", "default");
   int i;
   for (i = 0; i < theme_count(); i++) if (!strcmp(theme_id(i), want)) return i;
   return 0;
}

typedef struct {
   int type, id, enabled, dim, check, enter, at_lo, at_hi;
   int swatch;                  /* profile colour drawn left of the value, -1 none */
   int swatches;                /* colour row: draw the 8 swatches, this one selected */
   int avatar;                  /* the active profile's picture drawn left of the value */
   const char *why;             /* disabled row: tr key shown as a toast on Cross */
   char label[96], value[96];
} row_t;

static row_t   s_rows[MAX_ROWS];
static int     s_nrows;
static char    s_foot[400];            /* page footer text (translated), "" = none */

static int     s_open, s_page = 1, s_sel;
static int     s_in_card;              /* focus: 0 the rail of sections, 1 the rows of the card */
static spring  s_rail;                 /* rail highlight, y offset */
static int     s_osk_only;             /* keyboard opened from the game menu, panel hidden */
static spring  s_slide, s_fade, s_px, s_pa, s_hl, s_scroll;
static long    s_free_tenths = -1;     /* About: free space in tenths of GB, or */
static long    s_free_mb = -1;         /* in MB below 1 GB; measured when the page opens */

static label_t s_title, s_ptitle, s_footer, s_lt, s_gt, s_kick, s_cap, s_rail_l[16];
static label_t s_rl[MAX_ROWS], s_rv[MAX_ROWS];

/* Profile page: colour being chosen, written to profile.ini when it is committed
 * (leaving the page, closing the panel or any other profile action), not on every press */
static int s_pcolor = -1;

/* screensaver choices */
static const int k_minutes[] = { 0, 1, 2, 3, 5, 10, 15, 30 };
static const int k_seconds[] = { 30, 45, 60, 90, 120, 180 };

/* menu music: settings "music" (0/1) and "music_volume" (0..100, steps of 10) */
#define VOL_STEP 10
static int music_on(void) { return settings_get_int("music", 1, 0, 1); }
static int music_volume(void) { return (settings_get_int("music_volume", 60, 0, 100) + VOL_STEP / 2) / VOL_STEP * VOL_STEP; }
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
static int     s_modal, s_modal_drawn;
static int     s_sfx = -1;         /* sound the input of this frame asked for (panel_update plays it) */  /* open modal / modal still fading out */
static spring  s_mod;

/* keyboard (6.6) */
typedef const char *(*osk_accept_fn)(const char *text);  /* NULL = accepted, else a tr key to show */
#define OSK_W 760.0f
#define OSK_H 340.0f
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
static char        s_ktitle_txt[96];
static char        s_text[OSK_MAX_BYTES + 4];
static int         s_orow, s_ocol, s_lower;
static osk_accept_fn s_osk_accept;
static spring      s_kx, s_ky, s_kw;

/* confirm (6.7) */
typedef void (*confirm_fn)(void);
#define DLG_W 580.0f
#define DLG_H 228.0f
static label_t     s_cmsg, s_cnote, s_cbtn[2];
static int         s_csel;              /* 0 cancel (default), 1 delete */
static confirm_fn  s_con_yes;
static char        s_cbuf[300];         /* confirm message */

static void osk_open(const char *title_key, const char *initial, osk_accept_fn accept);
static void confirm_open(const char *message, confirm_fn on_yes);
static void confirm_open_ex(const char *message, const char *note_key, const char *yes_key, confirm_fn on_yes);

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
   r->type = type; r->id = id; r->enabled = 1; r->swatch = -1;
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

/* graphics.<system id>: 0 = original (default and any other value), 1 = enhanced,
 * 2 = native (internal resolution of the display; only systems that declare it) */
static void gfx_key(char *buf, size_t n, int plat) { snprintf(buf, n, "graphics.%s", g_plat[plat].id); }

static int gfx_level(int plat)
{
   char k[48];
   const char *v;
   gfx_key(k, sizeof(k), plat);
   v = settings_get(k, "original");
   if (!strcmp(v, "native")) return g_plat[plat].has_native ? 2 : 1;
   return !strcmp(v, "enhanced");
}
static const char *const gfx_names[3] = { "original", "enhanced", "native" };
static const char *const gfx_labels[3] = { "gfx.original", "gfx.enhanced", "gfx.native" };

static void page_build(void)
{
   char v[96], num[32];
   row_t *r;
   int i;
   s_nrows = 0; s_foot[0] = 0;
   switch (s_page)
   {
   case P_ROOT:
      i = lang_active_index();
      r = row_add(R_ACTION, P_LANGUAGE, tr("section.language"), i >= 0 ? i18n_name(i) : i18n_language());
      if (r) r->enter = 1;
      r = row_add(R_ACTION, P_PROFILE, tr("section.profile"), profiles_display_name(profiles_active()));
      if (r) { r->enter = 1; r->swatch = profiles_active()->color; }
      minutes_text(v, sizeof(v), settings_get_int("screensaver_minutes", 5, 0, 60));
      r = row_add(R_ACTION, P_SCREENSAVER, tr("section.screensaver"), v);
      if (r) r->enter = 1;
      r = row_add(R_ACTION, P_SOUND, tr("section.sound"), tr(music_on() ? "value.on" : "value.off"));
      if (r) r->enter = 1;
      snprintf(v, sizeof(v), "%s  \xC2\xB7  %s", out_in_use_1080() ? "1080p" : "720p", tr(k_comp_key[comp_in_use()]));
      r = row_add(R_ACTION, P_DISPLAY, tr("section.display"), v);
      if (r) r->enter = 1;
      r = row_add(R_ACTION, P_GRAPHICS, tr("section.graphics"), NULL);
      if (r) r->enter = 1;
      r = row_add(R_ACTION, P_LIBRARY, tr("section.library"), NULL);
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
   {
      const profile_t *ap = profiles_active();
      int ai = profiles_find(ap->id), np = profiles_count(), color = s_pcolor >= 0 ? s_pcolor : ap->color;
      if ((r = row_add(R_CHOICE, ID_PROFILE_ACTIVE, tr("profile.active"), profiles_display_name(ap))))
      { r->swatch = color; r->at_lo = ai <= 0; r->at_hi = ai >= np - 1; }
      snprintf(num, sizeof(num), "%d", np);
      if ((r = row_add(R_ACTION, ID_PROFILE_CREATE, tr("profile.create"), trf(v, sizeof(v), "profile.count", "n", num, NULL))) && np >= MAX_PROFILES)
      { r->enabled = 0; r->dim = 1; r->why = "profile.full"; }
      row_add(R_ACTION, ID_PROFILE_RENAME, tr("profile.rename"), NULL);
      if ((r = row_add(R_CHOICE, ID_PROFILE_COLOR, tr("profile.color"), NULL)))
      { r->swatches = 1; r->swatch = color; r->at_lo = color <= 0; r->at_hi = color >= PROFILE_COLORS - 1; }
      /* avatar: a picture picked from the stick (file picker), else the initials on the colour */
      if ((r = row_add(R_ACTION, ID_PROFILE_AVATAR, tr("profile.avatar"), tr(ap->avatar ? "profile.avatar_image" : "profile.avatar_initials"))))
      { r->enter = 1; if (ap->avatar) r->avatar = 1; else r->swatch = color; }
      if ((r = row_add(R_ACTION, ID_PROFILE_AVATAR_REMOVE, tr("profile.avatar_remove"), NULL)) && !ap->avatar)
      { r->enabled = 0; r->dim = 1; r->why = "profile.avatar_none"; }
      if ((r = row_add(R_ACTION, ID_PROFILE_DELETE, tr("profile.delete"), NULL)) && !strcmp(ap->id, "main"))
      { r->enabled = 0; r->dim = 1; r->why = "profile.main_locked"; }
      break;
   }
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
      {
         r->at_lo = s == 0; r->at_hi = s == NELEM(k_seconds) - 1; r->dim = !demos; r->enabled = demos;
         if (!demos) r->why = "ss.demo_length_off";
      }
      row_add(R_ACTION, ID_SS_PREVIEW, tr("ss.preview"), NULL);
      snprintf(s_foot, sizeof(s_foot), "%s", tr("ss.note"));
      break;
   }
   case P_DISPLAY:
   {
      int hd = out_1080(), sc = settings_get_int("scanout", 1, 0, 1);
      if ((r = row_add(R_CHOICE, ID_DSP_OUTPUT, tr("display.output"), hd ? "1080p" : "720p")))
      { r->at_lo = !hd; r->at_hi = hd; }
      if ((r = row_add(R_CHOICE, ID_DSP_SCANOUT, tr("display.scanout"), tr(sc ? "value.on" : "value.off"))))
      {
         r->at_lo = !sc; r->at_hi = sc;
         if (comp_in_use() == 0) { r->enabled = 0; r->dim = 1; r->why = "display.scanout_stock"; }
      }
      snprintf(v, sizeof(v), "%s  \xC2\xB7  %s", out_in_use_1080() ? "1080p" : "720p", tr(k_comp_key[comp_in_use()]));
      row_add(R_INFO, ID_INFO, tr("display.in_use"), v);
      if ((r = row_add(R_ACTION, ID_DSP_RESTART, tr("display.restart"), NULL)) && !display_pending())
      { r->enabled = 0; r->dim = 1; r->why = "display.nothing_pending"; }
      {  /* theme: chosen here, applied by restarting Lumen (fonts and art load at start) */
         int ti = theme_setting_index();
         if ((r = row_add(R_CHOICE, ID_DSP_THEME, tr("display.theme"), theme_name(ti))))
         { r->at_lo = ti <= 0; r->at_hi = ti >= theme_count() - 1; if (theme_count() < 2) { r->enabled = 0; r->dim = 1; r->why = "display.no_themes"; } }
         if ((r = row_add(R_ACTION, ID_DSP_THEME_APPLY, tr("display.theme_apply"), NULL)) && ti == theme_active_index())
         { r->enabled = 0; r->dim = 1; r->why = "display.theme_in_use"; }
      }
      {  /* CRT filter: the dashboard changes at once, games from their next launch */
         static const char *const k_crt_key[CRT_LEVELS] = { "value.off", "display.crt_soft", "display.crt_strong" };
         int cl = crt_level();
         if ((r = row_add(R_CHOICE, ID_DSP_CRT, tr("display.crt"), tr(k_crt_key[cl]))))
         { r->at_lo = cl == CRT_OFF; r->at_hi = cl == CRT_LEVELS - 1; }
      }
      snprintf(s_foot, sizeof(s_foot), "%s", tr("display.note"));
      break;
   }
   case P_SOUND:
   {
      int on = music_on(), vol = music_volume();
      if ((r = row_add(R_CHOICE, ID_SND_MUSIC, tr("sound.music"), tr(on ? "value.on" : "value.off"))))
      { r->at_lo = !on; r->at_hi = on; }
      snprintf(num, sizeof(num), "%d", vol);
      trf(v, sizeof(v), "sound.vol_value", "n", num, NULL);
      if ((r = row_add(R_CHOICE, ID_SND_VOLUME, tr("sound.volume"), v)))
      { r->at_lo = vol <= 0; r->at_hi = vol >= 100; }
      on = settings_get_int("ui_sounds", 1, 0, 1);
      if ((r = row_add(R_CHOICE, ID_SND_UI, tr("sound.ui"), tr(on ? "value.on" : "value.off"))))
      { r->at_lo = !on; r->at_hi = on; }
      snprintf(s_foot, sizeof(s_foot), "%s", tr("sound.note"));
      break;
   }
   case P_GRAPHICS:
      for (i = 0; i < g_nplat; i++)
      {
         int lv;
         if (!g_plat[i].has_gfx) continue;
         lv = gfx_level(i);
         if ((r = row_add(R_CHOICE, ID_GFX + i, g_plat[i].name, tr(gfx_labels[lv]))))
         { r->at_lo = lv == 0; r->at_hi = lv == (g_plat[i].has_native ? 2 : 1); }
      }
      snprintf(s_foot, sizeof(s_foot), "%s", tr(s_nrows ? "gfx.note" : "gfx.none"));
      break;
   case P_LIBRARY:
   {
      int stock = settings_get_int("show_stock_games", 0, 0, 1);
      if ((r = row_add(R_CHOICE, ID_LIB_STOCK, tr("lib.stock"), tr(stock ? "lib.visible" : "lib.hidden"))))
      { r->at_lo = !stock; r->at_hi = stock; }
      if ((r = row_add(R_ACTION, ID_LIB_AUDIT, tr("audit.title"), NULL))) r->enter = 1;
      if ((r = row_add(R_ACTION, ID_LIB_TRANSFER, tr("xfer.title"), NULL))) r->enter = 1;
      snprintf(s_foot, sizeof(s_foot), "%s", tr("lib.stock_note"));
      break;
   }
   case P_ABOUT:
      row_add(R_INFO, ID_INFO, tr("about.version"), LUMEN_VERSION);
      snprintf(num, sizeof(num), "%d", g_ngames);
      row_add(R_INFO, ID_INFO, tr("about.games"), num);
      snprintf(num, sizeof(num), "%d", g_nplat);
      row_add(R_INFO, ID_INFO, tr("about.systems"), num);
      snprintf(num, sizeof(num), "%d", profiles_count());
      row_add(R_INFO, ID_INFO, tr("about.profiles"), num);
      if (s_free_tenths >= 0)
      {
         /* the decimal separator is translated ("num.decimal": "," in es/pt/fr/de/it) */
         snprintf(num, sizeof(num), "%ld%s%ld", s_free_tenths / 10, tr("num.decimal"), s_free_tenths % 10);
         row_add(R_INFO, ID_INFO, tr("about.free"), trf(v, sizeof(v), "about.gb", "n", num, NULL));
      }
      else if (s_free_mb >= 0)
      {
         snprintf(num, sizeof(num), "%ld", s_free_mb);
         row_add(R_INFO, ID_INFO, tr("about.free"), trf(v, sizeof(v), "about.mb", "n", num, NULL));
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
   s_free_tenths = s_free_mb = -1;
   if (statvfs(g_root, &st) == 0)
   {
      double b = (double)st.f_bavail * (double)st.f_frsize;
      if (b >= 1024.0 * 1024.0 * 1024.0) s_free_tenths = (long)(b / (1024.0 * 1024.0 * 1024.0) * 10.0 + 0.5);
      else s_free_mb = (long)(b / (1024.0 * 1024.0) + 0.5);
   }
}

/* place the highlight and the scroll without animation (page entry) */
static void rows_snap(void)
{
   s_hl.x = s_sel > 0 ? s_sel * ROW_H : 0.0f; s_hl.v = 0;
   s_scroll.x = 0; s_scroll.v = 0;
   s_rail.x = (s_page - 1) * RAIL_P; s_rail.v = 0;
}

static void page_enter(int page, int dir)
{
   int i;
   s_page = page;
   if (page == P_ABOUT) measure_free();
   page_build();
   s_sel = first_selectable();
   if (page == P_LANGUAGE && (i = lang_active_index()) >= 0) s_sel = i;
   s_hl.x = s_sel > 0 ? s_sel * ROW_H : 0.0f; s_hl.v = 0;
   s_scroll.x = 0; s_scroll.v = 0;
   s_px.x = dir * 24.0f; s_px.v = 0;
   s_pa.x = 0.0f; s_pa.v = 0;
}

static void color_commit(void)
{
   if (s_pcolor < 0) return;
   if (profiles_set_color(profiles_active()->id, s_pcolor) != 0) panel_toast("error.profile");
   s_pcolor = -1;
}

static void panel_close(void)
{
   if (!s_open) return;
   color_commit();
   s_open = 0; s_in_card = 0;
   logf_("panel: close");
   if (settings_dirty() && settings_save() != 0) panel_toast("error.save");
}

/* the frontend is exiting (launch, SDL_QUIT on power-off / SIGTERM) with the panel
 * maybe still open: commit what a close would. Text typed in the keyboard is not
 * accepted (it was never confirmed with Done), like a cancel. */
void panel_shutdown(void)
{
   s_modal = M_NONE;
   panel_close();
   color_commit();
}

/* ---- Profile page actions (7.4): any failure shows a toast ---- */
static void select_row(int id)
{
   int i;
   for (i = 0; i < s_nrows; i++) if (s_rows[i].id == id) { s_sel = i; return; }
}

static const char *profile_error(int rc)
{
   return rc == -1 ? "profile.full" : rc == -2 ? "profile.name_taken" : "error.profile";
}

static const char *accept_create(const char *name)
{
   char id[16];
   int rc = profiles_create(name, -1, id, sizeof(id));
   if (rc != 0) return profile_error(rc);
   if (profiles_set_active(id) != 0) panel_toast("error.profile");
   ui_cards_changed();
   return NULL;
}

static const char *accept_rename(const char *name)
{
   int rc = profiles_rename(profiles_active()->id, name);
   if (rc != 0) return profile_error(rc);
   ui_cards_changed();
   return NULL;
}

static void delete_yes(void)
{
   char id[16];
   snprintf(id, sizeof(id), "%s", profiles_active()->id);
   if (profiles_delete(id) != 0) panel_toast("error.profile");
   ui_cards_changed();
   page_build();
   select_row(ID_PROFILE_ACTIVE);
}

/* avatar: the picker hands the chosen image to avatar_accept on its worker thread (decode,
 * crop, scale, write avatar.png); avatar_done then refreshes what is on screen */
static char s_av_id[16];
static const char *avatar_accept(const char *path, void *user)
{
   (void)user;
   return profiles_set_avatar(s_av_id, path) == 0 ? NULL : "profile.avatar_error";
}
static void avatar_done(const char *path, void *user)
{
   (void)path; (void)user;
   profiles_avatar_refresh(s_av_id);
   ui_avatar_changed();
   panel_toast("profile.avatar_set");
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
   else if (r->id == ID_PROFILE_CREATE)
   {
      char name[64], num[8];
      int k;
      color_commit();
      for (k = profiles_count() + 1; k < 100; k++)
      {
         snprintf(num, sizeof(num), "%d", k);
         trf(name, sizeof(name), "profile.default_name", "n", num, NULL);
         if (!profiles_name_taken(name, NULL)) break;
      }
      osk_open("profile.name_title", name, accept_create);
   }
   else if (r->id == ID_PROFILE_RENAME)
   {
      char name[64];
      color_commit();
      snprintf(name, sizeof(name), "%s", profiles_display_name(profiles_active()));
      osk_open("profile.name_title", name, accept_rename);
   }
   else if (r->id == ID_PROFILE_DELETE)
   {
      color_commit();
      trf(s_cbuf, sizeof(s_cbuf), "profile.delete_confirm", "name", profiles_display_name(profiles_active()), NULL);
      confirm_open(s_cbuf, delete_yes);
   }
   else if (r->id == ID_PROFILE_AVATAR)
   {
      color_commit();
      snprintf(s_av_id, sizeof(s_av_id), "%s", profiles_active()->id);
      filepicker_open(&(filepicker_t){ .id = "avatar", .title_key = "profile.avatar_title", .exts = FP_EXTS_IMAGE,
                                       .empty_key = "profile.avatar_empty", .busy_key = "profile.avatar_saving",
                                       .preview = FP_PREVIEW_ROUND, .accept = avatar_accept, .done = avatar_done });
   }
   else if (r->id == ID_PROFILE_AVATAR_REMOVE)
   {
      color_commit();
      if (profiles_clear_avatar(profiles_active()->id) != 0) panel_toast("error.profile");
      else { ui_avatar_changed(); panel_toast("profile.avatar_removed"); select_row(ID_PROFILE_AVATAR); }
   }
   else if (r->id == ID_DSP_THEME_APPLY)
   {
      confirm_open_ex(tr("display.theme_confirm"), NULL, "confirm.restart", theme_apply_yes);
   }
   else if (r->id == ID_DSP_RESTART)
   {
      confirm_open_ex(tr("display.restart_confirm"), NULL, "confirm.restart", restart_yes);
   }
   else if (r->id == ID_LIB_AUDIT)
   {
      audit_open();
   }
   else if (r->id == ID_LIB_TRANSFER)
   {
      transfer_open();   /* a toast when no second drive is plugged in */
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
   case ID_PROFILE_ACTIVE:
   {
      const profile_t *p = profiles_get(profiles_find(profiles_active()->id) + dir);
      color_commit();
      if (!p) break;
      if (profiles_set_active(p->id) != 0) panel_toast("error.profile");
      ui_cards_changed();
      break;
   }
   case ID_PROFILE_COLOR:
      i = (s_pcolor >= 0 ? s_pcolor : profiles_active()->color) + dir;
      if (i >= 0 && i < PROFILE_COLORS) s_pcolor = i;
      break;
   case ID_SS_AFTER:
      i = nearest(k_minutes, NELEM(k_minutes), settings_get_int("screensaver_minutes", 5, 0, 60)) + dir;
      if (i >= 0 && i < NELEM(k_minutes)) settings_set_int("screensaver_minutes", k_minutes[i]);
      break;
   case ID_SS_DEMOS:
      settings_set_int("demo_enabled", dir > 0);
      break;
   case ID_LIB_STOCK:   /* the libraries are rescanned live by main.c */
      settings_set_int("show_stock_games", dir > 0);
      ui_library_changed();
      logf_("panel: show_stock_games %d", dir > 0);
      break;
   case ID_SS_LENGTH:
      i = nearest(k_seconds, NELEM(k_seconds), settings_get_int("demo_seconds", 60, 20, 300)) + dir;
      if (i >= 0 && i < NELEM(k_seconds)) settings_set_int("demo_seconds", k_seconds[i]);
      break;
   case ID_SND_MUSIC:
      settings_set_int("music", dir > 0);
      ui_sound_changed();
      break;
   case ID_SND_UI:
      settings_set_int("ui_sounds", dir > 0);
      ui_sound_changed();
      break;
   case ID_DSP_OUTPUT:
      settings_set("display", dir > 0 ? "1080p" : "720p");
      break;
   case ID_DSP_SCANOUT:
      settings_set_int("scanout", dir > 0);
      break;
   case ID_DSP_CRT:
      i = crt_level() + dir;
      if (i >= CRT_OFF && i < CRT_LEVELS) { settings_set("crt", crt_id(i)); logf_("panel: crt %s", crt_id(i)); }
      break;
   case ID_DSP_THEME:
      i = theme_setting_index() + dir;
      if (i >= 0 && i < theme_count()) settings_set("theme", theme_id(i));
      break;
   case ID_SND_VOLUME:
      i = music_volume() + dir * VOL_STEP;
      if (i >= 0 && i <= 100) { settings_set_int("music_volume", i); ui_sound_changed(); }
      break;
   default:
      if (r->id >= ID_GFX && r->id < ID_GFX + g_nplat)
      {
         char k[48];
         int p = r->id - ID_GFX, lv = gfx_level(p) + (dir > 0 ? 1 : -1), top = g_plat[p].has_native ? 2 : 1;
         gfx_key(k, sizeof(k), p);
         settings_set(k, gfx_names[lv < 0 ? 0 : (lv > top ? top : lv)]);
      }
      break;
   }
}

/* Cross on a choice row: two-state rows toggle, longer lists step forward and wrap */
static void row_cycle(const row_t *r)
{
   if (!r->enabled) return;
   if (!r->at_hi) { row_change(r, 1); return; }
   switch (r->id)
   {
   case ID_PROFILE_ACTIVE:
   {
      const profile_t *p = profiles_get(0);
      color_commit();
      if (p && profiles_set_active(p->id) == 0) ui_cards_changed();
      break;
   }
   case ID_PROFILE_COLOR: s_pcolor = 0; break;
   case ID_SS_AFTER:  settings_set_int("screensaver_minutes", k_minutes[0]); break;
   case ID_SS_LENGTH: settings_set_int("demo_seconds", k_seconds[0]); break;
   case ID_DSP_CRT:   settings_set("crt", crt_id(CRT_OFF)); break;
   case ID_SND_VOLUME: settings_set_int("music_volume", 0); ui_sound_changed(); break;
   default:
      if (r->id >= ID_GFX && r->id < ID_GFX + g_nplat)
      {  /* Original / Mejorado (/ Nativa): wrap to the first */
         char k[48];
         gfx_key(k, sizeof(k), r->id - ID_GFX);
         settings_set(k, gfx_names[0]);
      }
      else row_change(r, -1);   /* Sí/No */
      break;
   }
}

static void page_input(const ui_nav_t *nav)
{
   const row_t *r = row_selectable(s_sel) ? &s_rows[s_sel] : NULL;
   if (!s_in_card)
   {  /* the rail: Up / Down change the section shown in the card, Cross / Right enter it */
      int to = s_page + (nav->down ? 1 : 0) - (nav->up ? 1 : 0);
      if (to != s_page && to >= 1 && to < P_COUNT) page_enter(to, 0);
      else if ((nav->confirm || nav->right) && first_selectable() >= 0) s_in_card = 1;
      else if (nav->back || nav->options) panel_close();
      return;
   }
   if (nav->up) sel_move(-1);
   if (nav->down) sel_move(1);
   if (r && r->type == R_CHOICE && (nav->left || nav->right))
   {
      if (!r->enabled && r->why) panel_toast(r->why);
      else
      {
         if (r->enabled && !(nav->right ? r->at_hi : r->at_lo)) s_sfx = SFX_CONFIRM;   /* not at its end */
         row_change(r, nav->right ? 1 : -1);
      }
   }
   if (nav->options) { panel_close(); return; }   /* Triangle: out of the whole flow */
   if (nav->confirm && r && !r->enabled && r->why) panel_toast(r->why);
   else if (nav->confirm && r && r->type == R_ACTION && r->enabled) { s_sfx = SFX_CONFIRM; row_action(r); }
   else if (nav->confirm && r && r->type == R_CHOICE && r->enabled) { s_sfx = SFX_CONFIRM; row_cycle(r); }
   else if (nav->back || (nav->left && !(r && r->type == R_CHOICE)))
   {  /* back to the rail */
      color_commit();
      s_in_card = 0;
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
   snprintf(s_ktitle_txt, sizeof(s_ktitle_txt), "%s", tr(title_key));
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

/* accent layer: L1 / R1 cycle the last typed letter through its accented forms
 * (E -> É -> È -> Ê -> Ë -> E), keeping its case; upper / lower pairs per form */
static const char *const k_accents[][16] = {
   { "A", "a", "\xC3\x81", "\xC3\xA1", "\xC3\x80", "\xC3\xA0", "\xC3\x82", "\xC3\xA2", "\xC3\x84", "\xC3\xA4", "\xC3\x83", "\xC3\xA3", NULL },
   { "E", "e", "\xC3\x89", "\xC3\xA9", "\xC3\x88", "\xC3\xA8", "\xC3\x8A", "\xC3\xAA", "\xC3\x8B", "\xC3\xAB", NULL },
   { "I", "i", "\xC3\x8D", "\xC3\xAD", "\xC3\x8C", "\xC3\xAC", "\xC3\x8E", "\xC3\xAE", "\xC3\x8F", "\xC3\xAF", NULL },
   { "O", "o", "\xC3\x93", "\xC3\xB3", "\xC3\x92", "\xC3\xB2", "\xC3\x94", "\xC3\xB4", "\xC3\x96", "\xC3\xB6", "\xC3\x95", "\xC3\xB5", NULL },
   { "U", "u", "\xC3\x9A", "\xC3\xBA", "\xC3\x99", "\xC3\xB9", "\xC3\x9B", "\xC3\xBB", "\xC3\x9C", "\xC3\xBC", NULL },
   { "C", "c", "\xC3\x87", "\xC3\xA7", NULL },
   { "N", "n", "\xC3\x91", "\xC3\xB1", NULL },
   { "S", "s", "\xC3\x9F", "\xC3\x9F", NULL },   /* ß has no Latin-1 capital */
   { "Y", "y", "\xC3\x9D", "\xC3\xBD", NULL },
};

static void osk_accent(int dir)
{
   size_t len = strlen(s_text), start = len;
   int g, i, n, lo, found = -1, fg = 0;
   if (!len) return;
   while (start > 0 && ((unsigned char)s_text[start - 1] & 0xC0) == 0x80) start--;
   if (start > 0) start--;
   for (g = 0; g < NELEM(k_accents) && found < 0; g++)
      for (i = 0; k_accents[g][i]; i++)
         if (!strcmp(k_accents[g][i], s_text + start)) { found = i; fg = g; break; }
   if (found < 0) return;
   for (n = 0; k_accents[fg][n]; n++) {}
   lo = found & 1;
   if (!strcmp(k_accents[fg][found & ~1], k_accents[fg][(found & ~1) + 1])) lo = s_lower;   /* ß */
   i = ((found / 2 + dir + n / 2) % (n / 2)) * 2 + lo;
   if (start + strlen(k_accents[fg][i]) > OSK_MAX_BYTES) return;
   snprintf(s_text + start, sizeof(s_text) - start, "%s", k_accents[fg][i]);
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
   /* Square (and Backspace) erase; Circle cancels like Triangle and like ○ everywhere
    * else in the panel */
   if (nav->options || (nav->back && !nav->erase)) { s_modal = M_NONE; return; }
   if (nav->start) { s_sfx = SFX_CONFIRM; osk_ok(); return; }
   if (nav->erase) osk_erase();
   if (nav->l1) osk_accent(-1);
   if (nav->r1) osk_accent(1);
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
      s_sfx = SFX_CONFIRM;
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

/* modal draw helpers: a modal rises 16 px into place while it fades in */
static float s_mrise;
static float mx(float x) { return x; }
static float my(float y) { return y + s_mrise; }
static void m_label(const label_t *l, float x, float y, const float c[4])
{
   if (l->tex && c[3] > 0.003f) draw_2d(l->tex, floorf(mx(x)), floorf(my(y)), l->w, l->h, c, 0);
}

static void osk_draw(double t, float a)
{
   float ox = (W - OSK_W) * 0.5f, oy = (H - OSK_H) * 0.5f, x, y, w;
   float c[4];
   const float black[3] = { 0, 0, 0 }, white[3] = { 1, 1, 1 }, ink[4] = { 0.043f, 0.047f, 0.063f, a };
   int k, r;
   ui_card(mx(ox), my(oy), OSK_W, OSK_H, a);
   ui_kicker(&s_ktitle, s_ktitle_txt, mx(ox + 28), my(oy + 22), a);
   ui_fill(mx(ox + 24), my(oy + 50), OSK_W - 48, 48, 14, black, 0.30f * a);
   label_set(&s_ktext, g_font_medium, s_text);
   col(c, C_TXT, a);
   m_label(&s_ktext, ox + 42, oy + 74 - s_ktext.h * 0.5f, c);
   if (fmod(t, 1.0) < 0.55) ui_fill(mx(ox + 44 + s_ktext.w), my(oy + 61), 2, 26, 1, C_ACC, a);
   /* keys: glass, the last one (Done) is the white primary */
   for (r = 0; r < 5; r++)
      for (k = 0; k < (r < 4 ? 10 : 4); k++)
      {
         const label_t *l;
         const int primary = r == 4 && k == OK_OK;
         osk_key_rect(r, k, &x, &y, &w);
         if (primary) ui_fill(mx(x), my(y), w, 36, 10, white, a);
         else ui_fill(mx(x), my(y), w, 36, 10, white, ((r == 4 && k == OK_SHIFT && !s_lower) ? UI_SELECTED : UI_REST) * a);
         if (r < 4) l = (s_lower && s_klo[r * 10 + k].tex) ? &s_klo[r * 10 + k] : &s_kup[r * 10 + k];
         else l = &s_kact[k];
         col(c, C_TXT, a);
         m_label(l, x + (w - l->w) * 0.5f, y + (36 - l->h) * 0.5f, primary ? ink : c);
      }
   /* the focused key: brighter glass and the ring, travelling with a spring */
   if (!(s_orow == 4 && osk_act_of_col(s_ocol) == OK_OK)) ui_fill(mx(s_kx.x), my(s_ky.x), s_kw.x, 36, 10, white, (UI_SELECTED - UI_REST) * a);
   ui_ring(mx(s_kx.x), my(s_ky.x), s_kw.x, 36, 10, 2.0f, a);
}

/* ------------------------------------------------------------------------ */
/* confirm dialog                                                            */
/* note_key NULL = no note; yes_key = the label of the confirming button */
static void confirm_open_ex(const char *message, const char *note_key, const char *yes_key, confirm_fn on_yes)
{
   if (s_modal != M_NONE) return;
   if (s_modal_drawn == M_OSK) osk_free();
   wlabel_set(&s_cmsg, g_font_medium, message, (int)DLG_W - 64);
   if (note_key) wlabel_set(&s_cnote, g_font_small, tr(note_key), (int)DLG_W - 60);
   else label_free(&s_cnote);
   label_set(&s_cbtn[0], g_font_medium, tr("confirm.cancel"));
   label_set(&s_cbtn[1], g_font_medium, tr(yes_key));
   s_csel = 0; s_con_yes = on_yes;
   s_modal = s_modal_drawn = M_CONFIRM;
   s_mod.x = 0; s_mod.v = 0;
}
static void confirm_open(const char *message, confirm_fn on_yes)
{
   confirm_open_ex(message, "profile.delete_note", "confirm.delete", on_yes);
}

static void confirm_input(const ui_nav_t *nav)
{
   if (nav->left) s_csel = 0;
   if (nav->right) s_csel = 1;
   if (nav->back || nav->options) { s_modal = M_NONE; return; }
   if (nav->confirm)
   {
      s_modal = M_NONE;
      s_sfx = s_csel == 1 ? SFX_CONFIRM : SFX_BACK;
      if (s_csel == 1 && s_con_yes) s_con_yes();
   }
}

static void confirm_draw(float a)
{
   float ox = (W - DLG_W) * 0.5f, oy = (H - DLG_H) * 0.5f, bw = (DLG_W - 80) * 0.5f, by = oy + DLG_H - 28 - 52;
   float c[4];
   int b;
   ui_card(mx(ox), my(oy), DLG_W, DLG_H, a);
   col(c, C_TXT, a);
   m_label(&s_cmsg, ox + 32, oy + 28, c);
   col(c, C_DIM, a);
   m_label(&s_cnote, ox + 32, oy + 36 + s_cmsg.h, c);
   for (b = 0; b < 2; b++)
   {  /* two glass pills; the answer that does something is tinted, never filled */
      float bx = ox + 32 + b * (bw + 16);
      if (s_csel == b) ui_focus(mx(bx), my(by), bw, 52, 26, a);
      ui_rest(mx(bx), my(by), bw, 52, 26, a);
      col(c, b ? C_RED : C_TXT, a);
      m_label(&s_cbtn[b], bx + (bw - s_cbtn[b].w) * 0.5f, by + (52 - s_cbtn[b].h) * 0.5f, c);
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
   s_slide.x = SLIDE_OFF;
   s_page = P_LANGUAGE;
   if (theme_active_index() > 0)
   {
      theme_color(TC_TEXT, C_TXT); theme_color(TC_DIM, C_DIM);
      theme_color(TC_ACCENT, C_ACC); theme_color(TC_DANGER, C_RED);
   }
}

/* the on-screen keyboard alone, over the dashboard (game menu: New folder) */
void panel_osk(const char *title_key, const char *initial, const char *(*accept)(const char *))
{
   if (s_open) return;
   s_open = 1; s_osk_only = 1;
   music_sfx(SFX_OPEN);
   osk_open(title_key, initial, accept);
   logf_("panel: keyboard (%s)", title_key);
}

void panel_open(void)
{
   if (s_open) return;
   s_open = 1;
   music_sfx(SFX_OPEN);
   if (s_slide.x > SLIDE_OFF - 20.0f) { s_slide.x = SLIDE_OFF; s_slide.v = 0; }
   s_in_card = 0;
   page_enter(s_page >= 1 && s_page < P_COUNT ? s_page : P_LANGUAGE, 0);
   rows_snap();
   logf_("panel: open");
}

void panel_open_profile(void)
{
   if (s_open) return;
   panel_open();
   page_enter(P_PROFILE, 0);
   rows_snap();
   s_in_card = 1;
}

int panel_active(void) { return s_open || s_slide.x <= SLIDE_OFF - 20.0f; }

float panel_amount(void) { return clampf(s_fade.x, 0.0f, 1.0f); }

int panel_hint_set(void)
{
   if (!s_open) return HINTS_CAROUSEL;
   if (s_modal == M_OSK) return HINTS_OSK;
   if (s_modal == M_CONFIRM) return HINTS_CONFIRM;
   if (!s_in_card) return HINTS_PANEL_ROOT;
   {
      /* the D-pad only changes values on pages with choice rows */
      int i, choice = 0, sel = 0;
      for (i = 0; i < s_nrows; i++)
      {
         if (s_rows[i].type == R_CHOICE) choice = 1;
         if (row_selectable(i)) sel = 1;
      }
      return choice ? HINTS_PANEL_PAGE : (sel ? HINTS_PANEL_LIST : HINTS_PANEL_INFO);
   }
}

/* interface sounds (guidelines/06 "Sound"): what the input of this frame changed */
static void panel_sound(int open, int page, int sel, int card, int modal, int orow, int ocol, int csel)
{
   int id = -1;
   if (s_sfx >= 0) id = s_sfx;
   else if (open && !s_open) id = SFX_BACK;
   else if (s_modal != modal) id = s_modal != M_NONE ? SFX_OPEN : SFX_BACK;
   else if (card != s_in_card) id = s_in_card ? SFX_CONFIRM : SFX_BACK;
   else if (page != s_page || sel != s_sel || orow != s_orow) id = SFX_FOCUS;
   else if (ocol != s_ocol || csel != s_csel) id = s_ocol > ocol || s_csel > csel ? SFX_MOVE_NEXT : SFX_MOVE_PREV;
   s_sfx = -1;
   if (id >= 0) music_sfx(id);
}

void panel_update(const ui_nav_t *nav, double t, float dt)
{
   (void)t;
   if (s_open)
   {
      int open = s_open, page = s_page, sel = s_sel, card = s_in_card, modal = s_modal;
      int orow = s_orow, ocol = s_ocol, csel = s_csel;
      s_sfx = -1;
      if (s_modal == M_OSK) osk_input(nav);
      else if (s_modal == M_CONFIRM) confirm_input(nav);
      else page_input(nav);
      panel_sound(open, page, sel, card, modal, orow, ocol, csel);
   }
   else if (s_modal != M_NONE) s_modal = M_NONE;
   if (s_open && s_osk_only && s_modal == M_NONE) { s_open = 0; s_osk_only = 0; }

   spring_step(&s_slide, s_open && !s_osk_only ? 0.0f : SLIDE_OFF, 170, 18, dt);
   spring_step(&s_fade, s_open && !s_osk_only ? 1.0f : 0.0f, 90, 14, dt);
   if (!s_open && s_slide.x > SLIDE_OFF - 20.0f && s_fade.x < 0.003f && s_modal_drawn == M_NONE) return;   /* fully closed */

   page_build();
   if (s_sel >= s_nrows || (s_sel >= 0 && !row_selectable(s_sel))) s_sel = first_selectable();
   spring_step(&s_px, 0.0f, 170, 18, dt);
   spring_step(&s_pa, 1.0f, 120, 16, dt);
   {
      /* keep the selected row inside the visible rows area */
      float view = CARD_H - HEAD_H - 20.0f - (s_footer.tex ? s_footer.h + 16.0f : 0.0f);
      float y = s_sel >= 0 ? s_sel * ROW_H : 0.0f, target = s_scroll.x;
      if (s_sel >= 0)
      {
         if (y < target) target = y;
         if (y + ROW_H > target + view) target = y + ROW_H - view;
      }
      if (target < 0) target = 0;
      spring_step(&s_hl, y, 220, 22, dt);
      spring_step(&s_scroll, target, 220, 22, dt);
      spring_step(&s_rail, (s_page - 1) * RAIL_P, 260, 26, dt);
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

/* 18 px profile colour circle ending at x = xr, centred in the row at y; returns the new xr */
#define SW_D 18.0f
static float draw_swatch(int color, float xr, float y, float a)
{
   float f[4];
   profiles_color(color, f); f[3] = a;
   draw_round_rect(xr - SW_D, y + (ROW_H - SW_D) * 0.5f, SW_D, SW_D, SW_D * 0.5f, f, 0.0f);
   return xr - SW_D - 10;
}

void panel_draw(double t)
{
   /* the section rises 0..34 px into place while it fades in; a modal or the file picker
    * over it takes the stage: the section fades out under them */
   const float m = clampf(s_mod.x, 0.0f, 1.0f), fp = fmaxf(filepicker_amount(), fmaxf(transfer_amount(), audit_amount())), over = m > fp ? m : fp;
   float a = panel_amount() * (1.0f - over), rise = s_slide.x * 0.06f;
   float ca, cx, cy = CARD_Y + rise, top, bottom, c[4], f[4];
   char buf[200], num[24];
   int i;
   if (s_slide.x > SLIDE_OFF - 10.0f && panel_amount() < 0.003f && s_modal_drawn == M_NONE) return;
   gl_use_2d();
   if (a < 0.003f) goto modal;
   ca = a * clampf(s_pa.x, 0.0f, 1.0f);
   cx = CARD_X + s_px.x;

   /* title and the rail of sections */
   label_set(&s_title, g_font_title, tr("panel.title"));
   col(c, C_TXT, a);
   draw_label(&s_title, TITLE_X, TITLE_Y + rise, c);
   if (s_in_card) ui_rest(RAIL_X, RAIL_Y + rise + s_rail.x, RAIL_W, RAIL_H, RAIL_H * 0.5f, a);   /* you are here */
   else ui_focus(RAIL_X, RAIL_Y + rise + s_rail.x, RAIL_W, RAIL_H, RAIL_H * 0.5f, a);
   for (i = 1; i < P_COUNT; i++)
   {
      label_t *l = &s_rail_l[i];
      label_set(l, g_font_sub, tr(k_page_key[i]));
      col(c, C_TXT, a * (i == s_page ? 1.0f : 0.86f));
      draw_label(l, RAIL_X + 20, RAIL_Y + rise + (i - 1) * RAIL_P + (RAIL_H - l->h) * 0.5f, c);
   }

   /* the card: a header that gives the section a face, then its rows */
   ui_card(CARD_X, cy, CARD_W, CARD_H, a);
   {
      float hx = cx + PAD;
      const char *title = tr(k_page_key[s_page]), *kick = tr("panel.title"), *cap = "";
      if (s_page == P_PROFILE)
      {
         int played = stats_games_played();
         ui_avatar_draw(cx + PAD, cy + 26, 72, ca);
         hx += 72 + 22;
         kick = tr("section.profile"); title = profiles_display_name(profiles_active());
         snprintf(num, sizeof(num), "%d", played);
         cap = trf(buf, sizeof(buf), played == 1 ? "dash.profile_games_one" : "dash.profile_games", "n", num, NULL);
      }
      ui_kicker(&s_kick, kick, hx, cy + 28, ca);
      label_set(&s_ptitle, g_font_title, title);
      col(c, C_TXT, ca);
      draw_label(&s_ptitle, hx, cy + 44, c);
      label_set(&s_cap, g_font_small, cap);
      col(c, C_DIM, ca);
      draw_label(&s_cap, hx, cy + 88, c);
      col(f, (const float[3]){ 1, 1, 1 }, 0.08f * a);
      draw_2d(0, CARD_X + PAD, cy + 124, CARD_W - 2 * PAD, 1, f, 0);
   }

   /* footer note, inside the card */
   if (s_foot[0]) wlabel_set(&s_footer, g_font_small, s_foot, (int)(CARD_W - 2 * PAD - 12));
   else label_free(&s_footer);
   bottom = cy + CARD_H - 20.0f - (s_footer.tex ? s_footer.h + 16.0f : 0.0f);
   if (s_footer.tex)
   {
      col(c, C_DIM, ca);
      draw_label(&s_footer, cx + PAD + 6, cy + CARD_H - 20.0f - s_footer.h, c);
   }

   /* rows, clipped to their area (the ring needs 8 px around the pill) */
   top = cy + HEAD_H;
   glEnable(GL_SCISSOR_TEST);
   gl_scissor(CARD_X + 2, top - 8, CARD_W - 4, bottom - top + 16);
   if (s_sel >= 0 && s_in_card) ui_focus(cx + 14, top + s_hl.x - s_scroll.x + 2, CARD_W - 28, ROW_H - 4, (ROW_H - 4) * 0.5f, ca);
   for (i = 0; i < s_nrows; i++)
   {
      const row_t *r = &s_rows[i];
      float y = top + i * ROW_H - s_scroll.x, xr = cx + CARD_W - 14 - 20, la = ca * (r->dim ? 0.38f : 1.0f);
      label_set(&s_rl[i], g_font_sub, r->label);
      label_set(&s_rv[i], g_font_sub, r->value);
      if (y + ROW_H < top - 8 || y > bottom + 8) continue;
      col(c, C_TXT, la);
      draw_label(&s_rl[i], cx + 14 + 20, y + (ROW_H - s_rl[i].h) * 0.5f, c);
      if (r->type == R_CHOICE)
      {
         col(c, C_DIM, la * (r->at_hi ? 0.35f : 1.0f));
         draw_label(&s_gt, xr - s_gt.w, y + (ROW_H - s_gt.h) * 0.5f, c);
         xr -= s_gt.w + 12;
         if (r->swatches)
         {
            int k;
            for (k = PROFILE_COLORS - 1; k >= 0; k--)
            {
               float sx = xr - SW_D, sy = y + (ROW_H - SW_D) * 0.5f;
               if (k == r->swatch)
               {
                  col(f, (const float[3]){ 1, 1, 1 }, 0.90f * la);
                  draw_round_rect(sx - 3, sy - 3, SW_D + 6, SW_D + 6, SW_D * 0.5f + 3, f, 0.0f);
               }
               profiles_color(k, f); f[3] = la * (k == r->swatch ? 1.0f : 0.70f);
               draw_round_rect(sx, sy, SW_D, SW_D, SW_D * 0.5f, f, 0.0f);
               xr -= SW_D + 10;
            }
            xr -= 2;
         }
         else
         {
            col(c, C_TXT, la * 0.92f);
            draw_label(&s_rv[i], xr - s_rv[i].w, y + (ROW_H - s_rv[i].h) * 0.5f, c);
            xr -= s_rv[i].w + 12;
            if (r->swatch >= 0) xr = draw_swatch(r->swatch, xr, y, la);
         }
         col(c, C_DIM, la * (r->at_lo ? 0.35f : 1.0f));
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
         col(c, C_TXT, la * 0.92f);
         if (r->type == R_ACTION && !r->enter) col(c, C_DIM, la);
         draw_label(&s_rv[i], xr - s_rv[i].w, y + (ROW_H - s_rv[i].h) * 0.5f, c);
         if (r->swatch >= 0) draw_swatch(r->swatch, xr - s_rv[i].w - 12, y, la);
         if (r->avatar) ui_avatar_draw(xr - s_rv[i].w - 12 - 30, y + (ROW_H - 30) * 0.5f, 30, la);
      }
   }
   glDisable(GL_SCISSOR_TEST);

   /* modal: the keyboard or a question, alone on the scrim */
modal:
   if (s_modal_drawn != M_NONE)
   {
      if (panel_amount() < 0.5f) ui_scrim(m * (1.0f - panel_amount()));   /* keyboard alone over the home */
      s_mrise = (1.0f - m) * 16.0f;
      if (s_modal_drawn == M_OSK) osk_draw(t, m);
      else confirm_draw(m);
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
