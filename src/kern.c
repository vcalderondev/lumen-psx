/* Kernel card, see kern.h.
 *
 * States: the offer (restore Sony's, or go back to the previous kernel, with the steps
 * and the warning; Cancel is the default focus), running (the steps with the activity
 * dots, no input at all: nothing may interrupt it), done (steps checked, where the backup
 * is, restart now / later) and failed (what happened and what to do). kernel.sh does the
 * work and every check; it runs on a thread, its "step ..." lines move the card. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "lumen.h"
#include "kern.h"
#include "i18n.h"
#include "music.h"
#include "panel.h"
#include "theme.h"
#include "ui.h"

#define CW 880.0f
#define CH 536.0f
#define PX 32.0f

enum { K_UNKNOWN, K_STOCK, K_ERIS47, K_MMC2019, K_OTHER };
enum { S_OFFER, S_RUNNING, S_DONE, S_FAILED };

static char  s_script[300];
static SDL_atomic_t s_known;                 /* status read */
static int   s_kernel = K_UNKNOWN, s_boot2_ok;
static char  s_backup[400];                  /* newest backup file, "" none */
static int   s_open, s_state, s_btn, s_rollback;
static int   s_backup_only;                   /* Square in the offer: the firmware copy alone, nothing written */
static SDL_atomic_t s_step, s_result;        /* 1..3 while running; result: 0 none, 1 ok, 2 failed */
static char  s_err[24], s_done_file[400];
static char  s_fw[32];                        /* the partition the firmware copy is reading */
static double s_t0;
static spring s_fade;
static label_t s_kick, s_title, s_cap, s_step_l[3], s_warn, s_note, s_path, s_btn_l[2], s_num[3];

static int kernel_of(const char *v)
{
   return !strcmp(v, "stock") ? K_STOCK : !strcmp(v, "eris47") ? K_ERIS47 : !strcmp(v, "mmc2019") ? K_MMC2019 : K_OTHER;
}

static int status_thread(void *u)
{
   char cmd[400], line[512];
   FILE *f;
   (void)u;
   snprintf(cmd, sizeof(cmd), "sh '%s' status 2>/dev/null", s_script);
   if ((f = popen(cmd, "r")))
   {
      while (fgets(line, sizeof(line), f))
      {
         line[strcspn(line, "\n")] = 0;
         if (!strncmp(line, "kernel=", 7)) s_kernel = kernel_of(line + 7);
         else if (!strncmp(line, "boot2=", 6)) s_boot2_ok = !strcmp(line + 6, "stock");
         else if (!strncmp(line, "backup=", 7)) snprintf(s_backup, sizeof(s_backup), "%s", strcmp(line + 7, "none") ? line + 7 : "");
      }
      pclose(f);
   }
   logf_("kernel: %s, BOOTIMG2 %s, backup %s", s_kernel == K_STOCK ? "stock" : s_kernel == K_ERIS47 ? "eris47" :
         s_kernel == K_MMC2019 ? "mmc2019" : "other", s_boot2_ok ? "stock" : "NOT stock", s_backup[0] ? s_backup : "none");
   SDL_AtomicSet(&s_known, 1);
   return 0;
}

void kern_init(void)
{
   const char *o = getenv("LUMEN_KERNEL_SH");
   SDL_Thread *th;
   snprintf(s_script, sizeof(s_script), "%s", o && *o ? o : "");
   if (!s_script[0]) snprintf(s_script, sizeof(s_script), "%s/kernel.sh", g_home);
   if (access(s_script, R_OK) != 0) return;
   if ((th = SDL_CreateThread(status_thread, "kstatus", NULL))) SDL_DetachThread(th);
}

const char *kern_name_key(void)
{
   if (!SDL_AtomicGet(&s_known)) return "";
   switch (s_kernel)
   {
   case K_STOCK: return "kern.stock";
   case K_ERIS47: return "kern.eris47";
   case K_MMC2019: return "kern.mmc2019";
   default: return "kern.other";
   }
}

/* a backup path from kernel.sh: only the characters it writes */
static int safe_path(const char *p)
{
   for (; *p; p++) if (!strchr("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789/_.-", *p)) return 0;
   return 1;
}

static int work_thread(void *u)
{
   char cmd[900], line[512];
   FILE *f;
   int ok = 0;
   (void)u;
   if (s_backup_only) snprintf(cmd, sizeof(cmd), "sh '%s' firmware-backup 2>/dev/null", s_script);
   else if (s_rollback) snprintf(cmd, sizeof(cmd), "sh '%s' restore-backup '%s' 2>/dev/null", s_script, s_backup);
   else snprintf(cmd, sizeof(cmd), "sh '%s' restore-stock 2>/dev/null", s_script);
   logf_("kernel: %s", s_rollback ? "restoring the previous kernel" : "restoring Sony's stock kernel");
   snprintf(s_err, sizeof(s_err), "other");
   if ((f = popen(cmd, "r")))
   {
      while (fgets(line, sizeof(line), f))
      {
         line[strcspn(line, "\n")] = 0;
         logf_("kernel: %s", line);
         if (!strcmp(line, "step firmware")) SDL_AtomicSet(&s_step, 1);
         else if (!strncmp(line, "fw ", 3)) snprintf(s_fw, sizeof(s_fw), "%s", line + 3);
         else if (!strcmp(line, "step backup")) { s_fw[0] = 0; SDL_AtomicSet(&s_step, 1); }
         else if (!strcmp(line, "step write")) SDL_AtomicSet(&s_step, 2);
         else if (!strcmp(line, "step verify")) SDL_AtomicSet(&s_step, 3);
         else if (!strncmp(line, "backup ", 7)) snprintf(s_done_file, sizeof(s_done_file), "%s", line + 7);
         else if (!strncmp(line, "ok ", 3)) { ok = 1; snprintf(s_done_file, sizeof(s_done_file), "%s", line + 3); }
         else if (!strncmp(line, "firmware ", 9) && s_backup_only) { ok = 1; snprintf(s_done_file, sizeof(s_done_file), "%s/MANIFEST", line + 9); }
         else if (!strncmp(line, "error ", 6)) snprintf(s_err, sizeof(s_err), "%s", line + 6);
      }
      pclose(f);
   }
   SDL_AtomicSet(&s_result, ok ? 1 : 2);
   return 0;
}

void kern_open(void)
{
   if (s_open || !SDL_AtomicGet(&s_known)) { if (!SDL_AtomicGet(&s_known)) panel_toast("kern.unknown"); return; }
   s_rollback = s_kernel == K_STOCK; s_backup_only = 0;
   if (s_rollback && !s_backup[0]) { panel_toast("kern.is_stock"); return; }
   s_open = 1; s_state = S_OFFER; s_btn = 0; s_t0 = now_s();
   SDL_AtomicSet(&s_step, 0); SDL_AtomicSet(&s_result, 0); s_done_file[0] = 0; s_fw[0] = 0;
   if (s_fade.x < 0.02f) { s_fade.x = 0; s_fade.v = 0; }
   music_sfx(SFX_OPEN);
}
int   kern_active(void) { return s_open || s_fade.x > 0.01f; }
int   kern_open_now(void) { return s_open; }
float kern_amount(void) { return clampf(s_fade.x, 0.0f, 1.0f); }
int   kern_hint_set(void) { return s_state == S_RUNNING ? HINTS_KERNEL_BUSY : s_state == S_OFFER ? HINTS_KERNEL : HINTS_KERNEL_DONE; }

void kern_update(const ui_nav_t *nav, double t, float dt)
{
   spring_step(&s_fade, s_open ? 1.0f : 0.0f, 120, 18, dt);
   if (!s_open) return;
   if (s_state == S_RUNNING)
   {  /* no input while it runs */
      int r = SDL_AtomicGet(&s_result);
      if (r)
      {
         s_state = r == 1 ? S_DONE : S_FAILED; s_btn = r == 1 ? 1 : 0;
         music_sfx(r == 1 ? SFX_CONFIRM : SFX_BACK);
         SDL_AtomicSet(&s_known, 0); kern_init();   /* status again: the backup list changed */
      }
      return;
   }
   if (!nav || t - s_t0 < 0.15) return;
   if (nav->left && s_btn) { s_btn = 0; music_sfx(SFX_MOVE_PREV); }
   if (nav->right && !s_btn && (s_state != S_FAILED)) { s_btn = 1; music_sfx(SFX_MOVE_NEXT); }
   /* Square counts as back too (ui_nav_t): ask for it first */
   if (s_state == S_OFFER && nav->erase)
   {  /* Square: only the read-only firmware copy, to have it (and off the stick) first */
      SDL_Thread *th;
      s_backup_only = 1; s_state = S_RUNNING;
      SDL_AtomicSet(&s_step, 1);
      music_sfx(SFX_CONFIRM);
      if ((th = SDL_CreateThread(work_thread, "kernel", NULL))) SDL_DetachThread(th);
      else { snprintf(s_err, sizeof(s_err), "other"); SDL_AtomicSet(&s_result, 2); }
      return;
   }
   if (nav->back || nav->options) { s_open = 0; music_sfx(SFX_BACK); return; }
   if (!nav->confirm) return;
   if (s_state == S_OFFER)
   {
      SDL_Thread *th;
      if (!s_btn) { s_open = 0; music_sfx(SFX_BACK); return; }
      if (!s_boot2_ok && !s_rollback) { panel_toast("kern.err_boot2"); return; }
      {  /* while this is being proven on consoles: the write needs <home>/kernel-write-ok */
         char flag[400];
         snprintf(flag, sizeof(flag), "%s/kernel-write-ok", g_home);
         if (access(flag, F_OK) != 0) { panel_toast("kern.locked"); logf_("kernel: write locked (no %s)", flag); return; }
      }
      if (s_rollback && !safe_path(s_backup)) { panel_toast("kern.err_image"); return; }
      s_state = S_RUNNING;
      music_sfx(SFX_CONFIRM);
      if ((th = SDL_CreateThread(work_thread, "kernel", NULL))) SDL_DetachThread(th);
      else { snprintf(s_err, sizeof(s_err), "other"); SDL_AtomicSet(&s_result, 2); }
   }
   else if (s_state == S_DONE)
   {
      s_open = 0;
      if (s_btn && !s_backup_only) ui_request_reboot();
   }
   else s_open = 0;   /* failed: one button, close */
}

static void col4(float o[4], int tc, float a) { theme_color(tc, o); o[3] = a; }
static void dl(const label_t *l, float x, float y, const float c[4]) { if (l->tex && c[3] > 0.003f) draw_2d(l->tex, floorf(x), floorf(y), l->w, l->h, c, 0); }
static void wrap(label_t *l, TTF_Font *f, const char *s, int w)
{
   size_t n = sizeof(l->key) - 1;
   int same = strlen(s) >= n ? !strncmp(l->key, s, n) : !strcmp(l->key, s);
   if (same && (l->tex || !*s)) return;
   label_free(l); snprintf(l->key, sizeof(l->key), "%s", s);
   if (*s) l->tex = text_tex_wrapped(f, s, w, &l->w, &l->h);
}

void kern_draw(double t)
{
   const float a = kern_amount(), cx = (W - CW) * 0.5f, cy = 104.0f + (1 - a) * 16, tw = CW - 2 * PX;
   float txt[4], dim[4], acc[4], red[4], sep[4];
   char buf[600];
   const char *title, *cap, *b0, *b1;
   int i, step = SDL_AtomicGet(&s_step);
   if (a <= 0.003f) return;
   col4(txt, TC_TEXT, a); col4(dim, TC_DIM, a); col4(acc, TC_ACCENT, a); col4(red, TC_DANGER, a); col4(sep, TC_TEXT, 0.08f * a);
   gl_use_2d();
   ui_scrim(a * (1.0f - panel_amount()));
   ui_card(cx, cy, CW, CH, a);
   ui_kicker(&s_kick, tr("about.kernel"), cx + PX, cy + 26, a);
   if (s_state == S_DONE && s_backup_only) { title = tr("kern.fw_done_title"); cap = tr("kern.fw_done_note"); }
   else if (s_state == S_DONE) { title = tr(s_rollback ? "kern.done_rb_title" : "kern.done_title"); cap = tr("kern.done_note"); }
   else if (s_state == S_FAILED)
   {
      title = tr(!strcmp(s_err, "revert") ? "kern.err_revert_title" : !strcmp(s_err, "write") ? "kern.err_write_title" : "kern.err_title");
      snprintf(buf, sizeof(buf), "kern.err_%s", s_err);
      cap = tr(buf);
      if (!strcmp(cap, buf)) cap = tr("kern.err_other");
   }
   else { title = tr(s_rollback ? "kern.stock" : kern_name_key()); cap = tr(s_rollback ? "kern.rb_why" : s_boot2_ok ? "kern.why" : "kern.err_boot2"); }
   label_set(&s_title, g_font_title, title);
   dl(&s_title, cx + PX, cy + 44, txt);
   wrap(&s_cap, g_font_small, cap, (int)tw);
   dl(&s_cap, cx + PX, cy + 88, (s_state == S_FAILED && !strcmp(s_err, "revert")) ? red : dim);
   draw_2d(0, cx + PX, cy + 138, tw, 1, sep, 0);
   {
   float sy = cy + 156;
   const int nsteps = s_backup_only && s_state != S_OFFER ? 1 : 3;   /* backup only: step 1 alone */
   for (i = 0; i < nsteps; i++)
   {  /* the steps: numbered in the offer, the dots on the running one, checked when done */
      static const char *const k_restore[3] = { "kern.step1", "kern.step2", "kern.step3" };
      static const char *const k_rb[3] = { "kern.step1", "kern.rb_step2", "kern.step3" };
      const float y = sy, sx = cx + PX + 34;
      char n[4];
      float c[4];
      wrap(&s_step_l[i], g_font_sub, tr(s_rollback ? k_rb[i] : k_restore[i]), (int)(tw - 40));
      memcpy(c, txt, sizeof(c));
      if (s_state == S_RUNNING && i + 1 > step) c[3] *= 0.5f;
      dl(&s_step_l[i], sx, y, c);
      sy += (s_step_l[i].h > 0 ? s_step_l[i].h : 28) + 8;
      if (s_state == S_DONE || (s_state == S_RUNNING && i + 1 < step))
         draw_round_rect(cx + PX + 8, y + s_step_l[i].h * 0.5f - 6, 12, 12, 6, acc, 0.0f);
      else if (s_state == S_RUNNING && i + 1 == step)
         for (int d = 0; d < 3; d++)
         {
            float ph = (float)fmod(t * 1.6 - d * 0.18, 1.0), da = 0.35f + 0.65f * (ph < 0.5f ? 1.0f - ph * 2.0f : 0.0f);
            const float dc[4] = { acc[0], acc[1], acc[2], acc[3] * da };
            draw_round_rect(cx + PX + d * 9, y + s_step_l[i].h * 0.5f - 3, 6, 6, 3, dc, 0.0f);
         }
      else
      {
         snprintf(n, sizeof(n), "%d", i + 1);
         label_set(&s_num[i], g_font_sub, n);
         dl(&s_num[i], cx + PX + 2, y, dim);
      }
   }
   }
   {
      float y = cy + 290;
      if (s_state == S_OFFER || s_state == S_RUNNING)
      {
         if (s_state == S_RUNNING && s_fw[0] && step == 1)
         {  /* the first time: the whole firmware is copied, partition by partition */
            char run[300];
            snprintf(run, sizeof(run), "%s  %s", tr("kern.running_copy"), trf(buf, sizeof(buf), "kern.fw_copying", "p", s_fw, NULL));
            wrap(&s_warn, g_font_small, run, (int)tw);
         }
         else wrap(&s_warn, g_font_small, tr(s_state == S_RUNNING ? "kern.running" : "kern.warn"), (int)tw);
         dl(&s_warn, cx + PX, y, red);
         y += s_warn.h + 22;
         if (s_state == S_OFFER) { wrap(&s_note, g_font_small, tr(s_rollback ? "kern.rb_note" : "kern.undo"), (int)tw); dl(&s_note, cx + PX, y, dim); }
      }
      else if (s_done_file[0] && (s_state == S_DONE || !strcmp(s_err, "revert") || !strcmp(s_err, "write")))
      {
         const char *f = strrchr(s_done_file, '/');
         label_set(&s_path, g_font_small, trf(buf, sizeof(buf), s_backup_only ? "kern.fw_at" : "kern.backup_at", "f", f ? f + 1 : "", NULL));
         dl(&s_path, cx + PX, y, txt);
         wrap(&s_note, g_font_small, tr("kern.copy_note"), (int)tw);
         dl(&s_note, cx + PX, y + s_path.h + 10, dim);
      }
   }
   if (s_state == S_RUNNING) return;
   if (s_state == S_DONE && s_backup_only)
   {  /* nothing was written: one button */
      const float by = cy + 452, white[3] = { 1, 1, 1 }, ink[4] = { 0.043f, 0.047f, 0.063f, a };
      label_set(&s_btn_l[0], g_font_medium, tr("kern.close"));
      ui_fill(cx + PX, by, tw, 52, 26, white, a);
      ui_ring(cx + PX, by, tw, 52, 26, 4.0f, a);
      dl(&s_btn_l[0], cx + PX + (tw - s_btn_l[0].w) * 0.5f, by + (52 - s_btn_l[0].h) * 0.5f, ink);
      return;
   }
   /* buttons: Cancel / the action (danger), or Later / Restart (primary), or Close */
   b0 = tr(s_state == S_DONE ? "kern.later" : s_state == S_FAILED ? "kern.close" : "confirm.cancel");
   b1 = s_state == S_DONE ? tr("kern.reboot") : tr(s_rollback ? "kern.rollback_btn" : "kern.restore_btn");
   label_set(&s_btn_l[0], g_font_medium, b0);
   label_set(&s_btn_l[1], g_font_medium, b1);
   {
      const float by = cy + 452, bw = (tw - 24) * 0.5f, white[3] = { 1, 1, 1 }, ink[4] = { 0.043f, 0.047f, 0.063f, a };
      const int nb = s_state == S_FAILED ? 1 : 2;
      for (i = 0; i < nb; i++)
      {
         const float bx = cx + PX + i * (bw + 24), w = nb == 1 ? tw : bw;
         const int primary = s_state == S_DONE && i == 1;
         if (primary) ui_fill(bx, by, w, 52, 26, white, a);
         else if (s_btn == i) ui_focus(bx, by, w, 52, 26, a);
         else ui_rest(bx, by, w, 52, 26, a);
         if (s_btn == i) ui_ring(bx, by, w, 52, 26, 4.0f, a);
         dl(&s_btn_l[i], bx + (w - s_btn_l[i].w) * 0.5f, by + (52 - s_btn_l[i].h) * 0.5f,
            primary ? ink : (i == 1 && s_state == S_OFFER) ? red : txt);
      }
   }
}
