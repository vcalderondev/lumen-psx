/* Library check, see audit.h.
 *
 * The worker reads the system declarations itself (not g_plat: a system whose games are
 * all broken is not loaded, and that is the one to report) and walks their folders with
 * the same rules as the scanner in main.c:
 *   psc    <path>/<n>/Game.ini, Discs=<disc>[,<disc>...], <disc>.<ext>
 *   files  <path>/<name>.<ext>
 *   apps   <path>/<name>/app.ini, Exec=<program>
 * The built-in games (layout = stock) are the console's own and are not checked. */
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include "lumen.h"
#include "audit.h"
#include "i18n.h"
#include "music.h"
#include "panel.h"
#include "theme.h"
#include "ui.h"

/* geometry: the file picker's card (lumen-design, library-audit.html) */
#define CW       940.0f
#define CH       520.0f
#define PADX     28.0f
#define LIST_X   14.0f
#define LIST_Y   110.0f
#define LIST_W   560.0f
#define ROW      46.0f
#define VIS      8
#define POOL     16
#define MAX_ISS  400
#define MAX_LIST 600
#define NAME_LEN 256
#define PATH_LEN 1024

enum { K_NOINI, K_NODISCS, K_NODISC, K_CUEREF, K_CUECASE, K_CUEBAD, K_EMPTY, K_NOCUE, K_M3UREF, K_SBI, K_SBINAME,
       K_EMPTYDIR, K_NOCORE, K_APPINI, K_APPEXEC, K_PART, K_COUNT };
static const char *const k_key[K_COUNT] = {
   "audit.noini", "audit.nodiscs", "audit.nodisc", "audit.cueref", "audit.cuecase", "audit.cuebad", "audit.empty",
   "audit.nocue", "audit.m3uref", "audit.sbi", "audit.sbiname", "audit.emptydir", "audit.nocore", "audit.appini",
   "audit.appexec", "audit.part"
};

typedef struct { int kind, sysi; char sys[48], name[128], file[200], dir[600]; } issue_t;
typedef struct { char id[32], name[48], layout[8], paths[512], exts[128], core[128]; int order; } asys_t;

/* ---- worker state: written by the worker while s_running, read by the UI after ---- */
static issue_t s_iss[MAX_ISS];
static int     s_niss, s_nsys;
static volatile int s_running, s_games;
static int     s_more;                   /* more problems than MAX_ISS */

static asys_t  w_sys[MAX_PLATFORMS];
static char    w_list[MAX_LIST][NAME_LEN];   /* the folder being checked */
static int     w_nlist;
static char    w_ref[MAX_LIST][NAME_LEN];    /* files a .cue of this folder points to */
static int     w_nref;
static char    w_prot[512][20];              /* <id>.protected: discs that need an .sbi */
static int     w_nprot;

static void issue(int kind, const asys_t *sy, const char *name, const char *file, const char *dir)
{
   issue_t *i;
   if (s_niss >= MAX_ISS) { s_more = 1; return; }
   i = &s_iss[s_niss];
   i->kind = kind; i->sysi = (int)(sy - w_sys);
   snprintf(i->sys, sizeof(i->sys), "%s", sy->name);
   snprintf(i->name, sizeof(i->name), "%s", name);
   snprintf(i->file, sizeof(i->file), "%s", file ? file : "");
   snprintf(i->dir, sizeof(i->dir), "%s", dir);
   s_niss++;   /* last: the UI may be counting */
}

static char *trim(char *s)
{
   char *e;
   while (*s == ' ' || *s == '\t') s++;
   for (e = s + strlen(s); e > s && strchr(" \t\r\n", e[-1]); ) e--;
   *e = 0;
   return s;
}

static const char *ext_of(const char *name) { const char *d = strrchr(name, '.'); return d ? d + 1 : ""; }

static int ext_in(const char *name, const char *exts)
{
   const char *x = ext_of(name), *p = exts;
   size_t n = strlen(x);
   if (!n) return 0;
   while (*p)
   {
      const char *e = strchr(p, ',');
      size_t len = e ? (size_t)(e - p) : strlen(p);
      while (len && *p == ' ') { p++; len--; }
      while (len && p[len - 1] == ' ') len--;
      if (len == n && !strncasecmp(p, x, n)) return 1;
      if (!e) break;
      p = e + 1;
   }
   return 0;
}

static int list_dir(const char *dir)
{
   DIR *d = opendir(dir);
   struct dirent *e;
   w_nlist = 0;
   if (!d) return -1;
   while ((e = readdir(d)) && w_nlist < MAX_LIST)
   {
      if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || !strncmp(e->d_name, "._", 2) ||
          !strcmp(e->d_name, ".DS_Store") || strlen(e->d_name) >= NAME_LEN) continue;
      snprintf(w_list[w_nlist++], NAME_LEN, "%s", e->d_name);
   }
   closedir(d);
   return w_nlist;
}

/* index of name in the listed folder: exact, else (ci) whatever the case; -1 none */
static int list_find(const char *name, int ci)
{
   int i;
   for (i = 0; i < w_nlist; i++) if (!strcmp(w_list[i], name)) return i;
   for (i = 0; ci && i < w_nlist; i++) if (!strcasecmp(w_list[i], name)) return i;
   return -1;
}

static long long file_size(const char *dir, const char *name)
{
   char p[PATH_LEN];
   struct stat st;
   snprintf(p, sizeof(p), "%s/%s", dir, name);
   return stat(p, &st) == 0 && S_ISREG(st.st_mode) ? (long long)st.st_size : -1;
}

static int is_dir(const char *dir, const char *name)
{
   char p[PATH_LEN];
   struct stat st;
   snprintf(p, sizeof(p), "%s/%s", dir, name);
   return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static size_t read_text(const char *dir, const char *name, char *buf, size_t n)
{
   char p[PATH_LEN];
   FILE *f;
   size_t r;
   snprintf(p, sizeof(p), "%s/%s", dir, name);
   buf[0] = '\n'; buf[1] = 0;
   if (!(f = fopen(p, "rb"))) return 0;
   r = fread(buf + 1, 1, n - 2, f);
   fclose(f);
   buf[r + 1] = 0;
   return r;
}

/* "\nKey=value" of a Game.ini / app.ini */
static void ini_val(const char *ini, const char *key, char *out, size_t n)
{
   char pat[64], *t;
   const char *p, *e;
   size_t len;
   out[0] = 0;
   snprintf(pat, sizeof(pat), "\n%s=", key);
   if (!(p = strstr(ini, pat))) return;
   p += strlen(pat);
   e = strpbrk(p, "\r\n");
   len = e ? (size_t)(e - p) : strlen(p);
   if (len >= n) len = n - 1;
   memcpy(out, p, len); out[len] = 0;
   t = trim(out);
   if (t != out) memmove(out, t, strlen(t) + 1);
}

/* a .cue: every FILE "x" must be in the folder, with that exact name and some bytes */
static void check_cue(const asys_t *sy, const char *label, const char *dir, const char *cue)
{
   static char buf[32 * 1024];
   char *line, *save = NULL;
   int files = 0;
   read_text(dir, cue, buf, sizeof(buf));
   for (line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save))
   {
      char *p = trim(line), *a, *b, ref[NAME_LEN];
      long long sz;
      if (strncasecmp(p, "FILE", 4) || (p[4] != ' ' && p[4] != '\t')) continue;
      p = trim(p + 4);
      if (*p == '"') { a = p + 1; b = strrchr(a, '"'); }
      else { a = p; b = strrchr(a, ' '); }   /* FILE name.bin BINARY */
      if (!b || b <= a) continue;
      snprintf(ref, sizeof(ref), "%.*s", (int)(b - a), a);
      files++;
      if (w_nref < MAX_LIST) snprintf(w_ref[w_nref++], NAME_LEN, "%s", ref);
      sz = file_size(dir, ref);
      if (sz > 0) continue;
      if (sz == 0) issue(K_EMPTY, sy, label, ref, dir);
      /* a name that only differs in case works on FAT and fails on ext4: the filesystem decides */
      else issue(!strpbrk(ref, "/\\") && list_find(ref, 1) >= 0 ? K_CUECASE : K_CUEREF, sy, label, ref, dir);
   }
   if (!files) issue(K_CUEBAD, sy, label, cue, dir);
}

/* a file a game is launched from */
static void check_content(const asys_t *sy, const char *label, const char *dir, const char *name)
{
   long long sz = file_size(dir, name);
   const char *x = ext_of(name);
   if (sz == 0) { issue(K_EMPTY, sy, label, name, dir); return; }
   if (!strcasecmp(x, "cue")) check_cue(sy, label, dir, name);
   else if (!strcasecmp(x, "m3u"))
   {
      static char buf[8 * 1024];
      char *line, *save = NULL;
      read_text(dir, name, buf, sizeof(buf));
      for (line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save))
      {
         char *p = trim(line), full[PATH_LEN];
         struct stat st;
         if (!*p || *p == '#') continue;
         if (*p == '/') snprintf(full, sizeof(full), "%s", p); else snprintf(full, sizeof(full), "%s/%s", dir, p);
         if (w_nref < MAX_LIST) snprintf(w_ref[w_nref++], NAME_LEN, "%s", p);
         if (stat(full, &st) != 0) issue(K_M3UREF, sy, label, p, dir);
      }
   }
}

/* "SLES_012.34", "sles-01234" -> "SLES01234" */
static void serial_norm(char *out, size_t n, const char *s)
{
   size_t o = 0;
   for (; *s && o + 1 < n; s++) if (isalnum((unsigned char)*s)) out[o++] = (char)toupper((unsigned char)*s);
   out[o] = 0;
}

static int is_protected(const char *serial)
{
   char k[20];
   int i;
   serial_norm(k, sizeof(k), serial);
   for (i = 0; k[0] && i < w_nprot; i++) if (!strcmp(w_prot[i], k)) return 1;
   return 0;
}

/* The serial a PlayStation disc image carries: the BOOT line of SYSTEM.CNF
 * ("BOOT = cdrom:\\SLES_012.34;1"), found through the ISO 9660 root folder. Works on raw
 * images (2352-byte sectors, .bin / .img) and plain ones (2048, .iso); a compressed image
 * (.chd, .pbp) cannot be read this way. out = "" when it is not found. */
static void image_serial(const char *path, char *out, size_t n)
{
   static const unsigned char sync[12] = { 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0 };
   unsigned char sec[2048], head[16];
   FILE *f = fopen(path, "rb");
   long long ssz = 2048, off = 0;
   unsigned long lba, len, pos, cnf = 0;
   int k;
   out[0] = 0;
   if (!f) return;
   if (fread(head, 1, 16, f) == 16 && !memcmp(head, sync, 12)) { ssz = 2352; off = head[15] == 2 ? 24 : 16; }
#define RD(l) (fseeko(f, (off_t)((long long)(l) * ssz + off), SEEK_SET) == 0 && fread(sec, 1, 2048, f) == 2048)
   if (!RD(16) || memcmp(sec + 1, "CD001", 5)) { fclose(f); return; }
   lba = sec[158] | sec[159] << 8 | sec[160] << 16 | (unsigned long)sec[161] << 24;
   len = sec[166] | sec[167] << 8 | sec[168] << 16 | (unsigned long)sec[169] << 24;
   for (k = 0; k < 8 && (unsigned long)k * 2048 < len && !cnf; k++)
   {
      if (!RD(lba + (unsigned long)k)) break;
      for (pos = 0; pos + 34 < 2048 && sec[pos]; pos += sec[pos])
         if (sec[pos + 32] >= 10 && !strncasecmp((const char *)sec + pos + 33, "SYSTEM.CNF", 10))
         { cnf = sec[pos + 2] | sec[pos + 3] << 8 | sec[pos + 4] << 16 | (unsigned long)sec[pos + 5] << 24; break; }
   }
   if (cnf && RD(cnf))
   {
      char *b, *e;
      sec[2047] = 0;
      if ((b = strcasestr((char *)sec, "cdrom")))
      {
         b += 5;
         while (*b == ':' || *b == '\\' || *b == '/' || (*b >= '0' && *b <= '9' && b[1] == ':')) b++;
         for (e = b; *e && *e != ';' && *e != '\r' && *e != '\n' && *e != ' '; ) e++;
         *e = 0;
         if ((e = strrchr(b, '\\'))) b = e + 1;   /* cdrom:\DIR\SLES_012.34 */
         serial_norm(out, n, b);
      }
   }
#undef RD
   fclose(f);
}

/* a protected disc (in <id>.protected) needs <stem>.sbi next to it; stem = the disc's
 * name without extension, which is what the core looks for */
static void check_sbi(const asys_t *sy, const char *label, const char *dir, const char *content)
{
   static char cue[8 * 1024];
   char stem[NAME_LEN], name[NAME_LEN + 8], path[PATH_LEN], serial[20] = "", *dot, *p, *a, *b;
   const char *x = ext_of(content);
   if (!w_nprot) return;
   snprintf(stem, sizeof(stem), "%s", content);
   if ((dot = strrchr(stem, '.'))) *dot = 0;
   path[0] = 0;
   if (!strcasecmp(x, "cue"))
   {  /* the image is the first FILE of the .cue */
      read_text(dir, content, cue, sizeof(cue));
      if ((p = strcasestr(cue, "\nFILE")) && (a = strchr(p, '"')) && (b = strchr(a + 1, '"')) && b - a < NAME_LEN)
         snprintf(path, sizeof(path), "%s/%.*s", dir, (int)(b - a - 1), a + 1);
   }
   else if (!strcasecmp(x, "bin") || !strcasecmp(x, "img") || !strcasecmp(x, "iso")) snprintf(path, sizeof(path), "%s/%s", dir, content);
   if (path[0]) image_serial(path, serial, sizeof(serial));
   /* no serial in the image (compressed, or the image is missing): the name may say it */
   if (!is_protected(serial) && !is_protected(stem)) return;
   snprintf(name, sizeof(name), "%s.sbi", stem);
   if (list_find(name, 1) < 0) issue(K_SBI, sy, label, name, dir);
}

static void leftovers(const asys_t *sy, const char *label, const char *dir)
{
   int i;
   for (i = 0; i < w_nlist; i++)
   {
      size_t n = strlen(w_list[i]);
      if (n > 11 && !strcmp(w_list[i] + n - 11, ".lumen-part")) issue(K_PART, sy, label, w_list[i], dir);
   }
}

static void audit_psc(const asys_t *sy, const char *dir)
{
   static char ini[4096];
   const char *exts = sy->exts[0] ? sy->exts : "cue,pbp";
   DIR *d = opendir(dir);
   struct dirent *e;
   if (!d) return;
   while ((e = readdir(d)))
   {
      char gdir[PATH_LEN], title[128], discs[512], *tok, *save = NULL, first_ext[16];
      char stems[16][64];
      int nst = 0, i, k;
      if (e->d_name[0] == '.' || !is_dir(dir, e->d_name)) continue;
      if (!strcmp(e->d_name, "lost+found") || !strcasecmp(e->d_name, "System Volume Information")) continue;
      snprintf(gdir, sizeof(gdir), "%s/%s", dir, e->d_name);
      s_games++;
      if (list_dir(gdir) <= 0) { issue(K_EMPTYDIR, sy, e->d_name, NULL, gdir); continue; }
      w_nref = 0;
      leftovers(sy, e->d_name, gdir);
      if ((i = list_find("Game.ini", 1)) < 0) { issue(K_NOINI, sy, e->d_name, "Game.ini", gdir); continue; }
      read_text(gdir, w_list[i], ini, sizeof(ini));
      ini_val(ini, "Title", title, sizeof(title));
      if (!title[0]) snprintf(title, sizeof(title), "%s", e->d_name);
      ini_val(ini, "Discs", discs, sizeof(discs));
      if (!discs[0]) { issue(K_NODISCS, sy, title, "Game.ini", gdir); continue; }
      snprintf(first_ext, sizeof(first_ext), "%.*s", (int)strcspn(exts, ", "), exts);
      for (tok = strtok_r(discs, ",", &save); tok; tok = strtok_r(NULL, ",", &save))
      {
         char name[NAME_LEN], x[160], *xt, *xs = NULL;
         int found = -1;
         tok = trim(tok);
         if (!*tok) continue;
         if (nst < 16) snprintf(stems[nst++], sizeof(stems[0]), "%s", tok);
         snprintf(x, sizeof(x), "%s", exts);
         for (xt = strtok_r(x, ",", &xs); xt && found < 0; xt = strtok_r(NULL, ",", &xs))
         {
            snprintf(name, sizeof(name), "%s.%s", tok, trim(xt));
            found = list_find(name, 1);
         }
         if (found < 0) { snprintf(name, sizeof(name), "%s.%s", tok, first_ext); issue(K_NODISC, sy, title, name, gdir); continue; }
         check_content(sy, title, gdir, w_list[found]);
         check_sbi(sy, title, gdir, w_list[found]);
      }
      for (i = 0; i < w_nlist; i++)
      {  /* an .sbi the core will not find: it must be named like its disc */
         size_t n = strlen(w_list[i]);
         if (n < 5 || strcasecmp(w_list[i] + n - 4, ".sbi")) continue;
         for (k = 0; k < nst; k++) if (strlen(stems[k]) == n - 4 && !strncasecmp(stems[k], w_list[i], n - 4)) break;
         if (k == nst) issue(K_SBINAME, sy, title, w_list[i], gdir);
      }
   }
   closedir(d);
}

static void audit_files(const asys_t *sy, const char *dir)
{
   static char names[MAX_LIST][NAME_LEN];
   int n, i, k, cues = ext_in("x.cue", sy->exts);
   if (list_dir(dir) <= 0) return;
   n = w_nlist; w_nref = 0;
   memcpy(names, w_list, sizeof(names[0]) * (size_t)n);
   leftovers(sy, sy->name, dir);
   for (i = 0; i < n; i++)
   {
      if (names[i][0] == '.' || !ext_in(names[i], sy->exts) || is_dir(dir, names[i])) continue;
      s_games++;
      check_content(sy, names[i], dir, names[i]);
      check_sbi(sy, names[i], dir, names[i]);
   }
   for (i = 0; cues && i < n; i++)
   {  /* a disc image nobody points to: its .cue is missing, so the game is not listed */
      const char *x = ext_of(names[i]);
      if (strcasecmp(x, "bin") && strcasecmp(x, "img")) continue;
      if (ext_in(names[i], sy->exts)) continue;
      for (k = 0; k < w_nref; k++) if (!strcasecmp(w_ref[k], names[i])) break;
      if (k == w_nref) issue(K_NOCUE, sy, names[i], names[i], dir);
   }
}

static void audit_apps(const asys_t *sy, const char *dir)
{
   static char ini[4096];
   DIR *d = opendir(dir);
   struct dirent *e;
   if (!d) return;
   while ((e = readdir(d)))
   {
      char gdir[PATH_LEN], exe[256], title[128];
      int i;
      if (e->d_name[0] == '.' || !is_dir(dir, e->d_name)) continue;
      snprintf(gdir, sizeof(gdir), "%s/%s", dir, e->d_name);
      s_games++;
      if (list_dir(gdir) <= 0) { issue(K_EMPTYDIR, sy, e->d_name, NULL, gdir); continue; }
      if ((i = list_find("app.ini", 0)) < 0) { issue(K_APPINI, sy, e->d_name, "app.ini", gdir); continue; }
      read_text(gdir, w_list[i], ini, sizeof(ini));
      ini_val(ini, "Title", title, sizeof(title));
      if (!title[0]) snprintf(title, sizeof(title), "%s", e->d_name);
      ini_val(ini, "Exec", exe, sizeof(exe));
      if (!exe[0] || strstr(exe, "..") || exe[0] == '/' || file_size(gdir, exe) <= 0)
         issue(K_APPEXEC, sy, title, exe[0] ? exe : "Exec=", gdir);
   }
   closedir(d);
}

/* by system, then by folder in natural order ("9" before "10"), then by kind */
static int cmp_issue(const void *a, const void *b)
{
   const issue_t *x = a, *y = b;
   int c;
   if (x->sysi != y->sysi) return x->sysi - y->sysi;
   if ((x->kind == K_NOCORE) != (y->kind == K_NOCORE)) return (y->kind == K_NOCORE) - (x->kind == K_NOCORE);
   if ((c = strverscmp(x->dir, y->dir))) return c;
   return x->kind != y->kind ? x->kind - y->kind : strcmp(x->file, y->file);
}

static int cmp_sys(const void *a, const void *b)
{
   const asys_t *x = a, *y = b;
   return x->order != y->order ? x->order - y->order : strcmp(x->id, y->id);
}

/* <home>/systems/<id>.ini: [system] id name order, [library] layout paths extensions, [launch] core */
static int systems_load(void)
{
   char dir[400], path[700], line[700], sec[32], id[32];
   DIR *d;
   struct dirent *e;
   int n = 0;
   snprintf(dir, sizeof(dir), "%s/systems", g_home);
   if (!(d = opendir(dir))) return 0;
   while ((e = readdir(d)) && n < MAX_PLATFORMS)
   {
      asys_t *sy = &w_sys[n];
      size_t l = strlen(e->d_name);
      FILE *f;
      if (l < 5 || strcmp(e->d_name + l - 4, ".ini") || e->d_name[0] == '.') continue;
      snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
      if (!(f = fopen(path, "r"))) continue;
      memset(sy, 0, sizeof(*sy));
      sy->order = 1000; sec[0] = 0;
      while (fgets(line, sizeof(line), f))
      {
         char *k = trim(line), *v, *c;
         if (*k == '[') { if ((c = strchr(k, ']'))) { *c = 0; snprintf(sec, sizeof(sec), "%s", k + 1); } continue; }
         if (*k == ';' || *k == '#' || !(v = strchr(k, '='))) continue;
         *v++ = 0; k = trim(k);
         /* "; comment" ends a value; in paths ';' also separates folders, so only " ;" does */
         if (!strcmp(k, "paths")) { if ((c = strstr(v, " ;"))) *c = 0; }
         else if ((c = strchr(v, ';'))) *c = 0;
         v = trim(v);
         if (!strcmp(sec, "system") && !strcmp(k, "id")) snprintf(sy->id, sizeof(sy->id), "%s", v);
         else if (!strcmp(sec, "system") && !strcmp(k, "name")) snprintf(sy->name, sizeof(sy->name), "%s", v);
         else if (!strcmp(sec, "system") && !strcmp(k, "order")) sy->order = atoi(v);
         else if (!strcmp(sec, "library") && !strcmp(k, "layout")) snprintf(sy->layout, sizeof(sy->layout), "%s", v);
         else if (!strcmp(sec, "library") && !strcmp(k, "paths")) snprintf(sy->paths, sizeof(sy->paths), "%s", v);
         else if (!strcmp(sec, "library") && !strcmp(k, "extensions")) snprintf(sy->exts, sizeof(sy->exts), "%s", v);
         else if (!strcmp(sec, "launch") && !strcmp(k, "core")) snprintf(sy->core, sizeof(sy->core), "%s", v);
      }
      fclose(f);
      if (!sy->id[0]) snprintf(sy->id, sizeof(sy->id), "%.*s", (int)(l - 4 < 31 ? l - 4 : 31), e->d_name);
      snprintf(id, sizeof(id), "%s", sy->id);   /* a copy: source and destination share the struct */
      if (!sy->name[0]) snprintf(sy->name, sizeof(sy->name), "%s", id);
      if (!sy->layout[0]) snprintf(sy->layout, sizeof(sy->layout), "files");
      if (!strcmp(sy->layout, "stock")) continue;
      if (!sy->paths[0]) snprintf(sy->paths, sizeof(sy->paths), "%s/games/%s", g_root, id);
      n++;
   }
   closedir(d);
   qsort(w_sys, (size_t)n, sizeof(asys_t), cmp_sys);
   return n;
}

static int worker_main(void *arg)
{
   int n, i;
   double t0 = now_s();
   (void)arg;
   n = systems_load();
   for (i = 0; i < n; i++)
   {
      const asys_t *sy = &w_sys[i];
      char paths[512], p[700], *tok, *save = NULL;
      struct stat st;
      FILE *f;
      int before = s_games;
      w_nprot = 0;
      snprintf(p, sizeof(p), "%s/systems/%s.protected", g_home, sy->id);
      if ((f = fopen(p, "r")))
      {
         char line[64];
         while (fgets(line, sizeof(line), f) && w_nprot < 512)
         { char *s = trim(line); if (*s && *s != '#') serial_norm(w_prot[w_nprot++], sizeof(w_prot[0]), s); }
         fclose(f);
      }
      snprintf(paths, sizeof(paths), "%s", sy->paths);
      for (tok = strtok_r(paths, ";", &save); tok; tok = strtok_r(NULL, ";", &save))
      {
         size_t l;
         tok = trim(tok);
         for (l = strlen(tok); l > 1 && tok[l - 1] == '/'; ) tok[--l] = 0;
         if (!*tok) continue;
         if (!strcmp(sy->layout, "psc")) audit_psc(sy, tok);
         else if (!strcmp(sy->layout, "apps")) audit_apps(sy, tok);
         else audit_files(sy, tok);
      }
      /* games without the core they start with (not asked of a system with no games) */
      if (s_games > before && strcmp(sy->layout, "apps"))
      {
         snprintf(p, sizeof(p), "%s/%s", g_core_dir, sy->core);
         if (!sy->core[0] || stat(p, &st) != 0) issue(K_NOCORE, sy, sy->name, sy->core[0] ? sy->core : "core =", g_core_dir);
      }
   }
   qsort(s_iss, (size_t)s_niss, sizeof(issue_t), cmp_issue);
   s_nsys = n;
   logf_("audit: %d problem(s) in %d game(s) of %d system(s), %.2f s", s_niss, s_games, n, now_s() - t0);
   for (i = 0; i < s_niss && i < 40; i++)
      logf_("audit: [%s] %s: %s (%s) in %s", s_iss[i].sys, s_iss[i].name, k_key[s_iss[i].kind] + 6, s_iss[i].file, s_iss[i].dir);
   s_running = 0;
   return 0;
}

/* ------------------------------------------------------------------------ */
/* UI                                                                        */
static int     s_open, s_sel, s_top, s_gen;
static double  s_t0;
static spring  s_fade, s_pill, s_scroll;
static label_t s_kick, s_title, s_cap, s_count, s_msg, s_file, s_where, s_ok1, s_ok2;
static label_t s_rl[POOL], s_rv[POOL];
static int     s_slot[POOL], s_slot_gen[POOL];

static void start_check(void)
{
   SDL_Thread *th;
   if (s_running) return;
   s_niss = 0; s_games = 0; s_nsys = 0; s_more = 0; s_sel = s_top = 0; s_gen++;
   s_pill.x = s_pill.v = s_scroll.x = s_scroll.v = 0;
   s_running = 1;
   if (!(th = SDL_CreateThread(worker_main, "audit", NULL))) { s_running = 0; logf_("audit: no thread (%s)", SDL_GetError()); return; }
   SDL_DetachThread(th);
}

void audit_open(void)
{
   int i;
   if (s_open) return;
   for (i = 0; i < POOL; i++) s_slot[i] = -1;
   s_open = 1; s_t0 = now_s();
   if (s_fade.x < 0.02f) { s_fade.x = 0; s_fade.v = 0; }
   start_check();
   music_sfx(SFX_OPEN);
}

int   audit_active(void) { return s_open || s_fade.x > 0.01f; }
int   audit_open_now(void) { return s_open; }
float audit_amount(void) { return clampf(s_fade.x, 0.0f, 1.0f); }

void audit_update(const ui_nav_t *nav, double t, float dt)
{
   int before = s_sel, n = s_running ? 0 : s_niss;
   spring_step(&s_fade, s_open ? 1.0f : 0.0f, 120, 18, dt);
   if (!s_open)
   {
      if (s_fade.x <= 0.01f && s_title.tex)
      {
         label_t *all[] = { &s_kick, &s_title, &s_cap, &s_count, &s_msg, &s_file, &s_where, &s_ok1, &s_ok2 };
         int i;
         for (i = 0; i < (int)(sizeof(all) / sizeof(all[0])); i++) label_free(all[i]);
         for (i = 0; i < POOL; i++) { label_free(&s_rl[i]); label_free(&s_rv[i]); s_slot[i] = -1; }
      }
      return;
   }
   if (nav && t - s_t0 >= 0.15)
   {
      if (nav->back || nav->options) { s_open = 0; music_sfx(SFX_BACK); return; }
      if (nav->confirm && !s_running) { start_check(); music_sfx(SFX_CONFIRM); return; }
      if (n)
      {
         if (nav->up && s_sel > 0) s_sel--;
         if (nav->down && s_sel < n - 1) s_sel++;
         if (nav->l1) s_sel = s_sel - VIS < 0 ? 0 : s_sel - VIS;
         if (nav->r1) s_sel = s_sel + VIS > n - 1 ? n - 1 : s_sel + VIS;
         if (s_sel != before) music_sfx(SFX_FOCUS);
      }
   }
   if (s_sel < s_top) s_top = s_sel;
   if (s_sel > s_top + VIS - 1) s_top = s_sel - VIS + 1;
   spring_step(&s_scroll, s_top * ROW, 240, 26, dt);
   spring_step(&s_pill, s_sel * ROW, 260, 26, dt);
}

static void col4(float o[4], int tc, float a) { theme_color(tc, o); o[3] = a; }

static void draw_label(const label_t *l, float x, float y, const float c[4])
{
   if (l->tex && c[3] > 0.003f) draw_2d(l->tex, floorf(x), floorf(y), l->w, l->h, c, 0);
}

/* a label of several lines, rendered again only when its text changes */
static void wrap_set(label_t *l, TTF_Font *f, const char *s, int wrap)
{
   size_t n = sizeof(l->key) - 1;
   int same = strlen(s) >= n ? !strncmp(l->key, s, n) : !strcmp(l->key, s);
   if (same && (l->tex || !*s)) return;
   label_free(l);
   snprintf(l->key, sizeof(l->key), "%s", s);
   if (*s) l->tex = text_tex_wrapped(f, s, wrap, &l->w, &l->h);
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

/* "USB / games / psx / 14" */
static void where_text(char *out, size_t n, const char *dir)
{
   size_t rl = strlen(g_root), o;
   const char *p = dir;
   if (!strncmp(dir, g_root, rl) && (dir[rl] == '/' || !dir[rl])) { p = dir + rl; o = (size_t)snprintf(out, n, "%s", tr("fp.root")); }
   else { out[0] = 0; o = 0; }
   while (*p && o + 6 < n)
   {
      if (*p == '/' && o) o += (size_t)snprintf(out + o, n - o, " / ");
      else if (*p != '/') { out[o++] = *p; out[o] = 0; }
      p++;
   }
}

void audit_draw(double t)
{
   const float a = audit_amount(), cx = (W - CW) * 0.5f, cy = 112.0f + (1 - a) * 16;
   const float lx = cx + LIST_X, ly = cy + LIST_Y, lh = VIS * ROW, dx = lx + LIST_W + 42, dw = CW - LIST_X - LIST_W - 42 - PADX;
   float txt[4], dim[4], sep[4];
   char buf[700], n1[16], n2[16];
   const int running = s_running, n = running ? 0 : s_niss;
   int i;
   if (a <= 0.003f) return;
   col4(txt, TC_TEXT, a); col4(dim, TC_DIM, a); col4(sep, TC_TEXT, 0.08f * a);
   gl_use_2d();
   ui_scrim(a * (1.0f - panel_amount()));
   ui_card(cx, cy, CW, CH, a);
   ui_kicker(&s_kick, tr("audit.title"), cx + PADX, cy + 24, a);
   snprintf(n1, sizeof(n1), "%d%s", n, s_more ? "+" : "");
   label_set(&s_title, g_font_title, running ? tr("audit.running") : !n ? tr("audit.none") : n == 1 ? tr("audit.one") :
             trf(buf, sizeof(buf), "audit.n", "n", n1, NULL));
   draw_label(&s_title, cx + PADX, cy + 42, txt);
   snprintf(n1, sizeof(n1), "%d", (int)s_games); snprintf(n2, sizeof(n2), "%d", s_nsys);
   label_set(&s_cap, g_font_small, running ? trf(buf, sizeof(buf), "audit.progress", "n", n1, NULL) :
             trf(buf, sizeof(buf), "audit.checked", "n", n1, "s", n2, NULL));
   draw_label(&s_cap, cx + CW - PADX - s_cap.w, cy + 58, dim);
   draw_2d(0, cx + PADX, cy + 96, CW - 2 * PADX, 1, sep, 0);

   if (running) { draw_dots(cx + CW * 0.5f, ly + lh * 0.5f, t, dim); return; }
   if (!n)
   {  /* nothing wrong: say so, and what a clean library means */
      label_set(&s_ok1, g_font_medium, tr("audit.ok"));
      wrap_set(&s_ok2, g_font_small, tr("audit.ok_note"), (int)(CW - 2 * PADX - 240));
      draw_label(&s_ok1, cx + (CW - s_ok1.w) * 0.5f, ly + lh * 0.5f - 44, txt);
      draw_label(&s_ok2, cx + (CW - s_ok2.w) * 0.5f, ly + lh * 0.5f - 4, dim);
      return;
   }

   glEnable(GL_SCISSOR_TEST);
   gl_scissor(lx - 8, ly - 8, LIST_W + 16, lh + 16);
   ui_focus(lx, ly + s_pill.x - s_scroll.x + 2, LIST_W, ROW - 4, (ROW - 4) * 0.5f, a);
   for (i = (int)(s_scroll.x / ROW) - 1; i < n; i++)
   {
      float y, c[4];
      int slot;
      if (i < 0) continue;
      y = ly + i * ROW - s_scroll.x;
      if (y >= ly + lh - 1) break;
      if (y + ROW <= ly + 1) continue;
      slot = i % POOL;
      if (s_slot[slot] != i || s_slot_gen[slot] != s_gen)
      {
         label_set(&s_rl[slot], g_font_sub, ui_fit(g_font_sub, s_iss[i].name, LIST_W - 230, buf, sizeof(buf)));
         label_set(&s_rv[slot], g_font_sub, ui_fit(g_font_sub, s_iss[i].sys, 170, buf, sizeof(buf)));
         s_slot[slot] = i; s_slot_gen[slot] = s_gen;
      }
      memcpy(c, txt, sizeof(c));
      if (i != s_sel) c[3] *= 0.86f;
      draw_label(&s_rl[slot], lx + 20, y + (ROW - s_rl[slot].h) * 0.5f, c);
      draw_label(&s_rv[slot], lx + LIST_W - 20 - s_rv[slot].w, y + (ROW - s_rv[slot].h) * 0.5f, dim);
   }
   glDisable(GL_SCISSOR_TEST);
   if (n > VIS)
   {
      float th = lh * VIS / (float)n, ty = ly + (lh - th) * clampf(s_scroll.x / ((n - VIS) * ROW), 0.0f, 1.0f);
      float tc[4]; col4(tc, TC_TEXT, 0.22f * a);
      draw_round_rect(lx + LIST_W + 6, ty, 4, th, 2, tc, 0.0f);
   }
   draw_2d(0, lx + LIST_W + 22, ly, 1, lh, sep, 0);
   {  /* the highlighted problem: what, which file, where */
      const issue_t *is = &s_iss[s_sel];
      float y = ly + 8;
      wrap_set(&s_msg, g_font_medium, tr(k_key[is->kind]), (int)dw);
      draw_label(&s_msg, dx, y, txt);
      y += s_msg.h + 14;
      wrap_set(&s_file, g_font_small, is->file, (int)dw);
      draw_label(&s_file, dx, y, txt);
      if (is->file[0]) y += s_file.h + 14;
      where_text(buf, sizeof(buf), is->dir);
      wrap_set(&s_where, g_font_small, buf, (int)dw);
      draw_label(&s_where, dx, y, dim);
   }
   snprintf(n1, sizeof(n1), "%d", s_sel + 1); snprintf(n2, sizeof(n2), "%d", n);
   label_set(&s_count, g_font_small, trf(buf, sizeof(buf), "fp.count", "i", n1, "n", n2, NULL));
   draw_label(&s_count, cx + PADX, cy + CH - 14 - s_count.h, dim);
}
