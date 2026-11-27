/* Settings panel, on-screen keyboard, confirm dialog and toast (docs/settings-design.md 6;
 * look and layout: lumen-design, settings.html / keyboard.html / confirm.html).
 *
 * Everything lives in static storage: the rows of the current page are rebuilt every
 * frame into fixed buffers (plain string copies, no heap), and every text on screen is a
 * label_t that re-renders only when its string changes. The keyboard renders its key
 * glyphs once when it opens and frees them when it has faded out. */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <time.h>
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
#include "net.h"
#include "module.h"
#include "conn.h"
#include "kern.h"
#include "update.h"
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
#define RAIL_H  46.0f        /* the sections of the rail (up to eight), from y 172 */
#define RAIL_P  50.0f        /* rail pitch */
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
/* Pages. The rail holds the categories (1 .. P_RAIL_END - 1); a category's card lists its
 * settings and the sub-pages it opens (a row with ›), which open in the same card with the
 * category as their kicker; Circle goes back up one level, to the row it came from. */
enum { P_ROOT, P_PROFILE, P_DISPLAY, P_SOUND, P_NETWORK, P_BLUETOOTH, P_LIBRARY, P_MODULES, P_SYSTEM,
       P_GRAPHICS, P_SCREENSAVER, P_THEME, P_AUDIO_OUT, P_LANGUAGE, P_UPDATE, P_DATETIME, P_ABOUT, P_MODULE, P_COUNT };
#define P_RAIL_END P_GRAPHICS
static const char *const k_page_key[P_COUNT] = {
   "panel.title", "section.profile", "section.display", "section.sound", "section.network", "section.bluetooth",
   "section.library", "section.modules", "section.system",
   "section.graphics", "section.screensaver", "section.theme", "section.audio_out", "section.language",
   "section.update", "section.datetime", "section.about", "section.modules"
};
static const int k_parent[P_COUNT] = {
   [P_GRAPHICS] = P_DISPLAY, [P_SCREENSAVER] = P_DISPLAY, [P_THEME] = P_DISPLAY, [P_AUDIO_OUT] = P_SOUND,
   [P_LANGUAGE] = P_SYSTEM, [P_UPDATE] = P_SYSTEM, [P_DATETIME] = P_SYSTEM, [P_ABOUT] = P_SYSTEM,
   [P_MODULE] = P_MODULES
};
static int rail_of(int page) { while (page > 0 && page < P_COUNT && k_parent[page]) page = k_parent[page]; return page; }

/* modules (docs/modules.md): a category of the rail shows only while a module behind it is
 * switched on; Network for wifi / ethernet, Bluetooth for bluetooth */
static int mod_on(const char *id) { return module_switch(module_index(id)); }
static int rail_shown(int page)
{
   if (page == P_NETWORK) return mod_on("wifi") || mod_on("ethernet");
   if (page == P_BLUETOOTH) return mod_on("bluetooth");
   return page >= 1 && page < P_RAIL_END;
}
/* place of a category in the rail, counting only the ones shown */
static int rail_pos(int page)
{
   int i, n = 0;
   for (i = 1; i < page && i < P_RAIL_END; i++) n += rail_shown(i);
   return n;
}
/* the next category shown from `page` in direction dir (+1 / -1), or page itself */
static int rail_step(int page, int dir)
{
   int p;
   for (p = page + dir; p >= 1 && p < P_RAIL_END; p += dir) if (rail_shown(p)) return p;
   return page;
}
static int s_mod_sel;   /* the module of the P_MODULE page (module.h index) */

enum { R_ACTION, R_CHOICE, R_INFO };
enum { ID_LANG = 100, ID_PROFILE_ACTIVE = 200, ID_PROFILE_CREATE, ID_PROFILE_RENAME, ID_PROFILE_COLOR, ID_PROFILE_DELETE,
       ID_PROFILE_AVATAR, ID_PROFILE_AVATAR_REMOVE,
       ID_SS_AFTER = 300, ID_SS_DEMOS, ID_SS_LENGTH, ID_SS_PREVIEW, ID_GFX = 400,
       ID_SND_MUSIC = 600, ID_SND_VOLUME, ID_SND_UI, ID_LIB_STOCK = 500, ID_LIB_TRANSFER, ID_LIB_AUDIT,
       ID_DSP_OUTPUT = 700, ID_DSP_COMP, ID_DSP_SCANOUT, ID_DSP_RESTART, ID_DSP_THEME, ID_DSP_THEME_APPLY, ID_DSP_CRT, ID_INFO = 900,
       ID_NET_WIFI = 1000, ID_NET_WIFI_LIST, ID_NET_BT, ID_NET_BT_LIST, ID_NET_ETH, ID_NET_TIME, ID_NET_BT_ADD, ID_NET_IP,
       ID_KERNEL = 1100, ID_UPD_CHECK = 1200, ID_UPD_GET, ID_UPD_CANCEL, ID_UPD_INSTALL, ID_UPD_AUTO,
       ID_SYS_REBOOT = 1300, ID_AUDIO_OUT = 1400, ID_AUDIO_BT = 1500,
       ID_MOD_SWITCH = 1600, ID_MOD_STATE, ID_MOD_VERSION, ID_MOD_SOURCE, ID_MOD = 1700 };

/* ---- Display page: output resolution (and direct scanout), applied by boot.sh at the
 * next boot (settings display / scanout). The compositor is not a choice: boot.sh runs
 * the newest PSC compositor installed in <home>/compositor/ (RetroArch 1.22 needs it)
 * and falls back to Sony's only if it does not start; "In use" shows which one runs. */
static const char *const k_comp_id[] = { "stock", "weston16" };
static const char *const k_comp_key[] = { "display.comp_stock", "display.comp_w16" };
static int comp_available(int i)
{
   static int has = -1;   /* asked once: the settings ask every frame, and the stick can be busy */
   char p[400];
   if (i == 0) return 1;
   if (has < 0)
   {
      snprintf(p, sizeof(p), "%s/compositor/psc-weston-16.tar.gz", g_home);
      has = access(p, R_OK) == 0;
   }
   return has;
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
static void update_yes(void) { ui_request_update(); }

/* ---- Sound > Audio output: the ALSA cards (/proc/asound/cards, read when the page opens:
 * a USB headset or DAC can come and go). run.sh hands the choice to RetroArch (audio_device)
 * and to Lumen at its start (AUDIODEV); while Lumen runs, panel_audio_route() moves the
 * sound itself, with no restart. "auto" = ALSA's default (HDMI). */
#define AO_MAX 8
static struct { char id[20], name[64]; } s_ao[AO_MAX];
static int s_nao;
static void audio_scan(void)
{
   FILE *f = fopen("/proc/asound/cards", "r");
   char line[200];
   s_nao = 0;
   if (!f) return;
   while (fgets(line, sizeof(line), f) && s_nao < AO_MAX)
   {  /* " 0 [HDMI           ]: driver - Long name" */
      char *lb = strchr(line, '['), *rb = lb ? strchr(lb, ']') : NULL, *nm = rb ? strstr(rb, " - ") : NULL;
      size_t l;
      if (!lb || !rb || !nm || line[0] != ' ' || line[1] < '0' || line[1] > '9') continue;
      l = (size_t)(rb - lb - 1);
      while (l && lb[l] == ' ') l--;
      if (!l || l >= sizeof(s_ao[0].id)) continue;
      memcpy(s_ao[s_nao].id, lb + 1, l); s_ao[s_nao].id[l] = 0;
      nm += 3; nm[strcspn(nm, "\r\n")] = 0;
      snprintf(s_ao[s_nao].name, sizeof(s_ao[0].name), "%s", nm);
      s_nao++;
   }
   fclose(f);
}
static const char *audio_out(void) { return settings_get("audio_out", "auto"); }
/* Bluetooth audio: the paired headsets and speakers, when the Bluetooth package has
 * bluez-alsa (net.sh starts bluealsad); audio_out = "bt_<address without colons>" */
static int bt_audio_available(void) { return access("/tmp/lumen-bt/bin/bluealsad", X_OK) == 0; }
static int dev_is_audio(const net_dev_t *d) { return !strncmp(d->icon, "audio-", 6) || !strcmp(d->icon, "multimedia-player"); }
static void bt_out_id(char *out, size_t n, const char *mac)
{
   size_t o = 0;
   o += (size_t)snprintf(out, n, "bt_");
   for (; *mac && o + 1 < n; mac++) if (*mac != ':') out[o++] = *mac;
   out[o] = 0;
}
/* ---- the audio route: where the sound goes now, following the setting and what is there.
 * A Bluetooth headset chosen as the output is used from the moment it is connected (at
 * power-on Bluetooth comes up after Lumen) and the sound goes back to the default when it
 * leaves; a USB card likewise. Nothing restarts: music_reopen() moves the device. */
#define BT_ALSA_CONF "/tmp/lumen-bt/asound-bluealsa.conf"
static char s_route[72];          /* the ALSA device in use ("" = the default, where Lumen starts) */

/* the ALSA device the setting asks for and that can be opened now ("" = the default) */
static void audio_target(char *out, size_t n)
{
   const char *w = audio_out();
   const net_state_t *ns = net_state();
   char id[24];
   int i;
   out[0] = 0;
   if (!strcmp(w, "auto") || !*w) return;
   if (!strncmp(w, "bt_", 3))
   {
      if (ns->bt != NET_CONNECTED) return;
      for (i = 0; i < ns->ndev; i++)
      {
         bt_out_id(id, sizeof(id), ns->dev[i].mac);
         if (!strcmp(id, w) && ns->dev[i].connected) { snprintf(out, n, "bluealsa:DEV=%s,PROFILE=a2dp", ns->dev[i].mac); return; }
      }
      return;
   }
   audio_scan();   /* /proc/asound/cards: a few lines of RAM */
   for (i = 0; i < s_nao; i++) if (!strcmp(s_ao[i].id, w)) { snprintf(out, n, "plughw:CARD=%s,DEV=0", w); return; }
}

/* the chosen output is not the one in use (it is not connected or plugged in) */
static int audio_out_pending(void)
{
   char t[72];
   if (!strcmp(audio_out(), "auto")) return 0;
   audio_target(t, sizeof(t));
   return !t[0];
}

/* Follows the setting and what is connected, twice a second. A named output is opened in
 * the background by music.c while the TV keeps playing, so an output that never answers
 * costs a toast, not a frozen dashboard. */
void panel_audio_route(double t)
{
   static double next, since, retry_at;
   static char want[72], failed[72], pending[72];
   char now[72];
   int bt, st;
   if (t < next) return;
   next = t + 0.5;
   st = music_output_state();
   if (pending[0])
   {  /* the answer of the output asked for */
      if (st == 1) return;
      bt = !strncmp(pending, "bluealsa:", 9);
      if (st == 2)
      {
         snprintf(s_route, sizeof(s_route), "%s", pending);
         failed[0] = 0;
         panel_toast(bt ? "audio.toast_bt" : "audio.toast_card");
      }
      else
      {  /* say so once, try again later */
         music_reopen(NULL, NULL);
         if (strcmp(pending, failed)) panel_toast("audio.toast_failed");
         snprintf(failed, sizeof(failed), "%s", pending);
         retry_at = t + 20.0;
      }
      logf_("panel: audio route %s -> %s", pending, st == 2 ? "ok" : "failed");
      pending[0] = 0;
      ui_sound_changed();
      return;
   }
   if (s_route[0] && st != 2)
   {  /* the output in use went away under us (the headset was switched off) */
      logf_("panel: audio: %s stopped, back to the default", s_route);
      music_reopen(NULL, NULL);
      snprintf(failed, sizeof(failed), "%s", s_route);
      retry_at = t + 5.0;
      s_route[0] = 0;
      ui_sound_changed();
   }
   audio_target(now, sizeof(now));
   if (strcmp(now, want)) { snprintf(want, sizeof(want), "%s", now); since = t; }
   if (!strcmp(want, s_route)) return;
   /* a headset that has just connected needs a moment before its audio link is up; an
    * output that failed is tried again after 20 s */
   if (t - since < (want[0] ? 2.0 : 0.3)) return;
   if (!strcmp(want, failed) && t < retry_at) return;
   bt = !strncmp(want, "bluealsa:", 9);
   if (bt && access(BT_ALSA_CONF, R_OK) != 0) return;
   if (music_reopen(want, bt ? "/usr/share/alsa/alsa.conf:" BT_ALSA_CONF : NULL) < 0) { retry_at = t + 20.0; snprintf(failed, sizeof(failed), "%s", want); return; }
   if (want[0]) { snprintf(pending, sizeof(pending), "%s", want); return; }
   s_route[0] = 0;
   panel_toast("audio.toast_default");
   ui_sound_changed();
   logf_("panel: audio route default -> ok");
}
static const char *audio_out_name(void)
{
   const char *w = audio_out();
   const net_state_t *ns = net_state();
   char id[24];
   int i;
   if (!strcmp(w, "auto")) return tr("audio.auto");
   for (i = 0; i < s_nao; i++) if (!strcmp(s_ao[i].id, w)) return s_ao[i].name;
   for (i = 0; i < ns->ndev; i++) { bt_out_id(id, sizeof(id), ns->dev[i].mac); if (!strcmp(id, w)) return ns->dev[i].name; }
   return !strncmp(w, "bt_", 3) ? "Bluetooth" : w;
}

/* a connection to the internet: Wi-Fi joined or the cable up */
static int net_online(void) { const net_state_t *ns = net_state(); return ns->wifi == NET_CONNECTED || ns->eth == NET_CONNECTED; }

/* ---- System > Software update: the state in words (update.h) */
static const char *update_status(char *buf, size_t n)
{
   const update_info_t *u = update_info();
   char num[16];
   switch (u->state)
   {
   case UPD_CHECKING:  return tr("update.checking");
   case UPD_CURRENT:   return tr("update.current");
   case UPD_AVAILABLE: return trf(buf, n, "update.available", "v", u->version, NULL);
   case UPD_PREPARING:
      snprintf(num, sizeof(num), "%d", (int)(u->progress * 100.0f + 0.5f));
      return trf(buf, n, "update.progress", "phase", tr(u->phase ? u->phase : "update.phase_compare"), "n", num, NULL);
   case UPD_READY:     return trf(buf, n, "update.ready", "v", u->version, NULL);
   case UPD_ERROR:     return tr(!net_online() ? "update.offline" : u->error ? u->error : "update.err_offline");
   default:            return tr(net_online() ? "update.not_checked" : "update.offline");
   }
}
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
   const char *desc;            /* what the row does: tr key shown in the help line, NULL none */
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

static label_t s_title, s_ptitle, s_lt, s_gt, s_kick, s_cap, s_rail_l[16];
static label_t s_rl[MAX_ROWS], s_rv[MAX_ROWS], s_rvf[MAX_ROWS];   /* label, value, value cut to fit */
/* The help line shows one text at a time: when the focus moves, the text on screen fades
 * out, and the new one fades in only once the focus has rested (HELP_REST_S) and the old
 * one is gone. Two texts are never drawn over each other, and moving fast through the rows
 * does not flash every description on the way. */
#define HELP_REST_S 0.14
static label_t s_help;             /* the text on screen */
static char    s_help_want[400];   /* the text of the focus */
static double  s_help_want_t;      /* since when */
static float   s_help_target = 1.0f;
static spring  s_help_a;           /* 0..1: its opacity */
static double  s_sel_t;            /* when the focused row last changed (the marquee starts over) */
static int     s_sel_page = -1, s_sel_row = -1, s_sel_card = -1;

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
#define OSK_TEXT_MAX  200         /* buffer; a Wi-Fi password is up to 63 characters */
static int   s_osk_chars = OSK_MAX_CHARS, s_osk_bytes = OSK_MAX_BYTES;
static int   s_osk_raw;           /* password: spaces kept, empty allowed to reach accept() */
static int   s_sym;               /* the symbols layer (Shift cycles A -> a -> #+=) */
static const char *const k_sym[40] = {
   "1", "2", "3", "4", "5", "6", "7", "8", "9", "0",
   "!", "\"", "#", "$", "%", "&", "'", "(", ")", "*",
   "+", ",", "/", ":", ";", "<", "=", ">", "?", "@",
   "[", "\\", "]", "^", "`", "{", "|", "}", "~", "-",
};
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
static label_t     s_kup[40], s_klo[40], s_ksym[40], s_kact[4], s_ktitle, s_ktext;
static char        s_ktitle_txt[96];
static char        s_text[OSK_TEXT_MAX + 4];
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

static float help_room(void);
static void osk_open(const char *title_key, const char *initial, osk_accept_fn accept);
static void confirm_open(const char *message, confirm_fn on_yes);
static void confirm_open_ex(const char *message, const char *note_key, const char *yes_key, confirm_fn on_yes);

/* toast */
static label_t     s_toast;
static const char *s_toast_key;
static double      s_toast_t = -100.0;

/* ------------------------------------------------------------------------ */
/* rows of the current page                                                  */
/* the help line of each row (lumen-design: a console's settings explain the focused option
 * at the foot of the card); sub-page rows explain the page they open */
static const char *row_desc(int id)
{
   static const struct { int id; const char *key; } k[] = {
      { ID_PROFILE_ACTIVE, "desc.profile_active" }, { ID_PROFILE_CREATE, "desc.profile_create" },
      { ID_PROFILE_RENAME, "desc.profile_rename" }, { ID_PROFILE_COLOR, "desc.profile_color" },
      { ID_PROFILE_AVATAR, "desc.profile_avatar" }, { ID_PROFILE_AVATAR_REMOVE, "desc.profile_avatar_remove" },
      { ID_PROFILE_DELETE, "desc.profile_delete" },
      { ID_DSP_OUTPUT, "desc.display_output" }, { ID_DSP_SCANOUT, "desc.display_scanout" },
      { ID_DSP_RESTART, "desc.display_restart" }, { ID_DSP_CRT, "desc.display_crt" },
      { ID_DSP_THEME, "desc.display_theme" }, { ID_DSP_THEME_APPLY, "desc.display_theme_apply" },
      { ID_SS_AFTER, "desc.ss_after" }, { ID_SS_DEMOS, "desc.ss_demos" }, { ID_SS_LENGTH, "desc.ss_length" },
      { ID_SS_PREVIEW, "desc.ss_preview" },
      { ID_SND_MUSIC, "desc.sound_music" }, { ID_SND_VOLUME, "desc.sound_volume" }, { ID_SND_UI, "desc.sound_ui" },
      { ID_AUDIO_OUT, "desc.audio_auto" },
      { ID_NET_WIFI, "desc.net_wifi" }, { ID_NET_WIFI_LIST, "desc.net_network" }, { ID_NET_BT, "desc.net_bt" },
      { ID_NET_BT_LIST, "desc.net_devices" }, { ID_NET_BT_ADD, "desc.net_bt_add" }, { ID_NET_TIME, "desc.net_time" },
      { ID_LIB_STOCK, "desc.lib_stock" }, { ID_LIB_AUDIT, "desc.lib_audit" }, { ID_LIB_TRANSFER, "desc.lib_transfer" },
      { ID_KERNEL, "desc.kernel" }, { ID_SYS_REBOOT, "desc.system_reboot" },
      { ID_UPD_CHECK, "desc.update_check" }, { ID_UPD_GET, "desc.update_get" }, { ID_UPD_CANCEL, "desc.update_cancel" },
      { ID_UPD_INSTALL, "desc.update_install" }, { ID_UPD_AUTO, "desc.update_auto" },
      { P_THEME, "desc.page_theme" }, { P_GRAPHICS, "desc.page_graphics" }, { P_SCREENSAVER, "desc.page_screensaver" },
      { P_AUDIO_OUT, "desc.page_audio_out" }, { P_LANGUAGE, "desc.page_language" }, { P_UPDATE, "desc.page_update" },
      { P_DATETIME, "desc.page_datetime" }, { P_ABOUT, "desc.page_about" },
   };
   size_t i;
   if (id >= ID_LANG && id < ID_LANG + I18N_MAX_LANGS) return "desc.language";
   if (id >= ID_GFX && id < ID_GFX + MAX_PLATFORMS) return "desc.gfx";
   if (id > ID_AUDIO_OUT && id <= ID_AUDIO_OUT + AO_MAX) return "desc.audio_card";
   if (id >= ID_AUDIO_BT && id < ID_AUDIO_BT + NET_MAX_DEVS) return "desc.audio_bt";
   for (i = 0; i < sizeof(k) / sizeof(k[0]); i++) if (k[i].id == id) return k[i].key;
   return NULL;
}
/* what each category of the rail holds, while the focus is on the rail */
static const char *const k_rail_desc[P_RAIL_END] = {
   NULL, "desc.rail_profile", "desc.rail_display", "desc.rail_sound", "desc.rail_network", "desc.rail_bluetooth",
   "desc.rail_library", "desc.rail_modules", "desc.rail_system"
};

static row_t *row_add(int type, int id, const char *label, const char *value)
{
   row_t *r;
   if (s_nrows >= MAX_ROWS) return NULL;
   r = &s_rows[s_nrows++];
   memset(r, 0, sizeof(*r));
   r->type = type; r->id = id; r->enabled = 1; r->swatch = -1; r->desc = row_desc(id);
   snprintf(r->label, sizeof(r->label), "%s", label);
   snprintf(r->value, sizeof(r->value), "%s", value ? value : "");
   return r;
}

/* a module's state in words (Settings > Modules): On, Off, Needs <module>, After restart,
 * or why it cannot run */
static const char *mod_state_text(int i, char *buf, size_t n)
{
   const char *b;
   switch (module_state(i))
   {
   case MOD_ON:      return tr("mods.state_on");
   case MOD_OFF:     return tr("mods.state_off");
   case MOD_RESTART: return tr("mods.state_restart");
   case MOD_BLOCKED:
      if (!(b = module_blocker(i))) return tr("mods.missing_dep");
      return trf(buf, n, "mods.state_needs", "name", tr(module_name_key(module_index(b))), NULL);
   default:          return tr(module_why(i) ? module_why(i) : "mods.unavailable");
   }
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
   char v[160], num[32];
   row_t *r;
   int i;
   s_nrows = 0; s_foot[0] = 0;
   switch (s_page)
   {
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
      {  /* CRT filter: the dashboard changes at once, games from their next launch */
         static const char *const k_crt_key[CRT_LEVELS] = { "value.off", "display.crt_soft", "display.crt_strong" };
         int cl = crt_level();
         if ((r = row_add(R_CHOICE, ID_DSP_CRT, tr("display.crt"), tr(k_crt_key[cl]))))
         { r->at_lo = cl == CRT_OFF; r->at_hi = cl == CRT_LEVELS - 1; }
      }
      if ((r = row_add(R_ACTION, P_THEME, tr("section.theme"), theme_name(theme_setting_index())))) r->enter = 1;
      if ((r = row_add(R_ACTION, P_GRAPHICS, tr("section.graphics"), NULL))) r->enter = 1;
      minutes_text(v, sizeof(v), settings_get_int("screensaver_minutes", 5, 0, 60));
      if ((r = row_add(R_ACTION, P_SCREENSAVER, tr("section.screensaver"), v))) r->enter = 1;
      snprintf(s_foot, sizeof(s_foot), "%s", tr("display.note"));
      break;
   }
   case P_THEME:
   {  /* chosen here, applied by restarting Lumen (fonts and art load at start) */
      int ti = theme_setting_index();
      if ((r = row_add(R_CHOICE, ID_DSP_THEME, tr("display.theme"), theme_name(ti))))
      { r->at_lo = ti <= 0; r->at_hi = ti >= theme_count() - 1; if (theme_count() < 2) { r->enabled = 0; r->dim = 1; r->why = "display.no_themes"; } }
      if ((r = row_add(R_ACTION, ID_DSP_THEME_APPLY, tr("display.theme_apply"), NULL)) && ti == theme_active_index())
      { r->enabled = 0; r->dim = 1; r->why = "display.theme_in_use"; }
      snprintf(s_foot, sizeof(s_foot), "%s", tr("display.theme_note"));
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
   case P_SOUND:
   {
      int on = music_on(), vol = music_volume();
      if ((r = row_add(R_ACTION, P_AUDIO_OUT, tr("section.audio_out"), audio_out_name()))) r->enter = 1;
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
   case P_AUDIO_OUT:
   {
      const char *w = audio_out();
      if ((r = row_add(R_ACTION, ID_AUDIO_OUT, tr("audio.auto"), tr("audio.auto_value")))) r->check = !strcmp(w, "auto");
      for (i = 0; i < s_nao; i++)
         if ((r = row_add(R_ACTION, ID_AUDIO_OUT + 1 + i, s_ao[i].name, s_ao[i].id))) r->check = !strcmp(w, s_ao[i].id);
      if (bt_audio_available())
      {
         const net_state_t *ns = net_state();
         char id[24];
         for (i = 0; i < ns->ndev; i++)
         {
            const net_dev_t *d = &ns->dev[i];
            if (!d->paired || !dev_is_audio(d)) continue;
            bt_out_id(id, sizeof(id), d->mac);
            if ((r = row_add(R_ACTION, ID_AUDIO_BT + i, d->name, tr(d->connected ? "audio.bt_connected" : "audio.bt_paired"))))
               r->check = !strcmp(w, id);
         }
      }
      snprintf(s_foot, sizeof(s_foot), "%s", tr(audio_out_pending() ? "audio.note_pending" : "audio.note"));
      break;
   }
   case P_NETWORK:
   {  /* lumen-design, settings-connections.html: the Wi-Fi switch and its network, the cable */
      const net_state_t *ns = net_state();
      const int m_wifi = mod_on("wifi"), m_eth = mod_on("ethernet");
      if (m_wifi && (r = row_add(R_CHOICE, ID_NET_WIFI, tr("net.wifi"), tr(ns->wifi_on ? "value.on" : "value.off"))))
      {
         r->at_lo = !ns->wifi_on; r->at_hi = ns->wifi_on;
         if (ns->wifi_cap == NET_CAP_NO && !ns->wifi_on) { r->enabled = 0; r->dim = 1; r->why = "net.wifi_nokernel"; }
      }
      /* the networks only once Wi-Fi is on and a USB adapter answers (a console's settings do
       * not offer what cannot work); without one, the row says what is missing */
      if (m_wifi && ns->wifi_on && ns->wifi == NET_NOADAPTER)
         row_add(R_INFO, ID_INFO, tr("net.network"), tr("net.noadapter"));
      else if (m_wifi && ns->wifi_on && (r = row_add(R_ACTION, ID_NET_WIFI_LIST, tr("net.network"),
                       ns->wifi == NET_CONNECTED ? ns->ssid : ns->wifi == NET_CONNECTING ? tr("net.connecting") : tr("net.notconnected"))))
      { r->enter = 1; r->check = ns->wifi == NET_CONNECTED; }
      if (m_eth) row_add(R_INFO, ID_NET_ETH, tr("net.cable"),
              ns->eth == NET_CONNECTED ? ns->eth_ip : ns->eth == NET_CONNECTING ? tr("net.connecting") :
              ns->eth == NET_NOCABLE ? tr("net.nocable") : tr("net.noadapter"));
      if (m_wifi && ns->wifi == NET_CONNECTED && ns->ip[0]) row_add(R_INFO, ID_NET_IP, tr("net.ip"), ns->ip);
      snprintf(s_foot, sizeof(s_foot), "%s", tr("net.note"));
      break;
   }
   case P_BLUETOOTH:
   {  /* the switch, the devices paired with the console, and the search for new ones */
      const net_state_t *ns = net_state();
      char cnt[16], cnt2[16];
      if ((r = row_add(R_CHOICE, ID_NET_BT, tr("net.bt"), tr(ns->bt_on ? "value.on" : "value.off"))))
      {
         r->at_lo = !ns->bt_on; r->at_hi = ns->bt_on;
         if (ns->bt_cap == NET_CAP_NO && !ns->bt_on) { r->enabled = 0; r->dim = 1; r->why = "net.bt_nokernel"; }
      }
      snprintf(cnt, sizeof(cnt), "%d", ns->paired); snprintf(cnt2, sizeof(cnt2), "%d", ns->connected);
      /* devices and the search only with Bluetooth on and an adapter that answered */
      if (ns->bt_on && ns->bt == NET_NOADAPTER)
         row_add(R_INFO, ID_INFO, tr("net.adapter"), tr("net.noadapter"));
      else if (ns->bt_on && ns->bt == NET_CONNECTED)
      {
         if ((r = row_add(R_ACTION, ID_NET_BT_LIST, tr("net.devices"), trf(v, sizeof(v), "net.paired_n", "n", cnt, "c", cnt2, NULL))))
            r->enter = 1;
         if ((r = row_add(R_ACTION, ID_NET_BT_ADD, tr("net.bt_add"), NULL))) r->enter = 1;
      }
      else if (ns->bt_on) row_add(R_INFO, ID_INFO, tr("net.adapter"), tr("net.connecting"));
      snprintf(s_foot, sizeof(s_foot), "%s", tr("net.bt_page_note"));
      break;
   }
   case P_MODULES:
   {  /* every module, its state as the value; the help line shows its description */
      for (i = 0; i < modules_count() && i < MAX_ROWS - 2; i++)
         if ((r = row_add(R_ACTION, ID_MOD + i, tr(module_name_key(i)), mod_state_text(i, v, sizeof(v)))))
         { r->enter = 1; r->desc = module_desc_key(i); r->check = module_state(i) == MOD_ON; }
      if (modules_restart_pending()) row_add(R_ACTION, ID_SYS_REBOOT, tr("mods.restart"), NULL);
      snprintf(s_foot, sizeof(s_foot), "%s", tr("mods.note"));
      break;
   }
   case P_MODULE:
   {
      const lumen_module_t *m = module_at(s_mod_sel);
      int on = module_switch(s_mod_sel);
      if (!m) break;
      if ((r = row_add(R_CHOICE, ID_MOD_SWITCH, tr("mods.switch"), tr(on ? "value.on" : "value.off"))))
      {
         r->at_lo = !on; r->at_hi = on; r->desc = module_desc_key(s_mod_sel);
         if (m->flags & MOD_F_REQUIRED) { r->enabled = 0; r->dim = 1; r->why = "mods.required"; }
      }
      {  /* the state is under the title; this row only says why it is not plainly on or off */
         const int st = module_state(s_mod_sel);
         if (st != MOD_ON && st != MOD_OFF) row_add(R_INFO, ID_MOD_STATE, tr("mods.state"), mod_state_text(s_mod_sel, v, sizeof(v)));
      }
      row_add(R_INFO, ID_MOD_VERSION, tr("mods.version"), m->version ? m->version : "-");
      row_add(R_INFO, ID_MOD_SOURCE, tr("mods.source"), tr("mods.builtin"));
      if (modules_restart_pending()) row_add(R_ACTION, ID_SYS_REBOOT, tr("mods.restart"), NULL);
      snprintf(s_foot, sizeof(s_foot), "%s", tr(module_desc_key(s_mod_sel)));
      break;
   }
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
   case P_SYSTEM:
      i = lang_active_index();
      if ((r = row_add(R_ACTION, P_LANGUAGE, tr("section.language"), i >= 0 ? i18n_name(i) : i18n_language()))) r->enter = 1;
      if ((r = row_add(R_ACTION, P_UPDATE, tr("section.update"), update_status(v, sizeof(v))))) r->enter = 1;
      {
         const net_state_t *ns = net_state();
         if ((r = row_add(R_ACTION, P_DATETIME, tr("section.datetime"), ns->time_ok ? ns->time_text : tr("time.not_set")))) r->enter = 1;
      }
      if (*kern_name_key() && (r = row_add(R_ACTION, ID_KERNEL, tr("about.kernel"), tr(kern_name_key()))))
         r->enter = 1;   /* not Sony's stock kernel, or a backup to go back to: kern.c */
      if ((r = row_add(R_ACTION, P_ABOUT, tr("section.about"), LUMEN_VERSION))) r->enter = 1;
      row_add(R_ACTION, ID_SYS_REBOOT, tr("system.reboot"), NULL);
      break;
   case P_LANGUAGE:
      for (i = 0; i < i18n_count(); i++)
         if ((r = row_add(R_ACTION, ID_LANG + i, i18n_name(i), NULL)))
            r->check = !strcmp(i18n_code(i), i18n_language());
      break;
   case P_UPDATE:
   {
      const update_info_t *u = update_info();
      const int busy = u->state == UPD_CHECKING || u->state == UPD_PREPARING, online = net_online();
      row_add(R_INFO, ID_INFO, tr("update.installed"), u->installed[0] ? u->installed : LUMEN_VERSION);
      row_add(R_INFO, ID_INFO, tr("update.status"), update_status(v, sizeof(v)));
      if (u->state == UPD_AVAILABLE && (r = row_add(R_ACTION, ID_UPD_GET, trf(v, sizeof(v), "update.get", "v", u->version, NULL), NULL)) && !online)
      { r->enabled = 0; r->dim = 1; r->why = "update.need_net"; }
      else if (u->state == UPD_PREPARING)
         row_add(R_ACTION, ID_UPD_CANCEL, tr("update.cancel"), NULL);
      else if (u->state == UPD_READY)
         row_add(R_ACTION, ID_UPD_INSTALL, tr("update.install"), NULL);
      if (u->state != UPD_READY && (r = row_add(R_ACTION, ID_UPD_CHECK, tr("update.check"), NULL)) && (busy || !online))
      { r->enabled = 0; r->dim = 1; r->why = busy ? "update.busy" : "update.need_net"; }
      i = settings_get_int("update_auto", 1, 0, 1);
      if ((r = row_add(R_CHOICE, ID_UPD_AUTO, tr("update.auto"), tr(i ? "value.on" : "value.off"))))
      { r->at_lo = !i; r->at_hi = i; }
      if (u->last_check > 0)
      {
         time_t lt = (time_t)u->last_check;
         struct tm tmv;
         gmtime_r(&lt, &tmv);
         snprintf(num, sizeof(num), "%04d-%02d-%02d %02d:%02d", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min);
         row_add(R_INFO, ID_INFO, tr("update.last"), num);
      }
      snprintf(s_foot, sizeof(s_foot), "%s", tr("update.note"));
      break;
   }
   case P_DATETIME:
   {
      const net_state_t *ns = net_state();
      if ((r = row_add(R_CHOICE, ID_NET_TIME, tr("net.time"), tr(ns->time_on ? "value.on" : "value.off"))))
      { r->at_lo = !ns->time_on; r->at_hi = ns->time_on; }
      row_add(R_INFO, ID_INFO, tr("time.now"), ns->time_ok ? ns->time_text : tr("time.not_set"));
      snprintf(s_foot, sizeof(s_foot), "%s", tr("time.note"));
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

/* statvfs() goes to the stick's FUSE daemon and waits while the stick is busy (643 ms
 * on the console, on the frame the page opened): asked on a thread; the page shows the
 * last value known and is rebuilt when the new one arrives (panel_update). */
static SDL_atomic_t s_free_busy, s_free_new;
static int measure_free_thread(void *unused)
{
   struct statvfs st;
   (void)unused;
   if (statvfs(g_root, &st) == 0)
   {
      double b = (double)st.f_bavail * (double)st.f_frsize;
      if (b >= 1024.0 * 1024.0 * 1024.0) { s_free_mb = -1; s_free_tenths = (long)(b / (1024.0 * 1024.0 * 1024.0) * 10.0 + 0.5); }
      else { s_free_tenths = -1; s_free_mb = (long)(b / (1024.0 * 1024.0) + 0.5); }
   }
   SDL_AtomicSet(&s_free_new, 1);
   SDL_AtomicSet(&s_free_busy, 0);
   return 0;
}
static void measure_free(void)
{
   SDL_Thread *th;
   if (SDL_AtomicGet(&s_free_busy)) return;
   SDL_AtomicSet(&s_free_busy, 1);
   if ((th = SDL_CreateThread(measure_free_thread, "free", NULL))) SDL_DetachThread(th);
   else SDL_AtomicSet(&s_free_busy, 0);
}

/* place the highlight and the scroll without animation (page entry) */
static void rows_snap(void)
{
   s_hl.x = s_sel > 0 ? s_sel * ROW_H : 0.0f; s_hl.v = 0;
   s_scroll.x = 0; s_scroll.v = 0;
   s_rail.x = rail_pos(rail_of(s_page)) * RAIL_P; s_rail.v = 0;
}

static void page_enter(int page, int dir)
{
   int i;
   s_page = page;
   if (page == P_ABOUT) measure_free();
   if (page == P_AUDIO_OUT || page == P_SOUND) audio_scan();
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

/* a row with › that opens a sub-page, and the way back to the row it came from */
static void sub_enter(int page) { color_commit(); page_enter(page, 1); }
static void sub_back(void)
{
   int from = s_page;
   page_enter(k_parent[s_page], -1);
   select_row(from == P_MODULE ? ID_MOD + s_mod_sel : from);   /* a module's row is ID_MOD + its index */
   s_hl.x = s_sel > 0 ? s_sel * ROW_H : 0.0f; s_hl.v = 0;
}

static void row_action(const row_t *r)
{
   if (r->id > P_ROOT && r->id < P_COUNT) { sub_enter(r->id); return; }
   if (r->id >= ID_MOD && r->id < ID_MOD + modules_count()) { s_mod_sel = r->id - ID_MOD; sub_enter(P_MODULE); return; }
   if (r->id >= ID_AUDIO_BT && r->id < ID_AUDIO_BT + NET_MAX_DEVS)
   {
      char id[24];
      bt_out_id(id, sizeof(id), net_state()->dev[r->id - ID_AUDIO_BT].mac);
      settings_set("audio_out", id);
      logf_("panel: audio_out %s", id);
      /* paired but not connected: connect it now; the sound moves when it is (panel_audio_route) */
      if (!net_state()->dev[r->id - ID_AUDIO_BT].connected) net_bt_pair(net_state()->dev[r->id - ID_AUDIO_BT].mac);
      return;
   }
   if (r->id >= ID_AUDIO_OUT && r->id <= ID_AUDIO_OUT + AO_MAX)
   {
      const char *id = r->id == ID_AUDIO_OUT ? "auto" : s_ao[r->id - ID_AUDIO_OUT - 1].id;
      char keep[24];
      snprintf(keep, sizeof(keep), "%s", id);
      settings_set("audio_out", keep);
      logf_("panel: audio_out %s", keep);
      return;
   }
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
   else if (r->id == ID_KERNEL) kern_open();
   else if (r->id == ID_NET_WIFI_LIST) conn_open_wifi();
   else if (r->id == ID_NET_BT_LIST) conn_open_bt();
   else if (r->id == ID_NET_BT_ADD) conn_open_bt_add();
   else if (r->id == ID_UPD_CHECK) { if (net_online()) update_check(); else panel_toast("update.need_net"); }
   else if (r->id == ID_UPD_GET) { if (net_online()) update_prepare(); else panel_toast("update.need_net"); }
   else if (r->id == ID_UPD_CANCEL) update_cancel();
   else if (r->id == ID_UPD_INSTALL)
   {
      trf(s_cbuf, sizeof(s_cbuf), "update.install_confirm", "v", update_info()->version, NULL);
      confirm_open_ex(s_cbuf, "update.install_note", "update.install_yes", update_yes);
   }
   else if (r->id == ID_SYS_REBOOT) confirm_open_ex(tr("system.reboot_confirm"), NULL, "confirm.restart", restart_yes);
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
   case ID_NET_WIFI: net_wifi_set(dir > 0); break;
   case ID_NET_BT:   net_bt_set(dir > 0); break;
   case ID_NET_TIME: net_time_set(dir > 0); break;
   case ID_MOD_SWITCH:
   {  /* also what it requires (on) / what requires it (off): the toast says so */
      int more = module_set(s_mod_sel, dir > 0);
      if (more > 0) panel_toast(dir > 0 ? "mods.toast_more_on" : "mods.toast_more_off");
      else if (more == 0 && (module_at(s_mod_sel)->flags & MOD_F_RESTART)) panel_toast("mods.toast_restart");
      break;
   }
   case ID_UPD_AUTO: settings_set_int("update_auto", dir > 0); break;
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
      int to = nav->down ? rail_step(s_page, 1) : nav->up ? rail_step(s_page, -1) : s_page;
      if (to != s_page && to >= 1 && to < P_RAIL_END) page_enter(to, 0);
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
   {  /* up one level: a sub-page to its category, a category to the rail */
      color_commit();
      if (k_parent[s_page]) sub_back();
      else s_in_card = 0;
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
      label_set(&s_ksym[k], g_font_sub, k_sym[k]);
   }
   s_osk_chars = OSK_MAX_CHARS; s_osk_bytes = OSK_MAX_BYTES; s_osk_raw = 0; s_sym = 0;
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
   for (k = 0; k < 40; k++) { label_free(&s_kup[k]); label_free(&s_klo[k]); label_free(&s_ksym[k]); }
   for (k = 0; k < 4; k++) label_free(&s_kact[k]);
   label_free(&s_ktitle); label_free(&s_ktext);
}

static void osk_type(const char *s)
{
   size_t len = strlen(s_text), add = strlen(s);
   int was_empty = len == 0;
   if (utf8_chars(s_text) >= s_osk_chars || len + add > (size_t)s_osk_bytes) return;
   memcpy(s_text + len, s, add + 1);
   if (was_empty && !s_lower && !s_sym && !s_osk_raw && strcmp(s, " ")) s_lower = 1;   /* first letter upper case, then lower */
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
   if (start + strlen(k_accents[fg][i]) > (size_t)s_osk_bytes) return;
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
   char name[OSK_TEXT_MAX + 4];
   const char *b = s_text, *err;
   size_t n;
   if (s_osk_raw)
   {  /* a password: exactly as typed; accept() judges it */
      err = s_osk_accept ? s_osk_accept(s_text) : NULL;
      if (err) { panel_toast(err); return; }
      s_modal = M_NONE;
      return;
   }
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
         osk_type(s_sym ? k_sym[k] : s_lower ? s_klower[k] : k_keys[k]);
      }
      else switch (osk_act_of_col(s_ocol))
      {
      case OK_SHIFT:   /* A -> a -> #+= -> A */
         if (s_sym) { s_sym = 0; s_lower = 0; }
         else if (s_lower) { s_sym = 1; s_lower = 0; }
         else s_lower = 1;
         break;
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
         else ui_fill(mx(x), my(y), w, 36, 10, white, ((r == 4 && k == OK_SHIFT && (!s_lower || s_sym)) ? UI_SELECTED : UI_REST) * a);
         if (r < 4) l = s_sym ? &s_ksym[r * 10 + k] : (s_lower && s_klo[r * 10 + k].tex) ? &s_klo[r * 10 + k] : &s_kup[r * 10 + k];
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
   s_page = P_PROFILE;
   if (theme_active_index() > 0)
   {
      theme_color(TC_TEXT, C_TXT); theme_color(TC_DIM, C_DIM);
      theme_color(TC_ACCENT, C_ACC); theme_color(TC_DANGER, C_RED);
   }
}

/* the keyboard or a question for a card over the panel (conn.c): the card steps aside
 * (closes) and calls this; the panel, open underneath, owns the input until the modal
 * closes, and the card comes back when panel_modal_open() is 0 again */
void panel_keyboard(const char *title_text, int max_chars, int password, const char *(*accept)(const char *))
{
   if (!s_open || s_modal != M_NONE) return;
   osk_open("panel.title", "", accept);
   snprintf(s_ktitle_txt, sizeof(s_ktitle_txt), "%s", title_text);
   s_osk_chars = max_chars > 0 ? max_chars : OSK_MAX_CHARS;
   s_osk_bytes = s_osk_chars * 2 < OSK_TEXT_MAX ? s_osk_chars * 2 : OSK_TEXT_MAX;
   s_osk_raw = password;
   s_lower = password;   /* a password starts in lower case */
}
void panel_confirm(const char *message, const char *yes_key, void (*on_yes)(void))
{
   if (!s_open || s_modal != M_NONE) return;
   confirm_open_ex(message, NULL, yes_key, on_yes);
}
int panel_modal_open(void) { return s_open && s_modal != M_NONE; }
int panel_connections_page(void) { return s_open && (s_page == P_NETWORK || s_page == P_BLUETOOTH || s_page == P_SYSTEM || s_page == P_DATETIME); }

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
   {  /* back where it was left, unless a module took that category away */
      int pg = s_page >= 1 && s_page < P_RAIL_END ? s_page : rail_of(s_page) >= 1 ? rail_of(s_page) : P_PROFILE;
      if (!rail_shown(pg)) pg = P_PROFILE;
      page_enter(pg, 0);
   }
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
   if (SDL_AtomicGet(&s_free_new)) { SDL_AtomicSet(&s_free_new, 0); if (s_open && s_page == P_ABOUT) page_build(); }
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
   spring_step(&s_help_a, s_help_target, 260, 30, dt);
   if (s_sel != s_sel_row || s_page != s_sel_page || s_in_card != s_sel_card)
   { s_sel_row = s_sel; s_sel_page = s_page; s_sel_card = s_in_card; s_sel_t = t; }
   spring_step(&s_pa, 1.0f, 120, 16, dt);
   {
      /* keep the selected row inside the visible rows area */
      float view = CARD_H - HEAD_H - help_room() - 12.0f;
      float y = s_sel >= 0 ? s_sel * ROW_H : 0.0f, target = s_scroll.x;
      if (s_sel >= 0)
      {
         if (y < target) target = y;
         if (y + ROW_H > target + view) target = y + ROW_H - view;
      }
      if (target < 0) target = 0;
      spring_step(&s_hl, y, 220, 22, dt);
      spring_step(&s_scroll, target, 220, 22, dt);
      spring_step(&s_rail, rail_pos(rail_of(s_page)) * RAIL_P, 260, 26, dt);
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

/* the help line's room at the foot of the card: three lines of the small font, kept the same
 * whatever the text, so the rows never jump when the focus moves */
static float help_room(void)
{
   static float h;
   if (h <= 0.0f)
   {
      label_t m = { 0 };
      label_set(&m, g_font_small, "Ag");
      h = m.h > 0 ? m.h * 3.0f + 22.0f : 92.0f;
      label_free(&m);
   }
   return h;
}

/* a row's value, right-aligned at xr in at most maxw px. Too long: cut with an ellipsis, and
 * on the focused row the whole text glides left and back (pause, 42 px/s, pause), clipped to
 * its box. Returns the width it takes. */
static float draw_value(int i, const row_t *r, float xr, float y, float maxw, int focused, double t,
                        float clip_top, float clip_bot, const float c[4])
{
   label_t *v = &s_rv[i];
   float w = v->w;
   if (maxw < 40.0f) maxw = 40.0f;
   if (w <= maxw) { draw_label(v, xr - w, y + (ROW_H - v->h) * 0.5f, c); return w; }
   if (focused)
   {
      const float over = w - maxw, move = over / 42.0f + 0.4f, cyc = 1.4f + move + 1.4f + move;
      float ph = (float)fmod(t - s_sel_t, cyc), k, x0 = xr - maxw;
      if (ph < 1.4f) k = 0.0f;
      else if (ph < 1.4f + move) k = (ph - 1.4f) / move;
      else if (ph < 2.8f + move) k = 1.0f;
      else k = 1.0f - (ph - 2.8f - move) / move;
      k = k * k * (3.0f - 2.0f * k);   /* smoothstep: no jerk at the turns */
      gl_scissor(x0, fmaxf(y, clip_top), maxw, fminf(y + ROW_H, clip_bot) - fmaxf(y, clip_top));
      draw_label(v, x0 - over * k, y + (ROW_H - v->h) * 0.5f, c);
      gl_scissor(CARD_X + 2, clip_top, CARD_W - 4, clip_bot - clip_top);
      return maxw;
   }
   {
      char buf[160];
      label_set(&s_rvf[i], g_font_sub, ui_fit(g_font_sub, r->value, maxw, buf, sizeof(buf)));
      draw_label(&s_rvf[i], xr - s_rvf[i].w, y + (ROW_H - s_rvf[i].h) * 0.5f, c);
      return s_rvf[i].w;
   }
}

void panel_draw(double t)
{
   /* the section rises 0..34 px into place while it fades in; a modal or the file picker
    * over it takes the stage: the section fades out under them */
   const float m = clampf(s_mod.x, 0.0f, 1.0f), fp = fmaxf(fmaxf(fmaxf(filepicker_amount(), conn_amount()), kern_amount()), fmaxf(transfer_amount(), audit_amount())), over = m > fp ? m : fp;
   float a = panel_amount() * (1.0f - over), rise = s_slide.x * 0.06f;
   float ca, cx, cy = CARD_Y + rise, top, bottom, c[4], f[4];
   char buf[200], num[80];
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
   for (i = 1; i < P_RAIL_END; i++)
   {
      label_t *l = &s_rail_l[i];
      if (!rail_shown(i)) continue;
      label_set(l, g_font_sub, tr(k_page_key[i]));
      col(c, C_TXT, a * (i == rail_of(s_page) ? 1.0f : 0.86f));
      draw_label(l, RAIL_X + 20, RAIL_Y + rise + rail_pos(i) * RAIL_P + (RAIL_H - l->h) * 0.5f, c);
   }

   /* the card: a header that gives the section a face, then its rows */
   ui_card(CARD_X, cy, CARD_W, CARD_H, a);
   {
      float hx = cx + PAD;
      const char *title = tr(k_page_key[s_page]), *kick = tr(k_page_key[k_parent[s_page]]), *cap = "";
      if (s_page == P_MODULE)
      {  /* a module: its name, and its state under it */
         title = tr(module_name_key(s_mod_sel));
         cap = mod_state_text(s_mod_sel, buf, sizeof(buf));
      }
      if (s_page == P_PROFILE)
      {
         int played = stats_games_played();
         ui_avatar_draw(cx + PAD, cy + 26, 72, ca);
         hx += 72 + 22;
         kick = tr("section.profile"); title = profiles_display_name(profiles_active());
         snprintf(num, sizeof(num), "%d", played);
         cap = trf(buf, sizeof(buf), played == 1 ? "dash.profile_games_one" : "dash.profile_games", "n", num, NULL);
      }
      if (s_page == P_UPDATE)
      {  /* the version, and what is going on */
         const update_info_t *u = update_info();
         title = u->state == UPD_AVAILABLE || u->state == UPD_READY || u->state == UPD_PREPARING ?
                 trf(buf, sizeof(buf), "update.title_new", "v", u->version, NULL) : trf(buf, sizeof(buf), "update.title", "v", u->installed[0] ? u->installed : LUMEN_VERSION, NULL);
         cap = update_status(num, sizeof(num));
         if (u->state == UPD_PREPARING)
         {  /* a progress bar under the caption */
            float pb[4], pf[4];
            col(pb, (const float[3]){ 1, 1, 1 }, 0.10f * ca); col(pf, C_ACC, ca);
            draw_round_rect(hx, cy + 112, CARD_W - 2 * PAD - (hx - cx - PAD), 6, 3, pb, 0.0f);
            draw_round_rect(hx, cy + 112, fmaxf(6.0f, (CARD_W - 2 * PAD - (hx - cx - PAD)) * clampf(u->progress, 0.0f, 1.0f)), 6, 3, pf, 0.0f);
         }
      }
      if (s_page == P_BLUETOOTH)
      {
         const net_state_t *ns = net_state();
         char n1[16], n2[16];
         snprintf(n1, sizeof(n1), "%d", ns->paired); snprintf(n2, sizeof(n2), "%d", ns->connected);
         cap = !ns->bt_on ? tr("value.off") : ns->bt == NET_NOADAPTER ? tr("net.bt_noadapter") : trf(buf, sizeof(buf), "net.paired_n", "n", n1, "c", n2, NULL);
      }
      if (s_page == P_NETWORK)
      {  /* the connection's face: online / offline / unavailable, then where and the clock */
         const net_state_t *ns = net_state();
         const int wifi = ns->wifi == NET_CONNECTED, eth = ns->eth == NET_CONNECTED;
         size_t o = 0;
         title = tr(wifi || eth ? "net.online" : (ns->wifi_cap == NET_CAP_NO && ns->eth_cap == NET_CAP_NO) ? "net.unavailable" : "net.offline");
         buf[0] = 0;
         if (eth) o += (size_t)snprintf(buf + o, sizeof(buf) - o, "%s  \xC2\xB7  %s", tr("net.cable"), ns->eth_ip);
         else if (wifi) o += (size_t)snprintf(buf + o, sizeof(buf) - o, "%s  \xC2\xB7  %s", ns->ssid, ns->ip);
         if (ns->time_ok && o < sizeof(buf)) o += (size_t)snprintf(buf + o, sizeof(buf) - o, "%s%s", o ? "  \xC2\xB7  " : "",
                                                                   trf(num, sizeof(num), "net.time_set", "t", ns->time_text + 11, NULL));
         cap = buf;
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

   /* the help line at the foot of the card (a console's settings): what the focused row does,
    * else the page's note; on the rail, what the category holds. A new text fades in over the
    * old one and rises 6 px; the room it takes never changes */
   {
      const row_t *fr = s_in_card && s_sel >= 0 && s_sel < s_nrows ? &s_rows[s_sel] : NULL;
      const char *help = !s_in_card ? (k_rail_desc[rail_of(s_page)] ? tr(k_rail_desc[rail_of(s_page)]) : "")
                         : fr && fr->desc ? tr(fr->desc) : s_foot;
      const float room = help_room(), hy = cy + CARD_H - room + 6.0f, ha = clampf(s_help_a.x, 0.0f, 1.0f);
      label_t *cur = &s_help;
      size_t kn = sizeof(cur->key) - 1;
      int same;
      if (strncmp(s_help_want, help, sizeof(s_help_want) - 1))
      { snprintf(s_help_want, sizeof(s_help_want), "%s", help); s_help_want_t = t; }
      same = strlen(s_help_want) >= kn ? !strncmp(cur->key, s_help_want, kn) : !strcmp(cur->key, s_help_want);
      if (same) s_help_target = 1.0f;
      else
      {  /* out first; the new text only when the old one is gone and the focus has rested */
         s_help_target = 0.0f;
         if ((ha < 0.03f || !cur->tex) && t - s_help_want_t >= HELP_REST_S)
         {
            wlabel_set(cur, g_font_small, s_help_want, (int)(CARD_W - 2 * PAD - 12));
            if (!cur->tex) snprintf(cur->key, sizeof(cur->key), "%s", s_help_want);
            s_help_a.x = 0.0f; s_help_a.v = 0.0f;
            s_help_target = 1.0f;
         }
      }
      col(f, (const float[3]){ 1, 1, 1 }, 0.08f * ca);
      draw_2d(0, CARD_X + PAD, hy - 10.0f, CARD_W - 2 * PAD, 1, f, 0);
      col(c, C_DIM, ca * ha);
      draw_label(cur, cx + PAD + 6, hy + (1.0f - ha) * 4.0f, c);
      bottom = hy - 18.0f;
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
      const int focused = i == s_sel && s_in_card;
      float lend;
      {  /* the label keeps at most 62 % of the row, cut with an ellipsis beyond */
         char buf[160];
         label_set(&s_rl[i], g_font_sub, ui_fit(g_font_sub, r->label, (CARD_W - 68) * 0.62f, buf, sizeof(buf)));
      }
      label_set(&s_rv[i], g_font_sub, r->value);
      if (y + ROW_H < top - 8 || y > bottom + 8) continue;
      col(c, C_TXT, la);
      draw_label(&s_rl[i], cx + 14 + 20, y + (ROW_H - s_rl[i].h) * 0.5f, c);
      lend = cx + 14 + 20 + s_rl[i].w + 28.0f;   /* nothing of the value left of here */
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
            float extra = (r->swatch >= 0 ? 30.0f : 0.0f) + s_lt.w + 12.0f;
            col(c, C_TXT, la * 0.92f);
            xr -= draw_value(i, r, xr, y, xr - lend - extra, focused, t, top - 8, bottom + 8, c) + 12;
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
         {
            float extra = r->swatch >= 0 ? 30.0f : r->avatar ? 42.0f : 0.0f, vw;
            col(c, C_TXT, la * 0.92f);
            if (r->type == R_ACTION && !r->enter) col(c, C_DIM, la);
            vw = draw_value(i, r, xr, y, xr - lend - extra, focused, t, top - 8, bottom + 8, c);
            if (r->swatch >= 0) draw_swatch(r->swatch, xr - vw - 12, y, la);
            if (r->avatar) ui_avatar_draw(xr - vw - 12 - 30, y + (ROW_H - 30) * 0.5f, 30, la);
         }
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
