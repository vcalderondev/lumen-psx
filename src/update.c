/* Software update, see update.h.
 *
 * One worker thread at a time (a check or a download), started on request; the UI reads a
 * copy of the state that update_poll() refreshes once per frame. Network transfers run
 * net.sh's "http" command with fork/exec (curl from RAM, the same package as Wi-Fi), so
 * this file only parses, compares, inflates (zlib) and hashes. Every file on the stick is
 * written under <home>/.update/ with fsync; nothing outside it changes before boot.sh
 * runs update.sh (see that script for the install itself). */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>
#include "lumen.h"
#include "settings.h"
#include "update.h"
#include "version.h"

#define REPO        "vcalderondev/lumen-psx"
#define RELEASES    "https://lpsx.dev/services/releases"      /* lumen-web netlify/functions/releases.mts */
#define GH_LATEST   "https://api.github.com/repos/" REPO "/releases/latest"
#define CDN         "https://github.com/" REPO "/releases/download/"
#define BOOT_DIR    "d8781dae-c45c-4fc1-b961-e03eedebbffb"
#define TMP         "/tmp/lumen-update"
#define GAP_MERGE   (256 * 1024)        /* entries closer than this share one request (lumen-web zip-remote.ts) */
#define GROUP_MAX   (8 * 1024 * 1024)   /* bytes of one request, in RAM (/tmp) */
#define DAY         86400.0

/* ------------------------------------------------------------------------ */
/* state                                                                     */
static SDL_mutex    *s_mx;
static update_info_t w_info;            /* the worker's, under s_mx */
static update_info_t s_ui;              /* the UI's copy */
static const char   *w_toast;           /* under s_mx */
static int           w_busy, w_cancel;  /* under s_mx */
static int           w_checked;         /* a check finished (successfully): update_poll records it */
static int           w_auto;            /* the running check was the daily one */
static double        s_next_auto = -1;  /* now_s() of the next daily check, -1 = once online */
static double        s_online_since = -1;
static char          s_script[300];
static char          s_stage[300];      /* <home>/.update */

/* what the check found: the prepare step needs it */
static char w_tag[40], w_zip[96], w_man[96], w_man_sha[65];
static long long w_zip_size;

static void lock(void) { SDL_LockMutex(s_mx); }
static void unlock(void) { SDL_UnlockMutex(s_mx); }
static void set_state(int st) { lock(); w_info.state = st; unlock(); }
static void fail(const char *key)
{
   lock(); w_info.state = UPD_ERROR; w_info.error = key; unlock();
   logf_("update: error %s", key);
}
static int cancelled(void) { int c; lock(); c = w_cancel; unlock(); return c; }

/* ------------------------------------------------------------------------ */
/* SHA-256 (FIPS 180-4)                                                      */
typedef struct { uint32_t h[8]; uint64_t len; unsigned char buf[64]; size_t n; } sha_t;
static const uint32_t K[64] = {
   0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
   0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
   0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
   0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
   0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
   0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
   0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
   0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
static void sha_block(sha_t *s, const unsigned char *p)
{
   uint32_t w[64], a, b, c, d, e, f, g, h, t1, t2;
   int i;
   for (i = 0; i < 16; i++) w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
   for (; i < 64; i++)
   {
      uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
      uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
   }
   a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3]; e = s->h[4]; f = s->h[5]; g = s->h[6]; h = s->h[7];
   for (i = 0; i < 64; i++)
   {
      t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
      t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
      h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
   }
   s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}
static void sha_init(sha_t *s)
{
   static const uint32_t h0[8] = { 0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19 };
   memcpy(s->h, h0, sizeof(h0)); s->len = 0; s->n = 0;
}
static void sha_add(sha_t *s, const void *data, size_t n)
{
   const unsigned char *p = data;
   s->len += n;
   while (n)
   {
      size_t c = 64 - s->n < n ? 64 - s->n : n;
      memcpy(s->buf + s->n, p, c); s->n += c; p += c; n -= c;
      if (s->n == 64) { sha_block(s, s->buf); s->n = 0; }
   }
}
static void sha_hex(sha_t *s, char out[65])
{
   uint64_t bits = s->len * 8;
   unsigned char pad = 0x80, z = 0, l[8];
   int i;
   sha_add(s, &pad, 1);
   while (s->n != 56) sha_add(s, &z, 1);
   for (i = 0; i < 8; i++) l[i] = (unsigned char)(bits >> (56 - 8 * i));
   sha_add(s, l, 8);
   for (i = 0; i < 8; i++) snprintf(out + i * 8, 9, "%08x", s->h[i]);
}
/* SHA-256 of a file; -1 when it cannot be read */
static int sha_file(const char *path, char out[65], long long *size)
{
   static unsigned char buf[65536];
   sha_t s;
   ssize_t r;
   int fd = open(path, O_RDONLY);
   if (fd < 0) return -1;
   sha_init(&s);
   while ((r = read(fd, buf, sizeof(buf))) > 0) sha_add(&s, buf, (size_t)r);
   close(fd);
   if (r < 0) return -1;
   if (size) *size = (long long)s.len;
   sha_hex(&s, out);
   return 0;
}

/* ------------------------------------------------------------------------ */
/* files                                                                     */
static int mkdirs(const char *path)   /* mkdir -p */
{
   char p[512];
   size_t i;
   snprintf(p, sizeof(p), "%s", path);
   for (i = 1; p[i]; i++)
      if (p[i] == '/') { p[i] = 0; if (mkdir(p, 0755) != 0 && errno != EEXIST) return -1; p[i] = '/'; }
   return mkdir(p, 0755) != 0 && errno != EEXIST ? -1 : 0;
}
static void fsync_dir_of(const char *path)
{
   char d[512], *sl;
   int fd;
   snprintf(d, sizeof(d), "%s", path);
   if (!(sl = strrchr(d, '/'))) return;
   *sl = 0;
   if ((fd = open(d, O_RDONLY | O_DIRECTORY)) >= 0) { fsync(fd); close(fd); }
}
/* write a whole small file: <path>.tmp, fsync, rename, fsync of the folder */
static int write_atomic(const char *path, const void *data, size_t n)
{
   char tmp[520];
   int fd, ok;
   snprintf(tmp, sizeof(tmp), "%s.tmp", path);
   if ((fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0) return -1;
   ok = write(fd, data, n) == (ssize_t)n && fsync(fd) == 0;
   if (close(fd) != 0) ok = 0;
   if (!ok || rename(tmp, path) != 0) { unlink(tmp); return -1; }
   fsync_dir_of(path);
   return 0;
}
static char *read_all(const char *path, size_t *len)
{
   FILE *f = fopen(path, "rb");
   char *b = NULL;
   long n;
   if (!f) return NULL;
   if (fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) >= 0 && fseek(f, 0, SEEK_SET) == 0 && (b = malloc((size_t)n + 1)))
   {
      if (fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
      else { b[n] = 0; if (len) *len = (size_t)n; }
   }
   fclose(f);
   return b;
}
static int rm_rf(const char *path)
{
   struct stat st;
   if (lstat(path, &st) != 0) return errno == ENOENT ? 0 : -1;
   if (S_ISDIR(st.st_mode))
   {
      DIR *d = opendir(path);
      struct dirent *e;
      char sub[600];
      if (!d) return -1;
      while ((e = readdir(d)))
      {
         if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
         snprintf(sub, sizeof(sub), "%s/%s", path, e->d_name);
         rm_rf(sub);
      }
      closedir(d);
      return rmdir(path);
   }
   return unlink(path);
}
static int exists(const char *p) { struct stat st; return stat(p, &st) == 0; }

/* stick path of a manifest path: lumen/... lives in <home>, the rest at the stick root */
static void stick_path(char *out, size_t n, const char *mp)
{
   if (!strncmp(mp, "lumen/", 6)) snprintf(out, n, "%s/%s", g_home, mp + 6);
   else if (!strcmp(mp, "lumen")) snprintf(out, n, "%s", g_home);
   else snprintf(out, n, "%s/%s", g_root, mp);
}

/* ------------------------------------------------------------------------ */
/* net.sh http                                                               */
static int http(const char *url, const char *out, long long from, long long to)
{
   char range[48];
   const char *argv[8];
   int st = 0, k = 0;
   pid_t pid;
   argv[k++] = "sh"; argv[k++] = s_script; argv[k++] = "http"; argv[k++] = url; argv[k++] = out;
   if (from >= 0) { snprintf(range, sizeof(range), "%lld-%lld", from, to); argv[k++] = range; }
   argv[k] = NULL;
   pid = fork();
   if (pid < 0) return -1;
   if (pid == 0)
   {
      int nul = open("/dev/null", O_RDWR);
      if (nul >= 0) { dup2(nul, 0); dup2(nul, 1); dup2(nul, 2); }
      execv("/bin/sh", (char *const *)argv);
      _exit(127);
   }
   while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
   return WIFEXITED(st) && WEXITSTATUS(st) == 0 && exists(out) ? 0 : -1;
}

/* ------------------------------------------------------------------------ */
/* JSON: just enough for the two release answers (strings and numbers of an object,
 * objects in an array), never trusting the text to be well formed */
static const char *skip_str(const char *p, const char *e)   /* p at the opening quote */
{
   for (p++; p < e; p++) { if (*p == '\\') p++; else if (*p == '"') return p + 1; }
   return e;
}
static const char *skip_val(const char *p, const char *e)
{
   int depth = 0;
   for (; p < e; )
   {
      if (*p == '"') { p = skip_str(p, e); if (!depth) return p; continue; }
      if (*p == '{' || *p == '[') depth++;
      else if (*p == '}' || *p == ']') { if (--depth <= 0) return p + 1; }
      else if (!depth && (*p == ',')) return p;
      p++;
   }
   return e;
}
/* the value of key in the object [s, e) (s at '{'), not in nested ones: [*vs, *ve) */
static int json_val(const char *s, const char *e, const char *key, const char **vs, const char **ve)
{
   const char *p = s + 1;
   size_t kl = strlen(key);
   while (p < e)
   {
      while (p < e && *p != '"' && *p != '}') p++;
      if (p >= e || *p == '}') return -1;
      {
         const char *ks = p + 1, *kend = skip_str(p, e);
         int match = (size_t)(kend - 1 - ks) == kl && !strncmp(ks, key, kl);
         p = kend;
         while (p < e && (*p == ' ' || *p == ':' || *p == '\n' || *p == '\r' || *p == '\t')) p++;
         if (match) { *vs = p; *ve = skip_val(p, e); return 0; }
         p = skip_val(p, e);
         while (p < e && (*p == ',' || *p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++;
      }
   }
   return -1;
}
static int json_str(const char *s, const char *e, const char *key, char *out, size_t n)
{
   const char *vs, *ve;
   size_t l;
   out[0] = 0;
   if (json_val(s, e, key, &vs, &ve) != 0 || *vs != '"' || ve - vs < 2) return -1;
   l = (size_t)(ve - vs - 2);
   if (l >= n) return -1;
   memcpy(out, vs + 1, l); out[l] = 0;
   return 0;
}
static long long json_num(const char *s, const char *e, const char *key)
{
   const char *vs, *ve;
   if (json_val(s, e, key, &vs, &ve) != 0) return -1;
   return *vs >= '0' && *vs <= '9' ? strtoll(vs, NULL, 10) : -1;
}
static int json_obj(const char *s, const char *e, const char *key, const char **os, const char **oe)
{
   if (json_val(s, e, key, os, oe) != 0 || **os != '{') return -1;
   return 0;
}

/* ------------------------------------------------------------------------ */
/* versions (lumen-web src/app/core/version.ts compareVersions)              */
static int vcmp(const char *a, const char *b)
{
   int i;
   const char *pa, *pb;
   if (*a == 'v') a++;
   if (*b == 'v') b++;
   for (i = 0; i < 3; i++)
   {
      long x = strtol(a, (char **)&a, 10), y = strtol(b, (char **)&b, 10);
      if (x != y) return x < y ? -1 : 1;
      if (*a == '.') a++;
      if (*b == '.') b++;
   }
   pa = *a == '-' ? a + 1 : NULL; pb = *b == '-' ? b + 1 : NULL;   /* a prerelease is older */
   if (!pa && !pb) return 0;
   if (!pa) return 1;
   if (!pb) return -1;
   i = strcmp(pa, pb);
   return i < 0 ? -1 : i > 0;
}
static void installed_version(char *out, size_t n)
{
   char path[300], *t, *p;
   snprintf(out, n, "%s", LUMEN_VERSION);
   snprintf(path, sizeof(path), "%s/VERSION", g_home);
   if (!(t = read_all(path, NULL))) return;
   if ((p = strstr(t, "Lumen PSX ")))
   {
      size_t l;
      p += 10; if (*p == 'v') p++;
      l = strspn(p, "0123456789.-abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ");
      if (l && l < n) { memcpy(out, p, l); out[l] = 0; }
   }
   free(t);
}

/* ------------------------------------------------------------------------ */
/* check                                                                     */
static int sums_lookup(const char *sums, const char *name, char out[65])
{
   const char *p = sums;
   size_t nl = strlen(name);
   while (p && *p)
   {
      const char *e = strchr(p, '\n'), *q = p + 64;
      size_t l = e ? (size_t)(e - p) : strlen(p);
      while (l && (p[l - 1] == '\r' || p[l - 1] == ' ')) l--;
      if (l > 66 && (q[0] == ' ') && (q[1] == ' ' || q[1] == '*'))
      {
         const char *nm = q + 2;
         if ((size_t)(p + l - nm) == nl && !strncmp(nm, name, nl)) { memcpy(out, p, 64); out[64] = 0; return 0; }
      }
      p = e ? e + 1 : NULL;
   }
   return -1;
}

/* lpsx.dev first (what the web uses, cached there); GitHub straight away when it is down */
static int fetch_latest(char *version, size_t vn)
{
   size_t len;
   char *t, path[64];
   const char *rs, *re, *os, *oe;
   int ok = 0;
   snprintf(path, sizeof(path), TMP "/release.json");
   w_tag[0] = w_zip[0] = w_man[0] = w_man_sha[0] = 0; w_zip_size = -1;
   if (http(RELEASES, path, -1, -1) == 0 && (t = read_all(path, &len)))
   {
      if (json_obj(t, t + len, "release", &rs, &re) == 0)
      {
         json_str(rs, re, "tag", w_tag, sizeof(w_tag));
         json_str(rs, re, "version", version, vn);
         if (json_obj(rs, re, "asset", &os, &oe) == 0)
         { json_str(os, oe, "name", w_zip, sizeof(w_zip)); w_zip_size = json_num(os, oe, "size"); }
         if (json_obj(rs, re, "manifest", &os, &oe) == 0)
         { json_str(os, oe, "name", w_man, sizeof(w_man)); json_str(os, oe, "sha256", w_man_sha, sizeof(w_man_sha)); }
         ok = w_tag[0] && version[0];
      }
      else if (strstr(t, "\"release\":null")) { free(t); unlink(path); return 1; }   /* no release yet */
      free(t);
   }
   unlink(path);
   if (!ok)
   {  /* GitHub: the tag and the assets; the checksums from SHA256SUMS on the CDN */
      const char *as, *ae, *p;
      logf_("update: lpsx.dev did not answer, asking GitHub");
      if (http(GH_LATEST, path, -1, -1) != 0 || !(t = read_all(path, &len))) { unlink(path); return -1; }
      if (json_str(t, t + len, "tag_name", w_tag, sizeof(w_tag)) == 0 && json_val(t, t + len, "assets", &as, &ae) == 0 && *as == '[')
      {
         snprintf(version, vn, "%s", w_tag[0] == 'v' ? w_tag + 1 : w_tag);
         for (p = as + 1; p < ae; )
         {
            char name[96];
            while (p < ae && *p != '{') p++;
            if (p >= ae) break;
            os = p; oe = skip_val(p, ae); p = oe;
            if (json_str(os, oe, "name", name, sizeof(name)) != 0) continue;
            if (!strncmp(name, "lumen-psx-", 10) && strlen(name) > 4 && !strcmp(name + strlen(name) - 4, ".zip"))
            { snprintf(w_zip, sizeof(w_zip), "%s", name); w_zip_size = json_num(os, oe, "size"); }
         }
         ok = w_zip[0] != 0;
      }
      free(t); unlink(path);
      if (!ok) return -1;
      snprintf(w_man, sizeof(w_man), "%.*s.manifest", (int)(strlen(w_zip) - 4), w_zip);
      {
         char url[300], *sums;
         snprintf(url, sizeof(url), CDN "%s/SHA256SUMS", w_tag);
         if (http(url, path, -1, -1) != 0 || !(sums = read_all(path, NULL))) { unlink(path); return -1; }
         if (sums_lookup(sums, w_man, w_man_sha) != 0) w_man_sha[0] = 0;
         free(sums); unlink(path);
      }
   }
   /* the names go into URLs and file names: only what a release can be called */
   if (strspn(w_tag, "v0123456789.-abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ") != strlen(w_tag) ||
       strncmp(w_zip, "lumen-psx-", 10) || strchr(w_zip, '/') || strchr(w_man, '/')) return -1;
   return 0;
}

static int check_worker(void *unused)
{
   char ver[32] = "", inst[32];
   int rc;
   (void)unused;
   mkdirs(TMP);
   installed_version(inst, sizeof(inst));
   lock(); snprintf(w_info.installed, sizeof(w_info.installed), "%s", inst); unlock();
   rc = fetch_latest(ver, sizeof(ver));
   if (rc < 0) fail("update.err_offline");
   else
   {
      int newer = rc == 0 && vcmp(inst, ver) < 0;
      lock();
      snprintf(w_info.version, sizeof(w_info.version), "%s", rc == 0 ? ver : "");
      w_info.state = newer ? (w_man_sha[0] ? UPD_AVAILABLE : UPD_ERROR) : UPD_CURRENT;
      if (newer && !w_man_sha[0]) w_info.error = "update.err_old_release";
      if (newer && w_man_sha[0] && w_auto) w_toast = "update.toast_available";
      w_checked = 1;
      unlock();
      logf_("update: installed %s, latest %s (%s)", inst, rc == 0 ? ver : "-", newer ? "newer" : "current");
   }
   rmdir(TMP);
   lock(); w_busy = 0; unlock();
   return 0;
}

/* ------------------------------------------------------------------------ */
/* manifests                                                                 */
typedef struct { const char *path, *sha; long long size; int dir; } ment_t;
typedef struct { char *text; ment_t *e; int n; } man_t;

static int ment_cmp(const void *a, const void *b) { return strcmp(((const ment_t *)a)->path, ((const ment_t *)b)->path); }
static int man_parse(man_t *m, char *text)
{
   char *line, *save = NULL;
   int cap = 0;
   memset(m, 0, sizeof(*m));
   m->text = text;
   for (line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save))
   {
      ment_t x;
      char *p;
      size_t l = strlen(line);
      if (l && line[l - 1] == '\r') line[--l] = 0;
      memset(&x, 0, sizeof(x));
      if (line[0] == 'd' && line[1] == ' ') { x.dir = 1; x.path = line + 2; }
      else if (line[0] == 'f' && line[1] == ' ' && l > 2 + 64 + 3 && line[66] == ' ')
      {
         line[66] = 0; x.sha = line + 2;
         x.size = strtoll(line + 67, &p, 10);
         if (*p != ' ') continue;
         x.path = p + 1;
      }
      else continue;
      /* nothing outside the stick: no absolute paths, no "..", no backslashes */
      if (!*x.path || x.path[0] == '/' || strstr(x.path, "..") || strchr(x.path, '\\')) continue;
      if (m->n == cap)
      {
         ment_t *ne = realloc(m->e, sizeof(ment_t) * (size_t)(cap = cap ? cap * 2 : 512));
         if (!ne) return -1;
         m->e = ne;
      }
      m->e[m->n++] = x;
   }
   qsort(m->e, (size_t)m->n, sizeof(ment_t), ment_cmp);
   return m->n > 0 ? 0 : -1;
}
static const ment_t *man_find(const man_t *m, const char *path)
{
   ment_t key;
   if (!m->n) return NULL;
   key.path = path;
   return bsearch(&key, m->e, (size_t)m->n, sizeof(ment_t), ment_cmp);
}
static void man_free(man_t *m) { free(m->e); free(m->text); memset(m, 0, sizeof(*m)); }

/* lumen-web update.ts: isPreserved / CONFIG / scope */
static int preserved(const char *p)
{
   static const char *const pre[] = { "lumen/settings.ini", "lumen/state.txt", "lumen/disable-boot" };
   static const char *const dirs[] = { "lumen/profiles/", "lumen/logs/", "lumen/backup/", "lumen/.update/" };
   size_t i, l = strlen(p);
   for (i = 0; i < sizeof(pre) / sizeof(pre[0]); i++) if (!strcmp(p, pre[i])) return 1;
   for (i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) if (!strncmp(p, dirs[i], strlen(dirs[i]))) return 1;
   return !strncmp(p, "lumen/debug/", 12) && l > 4 && !strcmp(p + l - 4, ".log");
}
static int is_config(const char *p) { return !strcmp(p, "lumen/lumen.conf") || !strcmp(p, "lumen/retroarch/retroarch.cfg"); }
static int is_boot(const char *p) { return !strncmp(p, BOOT_DIR "/", sizeof(BOOT_DIR)); }
static int in_scope(const char *p) { return !strncmp(p, "lumen/", 6) || is_boot(p); }

/* ------------------------------------------------------------------------ */
/* the release zip, read over byte ranges                                    */
typedef struct { const char *name; uint32_t lho, csize, usize; int method; } zent_t;
static uint32_t le16(const unsigned char *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t le32(const unsigned char *p) { return le16(p) | le16(p + 2) << 16; }
static int zent_cmp(const void *a, const void *b) { return strcmp(((const zent_t *)a)->name, ((const zent_t *)b)->name); }

typedef struct { ment_t m; int act; int boot_backup; zent_t z; long long end; } item_t;   /* end: last byte of its zip span */
enum { A_WRITE, A_NEWCOPY };
static int item_off_cmp(const void *a, const void *b)
{
   const item_t *x = a, *y = b;
   return x->z.lho < y->z.lho ? -1 : x->z.lho > y->z.lho;
}

static char *s_cd;            /* central directory */
static char *s_names;         /* the entry names, each terminated */
static zent_t *s_z;
static int s_nz;
static uint32_t s_cd_off;

static int read_cd(const char *zip_url)
{
   char path[64];
   unsigned char *tail;
   size_t tl;
   long long from;
   uint32_t cd_size, i, n;
   const unsigned char *p, *eocd = NULL;
   snprintf(path, sizeof(path), TMP "/tail.bin");
   if (w_zip_size < 22) return -1;
   from = w_zip_size > 65557 ? w_zip_size - 65557 : 0;
   if (http(zip_url, path, from, w_zip_size - 1) != 0 || !(tail = (unsigned char *)read_all(path, &tl))) { unlink(path); return -1; }
   unlink(path);
   for (p = tail + tl - 22; p >= tail; p--) if (le32(p) == 0x06054b50) { eocd = p; break; }
   if (!eocd) { free(tail); return -1; }
   n = le16(eocd + 10); cd_size = le32(eocd + 12); s_cd_off = le32(eocd + 16);
   if (n == 0xFFFF || cd_size == 0xFFFFFFFFu || s_cd_off == 0xFFFFFFFFu) { free(tail); return -1; }   /* zip64: not ours */
   if (s_cd_off >= from && s_cd_off + (long long)cd_size <= from + (long long)tl)
   {
      if (!(s_cd = malloc(cd_size + 1))) { free(tail); return -1; }
      memcpy(s_cd, tail + (s_cd_off - from), cd_size);
   }
   else
   {
      size_t got;
      if (http(zip_url, path, s_cd_off, (long long)s_cd_off + cd_size - 1) != 0 || !(s_cd = read_all(path, &got)) || got != cd_size)
      { unlink(path); free(tail); return -1; }
      unlink(path);
   }
   free(tail);
   if (!(s_z = calloc(n ? n : 1, sizeof(zent_t))) || !(s_names = malloc((size_t)cd_size + n + 1))) return -1;
   {
   char *np = s_names;
   for (p = (unsigned char *)s_cd, i = 0; i < n; i++)
   {
      uint32_t nl, xl, cl;
      if (p + 46 > (unsigned char *)s_cd + cd_size || le32(p) != 0x02014b50) return -1;
      nl = le16(p + 28); xl = le16(p + 30); cl = le16(p + 32);
      if (p + 46 + nl > (unsigned char *)s_cd + cd_size) return -1;
      s_z[i].method = (int)le16(p + 10); s_z[i].csize = le32(p + 20); s_z[i].usize = le32(p + 24); s_z[i].lho = le32(p + 42);
      memcpy(np, p + 46, nl); np[nl] = 0;
      s_z[i].name = np; np += nl + 1;
      p += 46 + nl + xl + cl;
   }
   }
   s_nz = (int)n;
   qsort(s_z, (size_t)s_nz, sizeof(zent_t), zent_cmp);
   return 0;
}
static const zent_t *zfind(const char *name)
{
   zent_t k;
   k.name = name;
   return s_nz ? bsearch(&k, s_z, (size_t)s_nz, sizeof(zent_t), zent_cmp) : NULL;
}
/* the end of each entry's span: the next local header (or the central directory) */
static long long zspan_end(uint32_t lho)
{
   uint32_t best = s_cd_off;
   int i;
   for (i = 0; i < s_nz; i++) if (s_z[i].lho > lho && s_z[i].lho < best) best = s_z[i].lho;
   return (long long)best - 1;
}

/* inflate one entry from a buffer holding its local header into the staging file,
 * checking the size and SHA-256 the manifest gives */
static int extract(const unsigned char *buf, size_t blen, size_t rel, const item_t *it)
{
   static unsigned char out[65536];
   char dst[600], hex[65];
   const unsigned char *data;
   uint32_t nl, xl;
   sha_t s;
   long long total = 0;
   int fd, ok = 1;
   if (rel + 30 > blen || le32(buf + rel) != 0x04034b50) return -1;
   nl = le16(buf + rel + 26); xl = le16(buf + rel + 28);
   if (rel + 30 + nl + xl + it->z.csize > blen) return -1;
   data = buf + rel + 30 + nl + xl;
   snprintf(dst, sizeof(dst), "%s/files/%s", s_stage, it->m.path);
   { char d[600], *sl; snprintf(d, sizeof(d), "%s", dst); if ((sl = strrchr(d, '/'))) { *sl = 0; if (mkdirs(d) != 0) return -1; } }
   if ((fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0) return -1;
   sha_init(&s);
   if (it->z.method == 0)
   {
      if (it->z.csize != it->z.usize) ok = 0;
      else { ok = write(fd, data, it->z.csize) == (ssize_t)it->z.csize; sha_add(&s, data, it->z.csize); total = it->z.csize; }
   }
   else if (it->z.method == 8)
   {
      z_stream z;
      int zr = Z_OK;
      memset(&z, 0, sizeof(z));
      if (inflateInit2(&z, -15) != Z_OK) ok = 0;
      else
      {
         z.next_in = (Bytef *)data; z.avail_in = it->z.csize;
         while (ok && zr != Z_STREAM_END)
         {
            size_t got;
            z.next_out = out; z.avail_out = sizeof(out);
            zr = inflate(&z, Z_NO_FLUSH);
            if (zr != Z_OK && zr != Z_STREAM_END) { ok = 0; break; }
            got = sizeof(out) - z.avail_out;
            if (got) { ok = write(fd, out, got) == (ssize_t)got; sha_add(&s, out, got); total += (long long)got; }
            else if (zr != Z_STREAM_END && z.avail_in == 0) { ok = 0; break; }
         }
         inflateEnd(&z);
      }
   }
   else ok = 0;
   if (ok && fsync(fd) != 0) ok = 0;
   if (close(fd) != 0) ok = 0;
   sha_hex(&s, hex);
   if (!ok || total != it->m.size || strcmp(hex, it->m.sha))
   {
      logf_("update: %s does not match the release manifest", it->m.path);
      unlink(dst);
      return -2;
   }
   return 0;
}

/* ------------------------------------------------------------------------ */
/* prepare: compare, download, verify, stage                                 */
static void progress(const char *phase, float p) { lock(); w_info.phase = phase; w_info.progress = p; unlock(); }

typedef struct { char *buf; size_t len, cap; } sbuf_t;
static void sb_add(sbuf_t *b, const char *a, const char *path)
{
   size_t need = strlen(a) + strlen(path) + 3;
   if (b->len + need >= b->cap)
   {
      size_t nc = (b->cap + need) * 2;
      char *nb = realloc(b->buf, nc);
      if (!nb) return;
      b->buf = nb; b->cap = nc;
   }
   b->len += (size_t)sprintf(b->buf + b->len, "%s %s\n", a, path);
}
static int rev_cmp(const void *a, const void *b) { return -strcmp(*(const char *const *)a, *(const char *const *)b); }

static int prepare_worker(void *unused)
{
   char url[300], path[600], hex[65], local[600];
   char *text = NULL, *mtext = NULL;
   size_t mlen = 0;
   man_t nm, om;
   item_t *items = NULL;
   const char **drop = NULL;
   int ni = 0, i, ndrop = 0, have_old;
   long long need = 0, dl_total = 0, dl_done = 0;
   sbuf_t plan = { 0 };
   (void)unused;
   memset(&nm, 0, sizeof(nm)); memset(&om, 0, sizeof(om));
   lock(); w_info.state = UPD_PREPARING; w_info.progress = 0; w_info.phase = "update.phase_compare"; unlock();
   rm_rf(s_stage); rm_rf(TMP);
   if (mkdirs(TMP) != 0) { fail("update.err_write"); goto out; }

   /* the release's manifest, checked against SHA256SUMS */
   snprintf(url, sizeof(url), CDN "%s/%s", w_tag, w_man);
   snprintf(path, sizeof(path), TMP "/MANIFEST");
   if (http(url, path, -1, -1) != 0) { fail("update.err_offline"); goto out; }
   if (sha_file(path, hex, NULL) != 0 || strcmp(hex, w_man_sha)) { fail("update.err_verify"); goto out; }
   if (!(mtext = read_all(path, &mlen)) || !(text = malloc(mlen + 1))) { fail("update.err_write"); goto out; }
   memcpy(text, mtext, mlen + 1);
   if (man_parse(&nm, text) != 0) { text = NULL; fail("update.err_verify"); goto out; }
   text = NULL;
   /* what this install was (absent on a hand-copied install: every file is compared) */
   snprintf(path, sizeof(path), "%s/MANIFEST", g_home);
   have_old = (text = read_all(path, NULL)) && man_parse(&om, text) == 0;
   if (!have_old) { if (!om.text) free(text); man_free(&om); }
   text = NULL;

   if (!(items = calloc((size_t)nm.n, sizeof(item_t)))) { fail("update.err_write"); goto out; }
   for (i = 0; i < nm.n; i++)
   {
      const ment_t *e = &nm.e[i], *o;
      struct stat st;
      int has, act = -1;
      if (cancelled()) goto out;
      progress("update.phase_compare", (float)i / (float)nm.n * 0.999f);
      if (e->dir || !in_scope(e->path) || !strcmp(e->path, "lumen/MANIFEST")) continue;
      stick_path(local, sizeof(local), e->path);
      has = stat(local, &st) == 0 && S_ISREG(st.st_mode);
      if (preserved(e->path)) { if (has) continue; act = A_WRITE; }
      o = have_old ? man_find(&om, e->path) : NULL;
      if (act < 0 && !has) act = A_WRITE;
      if (act < 0 && !is_boot(e->path) && !is_config(e->path) && o && !strcmp(o->sha, e->sha) && st.st_size == e->size) continue;
      if (act < 0)
      {
         if (sha_file(local, hex, NULL) != 0) act = A_WRITE;
         else if (!strcmp(hex, e->sha)) continue;                         /* already the new one */
         else if (!is_config(e->path)) act = A_WRITE;
         else if (o && !strcmp(o->sha, e->sha)) continue;                 /* the release kept it; the user's edit stays */
         else if (o && !strcmp(hex, o->sha)) act = A_WRITE;               /* never edited: replaced */
         else act = A_NEWCOPY;                                             /* edited: the new one beside it */
      }
      items[ni].m = *e; items[ni].act = act;
      items[ni].boot_backup = is_boot(e->path) && has;
      ni++;
      need += e->size;
   }
   /* what the release dropped: removed only while it is still the file we installed */
   if (have_old)
   {
      if (!(drop = calloc((size_t)om.n, sizeof(char *)))) { fail("update.err_write"); goto out; }
      for (i = 0; i < om.n; i++)
      {
         const ment_t *o = &om.e[i];
         if (!in_scope(o->path) || preserved(o->path) || !strcmp(o->path, "lumen/MANIFEST") || man_find(&nm, o->path)) continue;
         if (!o->dir)
         {
            stick_path(local, sizeof(local), o->path);
            if (sha_file(local, hex, NULL) != 0 || strcmp(hex, o->sha)) continue;   /* gone, or the user's now */
         }
         drop[ndrop++] = o->path;
      }
   }
   logf_("update: %d files to write (%lld bytes), %d to remove", ni, need, ndrop);

   /* room on the stick for the staged copies (the old files go only at install) */
   {
      struct statvfs sv;
      if (statvfs(g_root, &sv) == 0 && (long long)sv.f_bavail * (long long)sv.f_frsize < need + 16LL * 1024 * 1024)
      { fail("update.err_space"); goto out; }
   }

   /* the zip entries and their spans, in file order */
   snprintf(url, sizeof(url), CDN "%s/%s", w_tag, w_zip);
   if (ni)
   {
      progress("update.phase_download", 0.0f);
      if (read_cd(url) != 0) { fail("update.err_offline"); goto out; }
      for (i = 0; i < ni; i++)
      {
         const zent_t *z = zfind(items[i].m.path);
         if (!z) { fail("update.err_verify"); goto out; }
         items[i].z = *z;
         items[i].end = zspan_end(z->lho);
         dl_total += items[i].end - z->lho + 1;
      }
      qsort(items, (size_t)ni, sizeof(item_t), item_off_cmp);
   }
   lock(); w_info.download = dl_total; w_info.files = ni; unlock();

   /* download in groups of neighbouring entries, extract each, verify each */
   for (i = 0; i < ni; )
   {
      int j = i + 1, k, rc = 0;
      long long from = items[i].z.lho, to = items[i].end;
      unsigned char *buf;
      size_t blen;
      while (j < ni && items[j].z.lho - to - 1 <= GAP_MERGE && items[j].end - from + 1 <= GROUP_MAX) { to = items[j].end; j++; }
      if (cancelled()) goto out;
      snprintf(path, sizeof(path), TMP "/part.bin");
      if (http(url, path, from, to) != 0) { fail("update.err_offline"); goto out; }
      if (!(buf = (unsigned char *)read_all(path, &blen)) || (long long)blen != to - from + 1)
      { free(buf); fail("update.err_offline"); goto out; }
      unlink(path);
      for (k = i; k < j && rc == 0; k++) rc = extract(buf, blen, (size_t)(items[k].z.lho - from), &items[k]);
      free(buf);
      if (rc) { fail(rc == -2 ? "update.err_verify" : "update.err_write"); goto out; }
      dl_done += to - from + 1;
      progress("update.phase_download", dl_total ? (float)((double)dl_done / (double)dl_total) : 1.0f);
      i = j;
   }

   /* the plan: folders, backups, writes (VERSION last), removals, then empty old folders */
   progress("update.phase_finish", 1.0f);
   {
      int v = -1;
      sb_add(&plan, "#", "Lumen PSX update plan 1");
      for (i = 0; i < nm.n; i++)
         if (nm.e[i].dir && (in_scope(nm.e[i].path) || !strcmp(nm.e[i].path, "lumen") || !strcmp(nm.e[i].path, BOOT_DIR) ||
                             (!strncmp(nm.e[i].path, "games/", 6) && !strchr(nm.e[i].path + 6, '/'))))
            sb_add(&plan, "d", nm.e[i].path);
      for (i = 0; i < ni; i++) if (items[i].boot_backup) sb_add(&plan, "b", items[i].m.path);
      for (i = 0; i < ni; i++)
      {
         if (!strcmp(items[i].m.path, "lumen/VERSION")) { v = i; continue; }
         sb_add(&plan, items[i].act == A_NEWCOPY ? "n" : "w", items[i].m.path);
      }
      if (v >= 0) sb_add(&plan, "w", "lumen/VERSION");
      for (i = 0; i < ndrop; i++) if (!man_find(&om, drop[i])->dir) sb_add(&plan, "x", drop[i]);
      {
         const char **dirs = drop ? calloc((size_t)ndrop + 1, sizeof(char *)) : NULL;
         int nd = 0;
         for (i = 0; i < ndrop && dirs; i++) if (man_find(&om, drop[i])->dir) dirs[nd++] = drop[i];
         if (dirs) { qsort(dirs, (size_t)nd, sizeof(char *), rev_cmp); for (i = 0; i < nd; i++) sb_add(&plan, "r", dirs[i]); }
         free((void *)dirs);
      }
      if (!plan.buf) { fail("update.err_write"); goto out; }
   }
   snprintf(path, sizeof(path), "%s/PLAN", s_stage);
   if (mkdirs(s_stage) != 0 || write_atomic(path, plan.buf, plan.len) != 0) { fail("update.err_write"); goto out; }
   snprintf(path, sizeof(path), "%s/MANIFEST", s_stage);
   if (write_atomic(path, mtext, mlen) != 0) { fail("update.err_write"); goto out; }
   {  /* the commit point: from here boot.sh installs it */
      char ready[64];
      int n;
      lock(); n = snprintf(ready, sizeof(ready), "%s\n", w_info.version); unlock();
      if (cancelled()) goto out;
      snprintf(path, sizeof(path), "%s/READY", s_stage);
      if (write_atomic(path, ready, (size_t)n) != 0) { fail("update.err_write"); goto out; }
   }
   set_state(UPD_READY);
   logf_("update: %s staged, %lld bytes downloaded; restart to install", w_info.version, dl_done);

out:
   {
      int st;
      lock(); st = w_info.state; if (w_cancel && st != UPD_READY) w_info.state = UPD_AVAILABLE; w_cancel = 0; unlock();
      if (st != UPD_READY) { rm_rf(s_stage); logf_("update: staging folder removed"); }
   }
   rm_rf(TMP);
   free(items); free((void *)drop); free(plan.buf); free(mtext); free(text);
   man_free(&nm); man_free(&om);
   free(s_cd); s_cd = NULL; free(s_names); s_names = NULL; free(s_z); s_z = NULL; s_nz = 0;
   lock(); w_busy = 0; unlock();
   return 0;
}

/* ------------------------------------------------------------------------ */
/* API                                                                       */
static int start(SDL_ThreadFunction fn, const char *name)
{
   SDL_Thread *th;
   lock();
   if (w_busy) { unlock(); return -1; }
   w_busy = 1; w_cancel = 0;
   unlock();
   if (!(th = SDL_CreateThread(fn, name, NULL))) { lock(); w_busy = 0; unlock(); return -1; }
   SDL_DetachThread(th);
   return 0;
}

int update_staged(void)
{
   char p[320];
   snprintf(p, sizeof(p), "%s/READY", s_stage);
   return exists(p);
}

void update_init(void)
{
   const char *e = getenv("LUMEN_NET_SH");
   s_mx = SDL_CreateMutex();
   snprintf(s_script, sizeof(s_script), "%s", e && *e ? e : "");
   if (!s_script[0]) snprintf(s_script, sizeof(s_script), "%s/net.sh", g_home);
   snprintf(s_stage, sizeof(s_stage), "%s/.update", g_home);
   memset(&w_info, 0, sizeof(w_info));
   installed_version(w_info.installed, sizeof(w_info.installed));
   w_info.last_check = settings_get_int("update_last", 0, 0, 0x7FFFFFFF);
   if (update_staged())
   {  /* staged and never installed (boot.sh installs it at power-on: this is the case
       * of an install that could not finish); offered again */
      char p[320], *v;
      snprintf(p, sizeof(p), "%s/READY", s_stage);
      if ((v = read_all(p, NULL))) { v[strcspn(v, "\r\n")] = 0; snprintf(w_info.version, sizeof(w_info.version), "%s", v); free(v); }
      w_info.state = UPD_READY;
   }
   else if (exists(s_stage)) { rm_rf(s_stage); logf_("update: unfinished download removed"); }
   s_ui = w_info;
}

void update_poll(double now, int online, int clock_ok)
{
   int checked = 0;
   lock();
   checked = w_checked; w_checked = 0;
   s_ui = w_info;
   unlock();
   if (checked)
   {
      s_next_auto = now + DAY;
      if (clock_ok)
      {
         long long t = (long long)time(NULL);
         settings_set_int("update_last", (int)t);
         lock(); w_info.last_check = t; unlock();
         s_ui.last_check = t;
         if (settings_dirty()) settings_save();
      }
   }
   /* the daily look: once online (after 20 s, the clock set by then), then every 24 h of
    * uptime; a check made today, by the clock, is not repeated at the next power-on */
   if (!online) { s_online_since = -1; return; }
   if (s_online_since < 0) s_online_since = now;
   if (!settings_get_int("update_auto", 1, 0, 1) || now - s_online_since < 20.0) return;
   if (s_ui.state == UPD_CHECKING || s_ui.state == UPD_PREPARING || s_ui.state == UPD_READY) return;
   if (s_next_auto < 0)
   {
      long long last = settings_get_int("update_last", 0, 0, 0x7FFFFFFF);
      if (clock_ok && last > 0 && (double)((long long)time(NULL) - last) < DAY) { s_next_auto = now + DAY - (double)((long long)time(NULL) - last); return; }
      s_next_auto = now;
   }
   if (now < s_next_auto) return;
   s_next_auto = now + 600.0;   /* not answered (offline after all): try again in 10 minutes */
   lock(); w_auto = 1; unlock();
   if (start(check_worker, "update-check") == 0) { lock(); w_info.state = UPD_CHECKING; w_info.error = NULL; unlock(); }
}

const update_info_t *update_info(void) { return &s_ui; }

const char *update_take_toast(void)
{
   const char *t;
   lock(); t = w_toast; w_toast = NULL; unlock();
   return t;
}

void update_check(void)
{
   lock(); w_auto = 0; unlock();
   if (s_ui.state == UPD_PREPARING || s_ui.state == UPD_READY) return;
   if (start(check_worker, "update-check") == 0) { lock(); w_info.state = UPD_CHECKING; w_info.error = NULL; unlock(); s_ui.state = UPD_CHECKING; }
}

void update_prepare(void)
{
   if (s_ui.state != UPD_AVAILABLE) return;
   if (start(prepare_worker, "update-get") == 0) { lock(); w_info.state = UPD_PREPARING; w_info.progress = 0; w_info.phase = "update.phase_compare"; unlock(); s_ui.state = UPD_PREPARING; }
}

void update_cancel(void)
{
   lock(); if (w_busy && w_info.state == UPD_PREPARING) w_cancel = 1; unlock();
}
