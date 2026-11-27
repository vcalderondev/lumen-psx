/* File picker, see filepicker.h.
 *
 * Static storage, like the other UI modules: one folder listed at a time (folders first,
 * then the files that match the request, by name), every text a label_t that re-renders
 * only when it changes. Nothing slow runs on the render thread: one worker thread decodes
 * the preview of the highlighted file (a photo of several megapixels takes over a second
 * on the console) and runs the caller's accept(); the render thread only uploads the
 * small surface the worker hands back. */
#define _GNU_SOURCE
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include "lumen.h"
#include "filepicker.h"
#include "image.h"
#include "i18n.h"
#include "panel.h"
#include "settings.h"
#include "theme.h"
#include "ui.h"

/* ------------------------------------------------------------------------ */
/* geometry (logical 1280x720)                                               */
#define CW       940.0f    /* card                         */
#define CH       520.0f
#define PADX     28.0f
#define LIST_X   14.0f     /* list, relative to the card   */
#define LIST_Y   110.0f
#define LIST_W   590.0f
#define ROW      46.0f
#define VIS      8         /* rows visible                 */
#define PV       220.0f    /* preview box                  */
#define PV_PX    384       /* preview texture, longest side */
#define POOL     16        /* row labels (> VIS + 2)       */
#define MAX_ENT  1024
#define NAME_LEN 200
#define PATH_LEN 1024

typedef struct { char name[NAME_LEN]; unsigned char dir; } ent_t;

static filepicker_t s_req;
static char    s_id[24], s_root[512], s_exts[96];
static char    s_dir[PATH_LEN];           /* folder listed, s_root or below it */
static ent_t   s_ent[MAX_ENT];
static int     s_n, s_sel, s_top, s_scan_gen;
static int     s_open, s_busy;
static double  s_t0;
static float   s_dt = 1.0f / 60.0f;
static spring  s_fade, s_pill, s_scroll, s_busy_a;

static label_t s_title, s_kick, s_path, s_count, s_empty, s_pname, s_pinfo, s_busy_l;
static label_t s_rl[POOL];
static int     s_slot_ent[POOL], s_slot_gen[POOL];
static char    s_path_txt[400];

/* preview of the highlighted entry */
static unsigned s_gen;                    /* bumps on every selection change */
static double   s_sel_t;
static int      s_pv_state;               /* 0 not asked, 1 decoding, 2 shown, -1 no preview */
static GLuint   s_pv_tex;
static int      s_pv_w, s_pv_h, s_pv_ow, s_pv_oh;
static long     s_pv_bytes = -1;

/* ------------------------------------------------------------------------ */
/* worker: one pending job (the newest wins), one finished result            */
enum { JOB_NONE, JOB_PREVIEW, JOB_ACCEPT };
static SDL_mutex  *s_mx;
static SDL_cond   *s_cv;
static int         s_worker;              /* thread running */
static struct { int kind, square; unsigned gen; char path[PATH_LEN];
                const char *(*accept)(const char *, void *); void *user; } s_job;
static struct { int kind; unsigned gen; SDL_Surface *surf; int ow, oh; const char *err; char path[PATH_LEN]; } s_res;

static int worker_main(void *arg)
{
   (void)arg;
   SDL_LockMutex(s_mx);
   for (;;)
   {
      static char path[PATH_LEN];
      int kind, square, ow = 0, oh = 0;
      unsigned gen;
      const char *(*accept)(const char *, void *);
      const char *err = NULL;
      void *user;
      SDL_Surface *surf = NULL;
      while (s_job.kind == JOB_NONE) SDL_CondWait(s_cv, s_mx);
      kind = s_job.kind; square = s_job.square; gen = s_job.gen; accept = s_job.accept; user = s_job.user;
      snprintf(path, sizeof(path), "%s", s_job.path);
      s_job.kind = JOB_NONE;
      SDL_UnlockMutex(s_mx);
      if (kind == JOB_PREVIEW) surf = image_load_scaled(path, PV_PX, square, &ow, &oh);
      else if (accept) err = accept(path, user);
      SDL_LockMutex(s_mx);
      if (s_res.surf) SDL_FreeSurface(s_res.surf);   /* a preview nobody collected */
      s_res.kind = kind; s_res.gen = gen; s_res.surf = surf; s_res.ow = ow; s_res.oh = oh; s_res.err = err;
      snprintf(s_res.path, sizeof(s_res.path), "%s", path);
   }
   return 0;
}

static void worker_start(void)
{
   SDL_Thread *th;
   if (s_worker) return;
   if (!s_mx) s_mx = SDL_CreateMutex();
   if (!s_cv) s_cv = SDL_CreateCond();
   if (!s_mx || !s_cv || !(th = SDL_CreateThread(worker_main, "filepicker", NULL)))
   { logf_("filepicker: no worker thread (%s): previews off", SDL_GetError()); return; }
   SDL_DetachThread(th);
   s_worker = 1;
}

static void job_post(int kind, const char *path)
{
   SDL_LockMutex(s_mx);
   s_job.kind = kind; s_job.gen = s_gen; s_job.square = s_req.preview == FP_PREVIEW_ROUND;
   s_job.accept = s_req.accept; s_job.user = s_req.user;
   snprintf(s_job.path, sizeof(s_job.path), "%s", path);
   SDL_CondSignal(s_cv);
   SDL_UnlockMutex(s_mx);
}

/* ------------------------------------------------------------------------ */
/* listing                                                                   */
static int hidden(const char *name)
{
   return name[0] == '.' || name[0] == '$' || !strcasecmp(name, "System Volume Information") ||
          !strncasecmp(name, "FOUND.", 6) || !strcmp(name, "lost+found");
}

static int ext_ok(const char *name)
{
   const char *dot = strrchr(name, '.'), *p = s_exts;
   size_t n;
   if (!s_exts[0]) return 1;
   if (!dot || !dot[1]) return 0;
   n = strlen(++dot);
   while (*p)
   {
      const char *e = strchr(p, ',');
      size_t len = e ? (size_t)(e - p) : strlen(p);
      if (len == n && !strncasecmp(p, dot, n)) return 1;
      p += len + (e ? 1 : 0);
   }
   return 0;
}

static int cmp_ent(const void *a, const void *b)
{
   const ent_t *x = a, *y = b;
   int c;
   if (x->dir != y->dir) return y->dir - x->dir;
   return (c = strcasecmp(x->name, y->name)) ? c : strcmp(x->name, y->name);
}

static int at_root(void) { return !strcmp(s_dir, s_root); }

static int join(char *out, size_t n, const char *dir, const char *name)
{
   return snprintf(out, n, "%s/%s", strcmp(dir, "/") ? dir : "", name) < (int)n;
}

/* "USB / photos / 2024": the path below the root, the root under its translated name */
static void path_text(void)
{
   const char *p = s_dir + strlen(s_root);
   size_t o = (size_t)snprintf(s_path_txt, sizeof(s_path_txt), "%s", tr("fp.root"));
   while (*p && o + 8 < sizeof(s_path_txt))
   {
      if (*p == '/') { o += (size_t)snprintf(s_path_txt + o, sizeof(s_path_txt) - o, "  /  "); p++; }
      else s_path_txt[o++] = *p++;
      s_path_txt[o] = 0;
   }
}

static void sel_changed(double t)
{
   s_gen++; s_sel_t = t; s_pv_state = 0; s_pv_bytes = -1; s_pv_ow = s_pv_oh = 0;
   if (s_pv_tex) { glDeleteTextures(1, &s_pv_tex); s_pv_tex = 0; }
   if (s_sel >= 0 && s_sel < s_n && !s_ent[s_sel].dir)
   {
      char path[PATH_LEN];
      struct stat st;
      if (join(path, sizeof(path), s_dir, s_ent[s_sel].name) && stat(path, &st) == 0) s_pv_bytes = (long)st.st_size;
   }
}

/* lists `dir` and selects `select` (a name) when it is there; -1 = cannot be read (the
 * current listing stays) */
static int scan(const char *dir, const char *select, double t)
{
   DIR *d = opendir(dir);
   struct dirent *e;
   int i;
   if (!d) { logf_("filepicker: cannot open %s", dir); return -1; }
   if (dir != s_dir) snprintf(s_dir, sizeof(s_dir), "%s", dir);
   s_n = 0;
   while ((e = readdir(d)) && s_n < MAX_ENT)
   {
      int is_dir;
      if (hidden(e->d_name) || strlen(e->d_name) >= NAME_LEN) continue;
      if (e->d_type == DT_DIR) is_dir = 1;
      else if (e->d_type == DT_REG) is_dir = 0;
      else
      {
         char path[PATH_LEN];
         struct stat st;
         if (!join(path, sizeof(path), s_dir, e->d_name) || stat(path, &st) != 0) continue;
         if (S_ISDIR(st.st_mode)) is_dir = 1;
         else if (S_ISREG(st.st_mode)) is_dir = 0;
         else continue;
      }
      if (!is_dir && !ext_ok(e->d_name)) continue;
      snprintf(s_ent[s_n].name, NAME_LEN, "%s", e->d_name);
      s_ent[s_n++].dir = (unsigned char)is_dir;
   }
   closedir(d);
   qsort(s_ent, (size_t)s_n, sizeof(ent_t), cmp_ent);
   s_sel = 0;
   for (i = 0; select && i < s_n; i++) if (!strcmp(s_ent[i].name, select)) { s_sel = i; break; }
   s_top = s_sel - VIS / 2;
   if (s_top > s_n - VIS) s_top = s_n - VIS;
   if (s_top < 0) s_top = 0;
   s_scroll.x = s_top * ROW; s_scroll.v = 0;
   s_pill.x = s_sel * ROW; s_pill.v = 0;
   s_scan_gen++;
   path_text();
   sel_changed(t);
   return 0;
}

static void go_up(double t)
{
   char parent[PATH_LEN], from[NAME_LEN];
   char *slash;
   if (at_root()) return;
   snprintf(parent, sizeof(parent), "%s", s_dir);
   if (!(slash = strrchr(parent, '/'))) return;
   snprintf(from, sizeof(from), "%s", slash + 1);
   *slash = 0;
   if (strlen(parent) < strlen(s_root)) snprintf(parent, sizeof(parent), "%s", s_root);
   if (scan(parent, from, t) != 0) panel_toast("fp.unreadable");
}

static void enter(double t)
{
   char path[PATH_LEN];
   if (!join(path, sizeof(path), s_dir, s_ent[s_sel].name) || scan(path, NULL, t) != 0) panel_toast("fp.unreadable");
}

/* ------------------------------------------------------------------------ */
/* open / close                                                              */
static int below_root(const char *dir)
{
   size_t n = strlen(s_root);
   return !strncmp(dir, s_root, n) && (dir[n] == 0 || dir[n] == '/' || !strcmp(s_root, "/")) && !strstr(dir, "/..");
}

void filepicker_open(const filepicker_t *req)
{
   char key[48];
   const char *last;
   size_t n;
   double t = now_s();
   if (s_open || !req) return;
   s_req = *req;
   snprintf(s_id, sizeof(s_id), "%s", req->id ? req->id : "file");
   snprintf(s_root, sizeof(s_root), "%s", req->root && req->root[0] ? req->root : g_root);
   for (n = strlen(s_root); n > 1 && s_root[n - 1] == '/'; ) s_root[--n] = 0;
   snprintf(s_exts, sizeof(s_exts), "%s", req->exts ? req->exts : "");
   worker_start();
   /* the folder used last time, when it still exists; else the top one */
   snprintf(key, sizeof(key), "picker.%s", s_id);
   last = settings_get(key, "");
   s_n = 0; s_sel = 0; s_dir[0] = 0;
   if (!last[0] || !below_root(last) || scan(last, NULL, t) != 0)
      if (scan(s_root, NULL, t) != 0) { snprintf(s_dir, sizeof(s_dir), "%s", s_root); s_scan_gen++; path_text(); sel_changed(t); }
   s_open = 1; s_busy = 0; s_t0 = t;
   if (s_fade.x < 0.02f) { s_fade.x = 0; s_fade.v = 0; }
   s_busy_a.x = s_busy_a.v = 0;
   logf_("filepicker: open %s (%s, %d entries)", s_id, s_dir, s_n);
}

static void close_picker(void) { s_open = 0; s_busy = 0; }

static void accepted(const char *path)
{
   char key[48];
   snprintf(key, sizeof(key), "picker.%s", s_id);
   settings_set(key, s_dir);
   logf_("filepicker: %s picked %s", s_id, path);
   close_picker();
   if (s_req.done) s_req.done(path, s_req.user);
}

static void pick(void)
{
   char path[PATH_LEN];
   if (!join(path, sizeof(path), s_dir, s_ent[s_sel].name)) { panel_toast("fp.unreadable"); return; }
   if (!s_req.accept) { accepted(path); return; }
   if (!s_worker)
   {  /* no worker thread: do it here (the frame waits) */
      const char *err = s_req.accept(path, s_req.user);
      if (err) panel_toast(err); else accepted(path);
      return;
   }
   s_busy = 1;
   job_post(JOB_ACCEPT, path);
}

int   filepicker_active(void) { return s_open || s_fade.x > 0.01f; }
int   filepicker_open_now(void) { return s_open; }
float filepicker_amount(void) { return clampf(s_fade.x, 0.0f, 1.0f); }

static void free_gl(void)
{
   int i;
   if (s_pv_tex) { glDeleteTextures(1, &s_pv_tex); s_pv_tex = 0; }
   label_free(&s_title); label_free(&s_kick); label_free(&s_path); label_free(&s_count); label_free(&s_empty);
   label_free(&s_pname); label_free(&s_pinfo); label_free(&s_busy_l);
   for (i = 0; i < POOL; i++) { label_free(&s_rl[i]); s_slot_ent[i] = -1; }
}

/* what the worker finished since the last frame */
static void collect(void)
{
   static char path[PATH_LEN];
   SDL_Surface *surf;
   const char *err;
   unsigned gen;
   int kind, ow, oh;
   if (!s_worker) return;
   SDL_LockMutex(s_mx);
   kind = s_res.kind; gen = s_res.gen; surf = s_res.surf; ow = s_res.ow; oh = s_res.oh; err = s_res.err;
   snprintf(path, sizeof(path), "%s", s_res.path);
   s_res.kind = JOB_NONE; s_res.surf = NULL;
   SDL_UnlockMutex(s_mx);
   if (kind == JOB_PREVIEW)
   {
      if (s_open && gen == s_gen && s_pv_state == 1)
      {
         if (s_pv_tex) glDeleteTextures(1, &s_pv_tex);
         s_pv_tex = surf ? tex_from_surface(surf) : 0;
         s_pv_state = s_pv_tex ? 2 : -1;
         if (surf) { s_pv_w = surf->w; s_pv_h = surf->h; s_pv_ow = ow; s_pv_oh = oh; }
      }
      if (surf) SDL_FreeSurface(surf);
   }
   else if (kind == JOB_ACCEPT && s_busy)
   {
      s_busy = 0;
      if (err) { panel_toast(err); logf_("filepicker: %s refused %s (%s)", s_id, path, err); }
      else accepted(path);
   }
}

void filepicker_update(const ui_nav_t *nav, double t, float dt)
{
   int before = s_sel;
   s_dt = dt > 0.0f ? dt : s_dt;
   spring_step(&s_fade, s_open ? 1.0f : 0.0f, 120, 18, dt);
   spring_step(&s_busy_a, s_busy ? 1.0f : 0.0f, 120, 18, dt);
   collect();
   if (!s_open)
   {
      if (s_fade.x <= 0.01f && (s_title.tex || s_pv_tex)) free_gl();
      return;
   }
   if (nav && !s_busy && t - s_t0 >= 0.15)   /* not the Cross that opened it */
   {
      if (nav->options) { close_picker(); return; }
      if (nav->back || nav->left)
      {
         if (!at_root()) go_up(t);
         else if (nav->back) { close_picker(); return; }
      }
      else if (s_n)
      {
         if (nav->up && s_sel > 0) s_sel--;
         if (nav->down && s_sel < s_n - 1) s_sel++;
         if (nav->l1) s_sel = s_sel - VIS < 0 ? 0 : s_sel - VIS;
         if (nav->r1) s_sel = s_sel + VIS > s_n - 1 ? s_n - 1 : s_sel + VIS;
         if (s_sel != before) sel_changed(t);
         else if (nav->confirm || nav->right)
         {
            if (s_ent[s_sel].dir) enter(t);
            else if (nav->confirm) pick();
         }
      }
   }
   /* keep the selection inside the visible rows */
   if (s_sel < s_top) s_top = s_sel;
   if (s_sel > s_top + VIS - 1) s_top = s_sel - VIS + 1;
   spring_step(&s_scroll, s_top * ROW, 240, 26, dt);
   spring_step(&s_pill, s_sel * ROW, 260, 26, dt);
   /* the preview is asked once the selection rests for a moment (not while scrolling) */
   if (s_worker && s_req.preview != FP_PREVIEW_NONE && s_pv_state == 0 && s_n && !s_ent[s_sel].dir && !s_busy &&
       t - s_sel_t > 0.15)
   {
      char path[PATH_LEN];
      if (s_pv_bytes > IMAGE_MAX_BYTES || !join(path, sizeof(path), s_dir, s_ent[s_sel].name)) s_pv_state = -1;
      else { s_pv_state = 1; job_post(JOB_PREVIEW, path); }
   }
}

/* ------------------------------------------------------------------------ */
/* drawing                                                                   */
static void col4(float o[4], int tc, float a) { theme_color(tc, o); o[3] = a; }

static void draw_label(const label_t *l, float x, float y, const float c[4])
{
   if (l->tex && c[3] > 0.003f) draw_2d(l->tex, floorf(x), floorf(y), l->w, l->h, c, 0);
}

/* s, or s cut with an ellipsis so it fits max_w logical px in font f */
static const char *fit(TTF_Font *f, const char *s, float max_w, char *buf, size_t n)
{
   int w, h;
   size_t len;
   if (!f || TTF_SizeUTF8(f, s, &w, &h) != 0 || w <= max_w * g_ui_scale) return s;
   snprintf(buf, n - 4, "%s", s);
   len = strlen(buf);
   while (len > 0)
   {
      do len--; while (len > 0 && ((unsigned char)buf[len] & 0xC0) == 0x80);
      memcpy(buf + len, "\xE2\x80\xA6", 4);
      if (TTF_SizeUTF8(f, buf, &w, &h) != 0 || w <= max_w * g_ui_scale) break;
   }
   return buf;
}

/* a path too long for its line keeps its end: "…  /  photos  /  2024" */
static const char *fit_path(TTF_Font *f, const char *s, float max_w, char *buf, size_t n)
{
   int w, h;
   const char *p = s;
   if (!f || TTF_SizeUTF8(f, s, &w, &h) != 0 || w <= max_w * g_ui_scale) return s;
   while ((p = strstr(p + 1, "  /  ")))
   {
      snprintf(buf, n, "\xE2\x80\xA6%s", p);
      if (TTF_SizeUTF8(f, buf, &w, &h) != 0 || w <= max_w * g_ui_scale) return buf;
   }
   return fit(f, s, max_w, buf, n);
}

static void draw_folder(float x, float y, float k, const float c[4])
{
   draw_round_rect(x, y, 10 * k, 7 * k, 2.5f * k, c, 0.0f);
   draw_round_rect(x, y + 3 * k, 22 * k, 15 * k, 3 * k, c, 0.0f);
}

static void draw_file(float x, float y, float k, const float c[4])
{
   const float in[4] = { c[0], c[1], c[2], c[3] * 0.28f };
   draw_round_rect(x + 2 * k, y, 18 * k, 18 * k, 4 * k, in, 0.55f * c[3]);
}

/* three dots, one after another: something is being worked on */
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

static void size_text(char *out, size_t n, long bytes)
{
   if (bytes < 0) { out[0] = 0; return; }
   if (bytes < 1024 * 1024) snprintf(out, n, "%ld KB", (bytes + 1023) / 1024);
   else
   {
      long tenths = (long)((double)bytes / (1024.0 * 1024.0) * 10.0 + 0.5);
      snprintf(out, n, "%ld%s%ld MB", tenths / 10, tr("num.decimal"), tenths % 10);
   }
}

static void draw_preview(float px, float py, float pw, double t, float a)
{
   const ent_t *e = s_n ? &s_ent[s_sel] : NULL;
   float txt[4], dim[4], acc[4], box[4];
   float bx = px + (pw - PV) * 0.5f, by = py, r = s_req.preview == FP_PREVIEW_ROUND ? PV * 0.5f : 14.0f;
   char buf[NAME_LEN + 8], info[96], sz[32];
   col4(txt, TC_TEXT, a); col4(dim, TC_DIM, a); col4(acc, TC_ACCENT, a);
   col4(box, TC_TEXT, 0.06f * a);
   if (!e) return;
   info[0] = 0;
   if (e->dir)
   {
      draw_round_rect(bx, by, PV, PV, 14, box, 0.06f * a);
      draw_folder(bx + PV * 0.5f - 44, by + PV * 0.5f - 36, 4.0f, txt);
      snprintf(info, sizeof(info), "%s", tr("fp.folder"));
   }
   else
   {
      size_text(sz, sizeof(sz), s_pv_bytes);
      if (s_pv_state == 2 && s_pv_tex)
      {
         const float wh[4] = { 1, 1, 1, a };
         float w = PV, h = PV;
         if (s_req.preview != FP_PREVIEW_ROUND && s_pv_w > 0 && s_pv_h > 0)
         {  /* the whole image, centred in the box */
            if (s_pv_w >= s_pv_h) h = PV * s_pv_h / (float)s_pv_w; else w = PV * s_pv_w / (float)s_pv_h;
         }
         ui_shadow(bx + (PV - w) * 0.5f, by + (PV - h) * 0.5f, w, h, r < h * 0.5f ? r : h * 0.5f, 18, 6, 0.45f * a);
         draw_image(s_pv_tex, s_pv_w, s_pv_h, bx + (PV - w) * 0.5f, by + (PV - h) * 0.5f, w, h, r < h * 0.5f ? r : h * 0.5f, wh);
         gl_use_2d();
         if (s_pv_ow > 0) snprintf(info, sizeof(info), "%d \xC3\x97 %d  \xC2\xB7  %s", s_pv_ow, s_pv_oh, sz);
      }
      else
      {
         draw_round_rect(bx, by, PV, PV, r, box, 0.06f * a);
         if (s_req.preview == FP_PREVIEW_NONE) draw_file(bx + PV * 0.5f - 44, by + PV * 0.5f - 36, 4.0f, dim);
         else if (s_pv_state == -1) snprintf(info, sizeof(info), "%s", tr("fp.no_preview"));
         else draw_dots(bx + PV * 0.5f, by + PV * 0.5f, t, dim);
      }
      if (!info[0]) snprintf(info, sizeof(info), "%s", sz);
   }
   label_set(&s_pname, g_font_sub, fit(g_font_sub, e->name, pw, buf, sizeof(buf)));
   label_set(&s_pinfo, g_font_small, info);
   draw_label(&s_pname, px + (pw - s_pname.w) * 0.5f, by + PV + 22, txt);
   draw_label(&s_pinfo, px + (pw - s_pinfo.w) * 0.5f, by + PV + 54, dim);
}

void filepicker_draw(double t)
{
   /* lumen-design, file-picker.html: one glass card; the kicker asks, the title is the
    * folder we are in, its path sits at the right */
   float a = filepicker_amount(), cx = (W - CW) * 0.5f, cy = 112.0f + (1 - a) * 16;
   float lx = cx + LIST_X, ly = cy + LIST_Y, lh = VIS * ROW;
   float txt[4], dim[4], acc[4], sep[4];
   char buf[420], num1[16], num2[16];
   const char *here;
   int i;
   if (a <= 0.003f) return;
   col4(txt, TC_TEXT, a); col4(dim, TC_DIM, a); col4(acc, TC_ACCENT, a); col4(sep, TC_TEXT, 0.08f * a);

   gl_use_2d();
   ui_scrim(a * (1.0f - panel_amount()));   /* over the settings the scrim is already there */
   ui_card(cx, cy, CW, CH, a);

   here = at_root() ? tr("fp.root") : strrchr(s_dir, '/') + 1;
   ui_kicker(&s_kick, tr(s_req.title_key ? s_req.title_key : "fp.title"), cx + PADX, cy + 24, a);
   label_set(&s_title, g_font_title, ui_fit(g_font_title, here, CW * 0.5f, buf, sizeof(buf)));
   draw_label(&s_title, cx + PADX, cy + 42, txt);
   label_set(&s_path, g_font_small, fit_path(g_font_small, s_path_txt, CW * 0.5f - 2 * PADX, buf, sizeof(buf)));
   draw_label(&s_path, cx + CW - PADX - s_path.w, cy + 58, dim);
   draw_2d(0, cx + PADX, cy + 96, CW - 2 * PADX, 1, sep, 0);

   if (!s_n)
   {
      const char *msg = tr(s_req.empty_key ? s_req.empty_key : "fp.empty");
      if (strcmp(s_empty.key, msg) || !s_empty.tex)
      {
         label_free(&s_empty);
         snprintf(s_empty.key, sizeof(s_empty.key), "%s", msg);
         s_empty.tex = text_tex_wrapped(g_font_sub, msg, (int)(CW - 2 * PADX - 160), &s_empty.w, &s_empty.h);
      }
      draw_folder(cx + CW * 0.5f - 33, ly + lh * 0.5f - 86, 3.0f, sep);
      draw_label(&s_empty, cx + (CW - s_empty.w) * 0.5f, ly + lh * 0.5f - 8, dim);
   }
   else
   {
      /* rows, clipped to the list */
      glEnable(GL_SCISSOR_TEST);
      gl_scissor(lx - 8, ly - 8, LIST_W + 16, lh + 16);   /* room for the ring */
      ui_focus(lx, ly + s_pill.x - s_scroll.x + 7, LIST_W, ROW - 14, UI_CTL_R, a);
      for (i = (int)(s_scroll.x / ROW) - 1; i < s_n; i++)
      {
         const ent_t *e;
         float y, c[4];
         int slot;
         if (i < 0) continue;
         y = ly + i * ROW - s_scroll.x;
         if (y >= ly + lh - 1) break;
         if (y + ROW <= ly + 1) continue;
         e = &s_ent[i];
         slot = i % POOL;
         if (s_slot_ent[slot] != i || s_slot_gen[slot] != s_scan_gen)
         {
            label_set(&s_rl[slot], g_font_sub, fit(g_font_sub, e->name, LIST_W - 86, buf, sizeof(buf)));
            s_slot_ent[slot] = i; s_slot_gen[slot] = s_scan_gen;
         }
         memcpy(c, txt, sizeof(c));
         if (i != s_sel) c[3] *= 0.86f;
         if (e->dir) draw_folder(lx + 22, y + (ROW - 18) * 0.5f, 1.0f, txt);
         else draw_file(lx + 22, y + (ROW - 18) * 0.5f, 1.0f, i == s_sel ? txt : dim);
         draw_label(&s_rl[slot], lx + 58, y + (ROW - s_rl[slot].h) * 0.5f, c);
         if (e->dir)
         {  /* chevron: it opens */
            float cv[4] = { c[0], c[1], c[2], c[3] * 0.6f }, ccx = lx + LIST_W - 30, ccy = y + ROW * 0.5f;
            int k;
            for (k = 0; k < 4; k++)
            {
               draw_2d(0, ccx + k * 2, ccy - 7 + k * 2, 2.5f, 2.5f, cv, 0);
               draw_2d(0, ccx + k * 2, ccy + 7 - k * 2 - 2.5f, 2.5f, 2.5f, cv, 0);
            }
         }
      }
      glDisable(GL_SCISSOR_TEST);
      if (s_n > VIS)
      {  /* scroll thumb */
         float th = lh * VIS / (float)s_n, ty = ly + (lh - th) * clampf(s_scroll.x / ((s_n - VIS) * ROW), 0.0f, 1.0f);
         float tc[4]; col4(tc, TC_TEXT, 0.22f * a);
         draw_round_rect(lx + LIST_W + 6, ty, 4, th, 2, tc, 0.0f);
      }
      /* vertical rule, then the preview of the highlighted entry */
      draw_2d(0, lx + LIST_W + 22, ly, 1, lh, sep, 0);
      draw_preview(lx + LIST_W + 38, ly + 20, CW - LIST_X - LIST_W - 38 - PADX, t, a);
      gl_use_2d();
      snprintf(num1, sizeof(num1), "%d", s_sel + 1);
      snprintf(num2, sizeof(num2), "%d", s_n);
      label_set(&s_count, g_font_small, trf(buf, sizeof(buf), "fp.count", "i", num1, "n", num2, NULL));
      draw_label(&s_count, cx + PADX, cy + CH - 14 - s_count.h, dim);
   }

   if (s_busy_a.x > 0.01f)
   {  /* accept() is running on the worker */
      float ba = clampf(s_busy_a.x, 0.0f, 1.0f) * a, veil[4], bt[4];
      col4(veil, TC_CARD, 0.82f * ba); col4(bt, TC_TEXT, ba);
      draw_round_rect(cx, cy, CW, CH, UI_CARD_R, veil, 0.0f);
      label_set(&s_busy_l, g_font_sub, tr(s_req.busy_key ? s_req.busy_key : "fp.working"));
      draw_label(&s_busy_l, cx + (CW - s_busy_l.w) * 0.5f, cy + CH * 0.5f - 34, bt);
      draw_dots(cx + CW * 0.5f, cy + CH * 0.5f + 18, t, bt);
   }
}
