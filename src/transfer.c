/* USB transfer, see transfer.h.
 *
 * Three parts:
 *   device  a watcher thread polls /sys/block once a second for a USB disk that is not
 *           the one Lumen runs from, mounts its first partition read-only on MNT (the
 *           system's `mount`, so the FUSE helpers of patched consoles work too) and
 *           notices when it goes away. LUMEN_USB2=<folder> replaces all of it with a
 *           folder (psc-sim: the drive is "plugged in" while the folder exists).
 *   worker  one thread for everything that touches a drive: folder listings, the copy,
 *           remounts, sync, eject. The render thread only posts requests and reads
 *           results under a mutex.
 *   UI      static storage like the other modules: two panes, and small cards over a
 *           veil for the destination, the progress and the result.
 *
 * Copy rules: folders are merged; a file that is already on the destination with the
 * same size is skipped (so a copy cut short can simply be repeated); every other file is
 * written as "<name>.lumen-part", flushed every few MB and at its end, then renamed. On
 * an error, a stop or a drive that disappears the unfinished part is removed. The second
 * drive is mounted read-only and made writable only while it is the destination. */
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64   /* 32-bit ARM: disc images over 2 GB */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include "lumen.h"
#include "transfer.h"
#include "i18n.h"
#include "music.h"
#include "panel.h"
#include "theme.h"
#include "ui.h"

/* ------------------------------------------------------------------------ */
/* geometry (logical 1280x720), lumen-design usb-transfer*.html              */
#define CW       1120.0f   /* card                          */
#define CH       540.0f
#define CY       96.0f
#define PADX     28.0f
#define PANE_W   524.0f
#define PANE_Y   108.0f    /* panes, relative to the card   */
#define LIST_DY  60.0f     /* list, relative to the pane    */
#define ROW      44.0f
#define VIS      8
#define POOL     16
#define MAX_ENT  1024
#define MAX_SEL  256
#define MAX_DEST (MAX_PLATFORMS + 1)
#define DVIS     6         /* destination rows visible      */
#define NAME_LEN 256
#define PATH_LEN 1024
#define MNT      "/tmp/lumen-usb2"
#define SEEN     "/tmp/lumen-usb2.seen"
#define PART     ".lumen-part"
#define OFFER_S  8.0

enum { SIDE_OTHER, SIDE_LUMEN };

typedef struct { char name[NAME_LEN]; unsigned char dir, mark; } ent_t;

/* ------------------------------------------------------------------------ */
/* device                                                                    */
enum { DEV_NONE, DEV_MOUNTING, DEV_READY, DEV_UNREADABLE, DEV_EJECTED };
enum { EV_ARRIVED = 1, EV_GONE = 2, EV_UNREADABLE = 4, EV_EJECTED = 8 };

static SDL_mutex *s_mx;          /* every shared field below                          */
static SDL_mutex *s_op_mx;       /* one mount / unmount / remount at a time           */
static SDL_cond  *s_cv;          /* wakes the worker                                  */
static int        s_threads;     /* watcher and worker running                        */
static volatile int s_quit;

static struct {
   int  state, rw, ours, fuse;
   char id[200];                 /* this insertion (sysfs path: a new one per plug-in) */
   char node[80], mnt[300], fs[16];
} s_dev;
static int  s_events;            /* EV_*, taken by the render thread                  */
static volatile int s_lost;      /* the second drive went away (a running copy stops) */
static char s_sim[300];          /* LUMEN_USB2: a folder plays the second drive       */
static char s_root_disk[32];     /* "sda": the disk Lumen runs from, never touched    */
static char s_root_fs[16];
static int  s_utf8;              /* mount FAT with utf8 names (not when the Lumen stick is FAT without it) */
static long s_kbps;              /* LUMEN_USB2_KBPS: test hook, slows the copy down   */
static long s_free_mb = -1;      /* LUMEN_USB2_FREE_MB: test hook, the free space a copy believes in */

/* the mount of `what` (a device node, or a mount point when by_dir) in /proc/mounts */
static int mount_find(const char *what, int by_dir, char *dir, size_t dn, char *type, size_t tn, char *opts, size_t on)
{
   FILE *f = fopen("/proc/mounts", "r");
   char line[1024], dev[256], mp[300], ty[32], op[256];
   int found = 0;
   if (!f) return 0;
   while (fgets(line, sizeof(line), f))
   {
      if (sscanf(line, "%255s %299s %31s %255s", dev, mp, ty, op) != 4) continue;
      if (strcmp(by_dir ? mp : dev, what)) continue;
      if (dir) snprintf(dir, dn, "%s", mp);
      if (type) snprintf(type, tn, "%s", ty);
      if (opts) snprintf(opts, on, "%s", op);
      found = 1;   /* the last one wins: the newest mount on that point */
   }
   fclose(f);
   return found;
}

/* which disk holds g_root: the longest mount point that is a prefix of it */
static void root_find(void)
{
   FILE *f = fopen("/proc/mounts", "r");
   char line[1024], dev[256], mp[300], ty[32], op[256], best[256] = "", bopts[256] = "";
   size_t n = 0;
   if (!f) return;
   while (fgets(line, sizeof(line), f))
   {
      size_t l;
      if (sscanf(line, "%255s %299s %31s %255s", dev, mp, ty, op) != 4) continue;
      l = strlen(mp);
      if (l < n || strncmp(g_root, mp, l) || (l > 1 && g_root[l] && g_root[l] != '/')) continue;
      n = l; snprintf(best, sizeof(best), "%s", dev); snprintf(s_root_fs, sizeof(s_root_fs), "%s", ty);
      snprintf(bopts, sizeof(bopts), "%s", op);
   }
   fclose(f);
   if (!strncmp(best, "/dev/sd", 7))
   {
      size_t l;
      snprintf(s_root_disk, sizeof(s_root_disk), "%s", best + 5);
      for (l = strlen(s_root_disk); l > 0 && s_root_disk[l - 1] >= '0' && s_root_disk[l - 1] <= '9'; ) s_root_disk[--l] = 0;
   }
   /* names must mean the same bytes on both drives: a FAT Lumen stick mounted with the
    * kernel's default (iso8859-1) gets a FAT second drive mounted the same way */
   s_utf8 = strcmp(s_root_fs, "vfat") || strstr(bopts, "utf8") != NULL;
}

static const char *fs_label(const char *fs)
{
   if (!strcmp(fs, "vfat") || !strcmp(fs, "msdos")) return "FAT32";
   if (!strcmp(fs, "exfat")) return "exFAT";
   if (!strncmp(fs, "ntfs", 4)) return "NTFS";
   if (!strncmp(fs, "fuse", 4)) return "";
   return fs;
}

/* run a system tool and wait for it (20 s at most); -1 = could not, else its exit code */
static int run_tool(const char *tool, char *const argv[])
{
   static const char *const dirs[] = { "/bin", "/sbin", "/usr/bin", "/usr/sbin" };
   char path[64] = "";
   pid_t pid;
   int i, st = 0;
   for (i = 0; i < 4 && !path[0]; i++)
   {
      snprintf(path, sizeof(path), "%s/%s", dirs[i], tool);
      if (access(path, X_OK) != 0) path[0] = 0;
   }
   if (!path[0]) return -1;
   pid = fork();
   if (pid < 0) return -1;
   if (pid == 0)
   {
      int nul = open("/dev/null", O_RDWR);
      if (nul >= 0) { dup2(nul, 0); dup2(nul, 1); dup2(nul, 2); }
      execv(path, argv);
      _exit(127);
   }
   for (i = 0; i < 400; i++)
   {
      pid_t r = waitpid(pid, &st, WNOHANG);
      if (r == pid) return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
      if (r < 0 && errno != EINTR) return -1;
      SDL_Delay(50);
   }
   kill(pid, SIGKILL);
   waitpid(pid, &st, 0);
   return -1;
}

/* mount node on MNT; op mutex held. 0 = mounted (type and fuse filled in) */
static int dev_mount(const char *node, int rw, char *fs, size_t fn, int *fuse)
{
   char opts[64], ty[32] = "", mo[256] = "";
   int pass;
   mkdir(MNT, 0755);
   for (pass = s_utf8 ? 0 : 1; pass < 2; pass++)
   {  /* utf8 is a FAT option: other filesystems refuse it, so a plain second try */
      char *argv[] = { "mount", "-o", opts, (char *)node, MNT, NULL };
      snprintf(opts, sizeof(opts), "%s,noatime%s", rw ? "rw" : "ro", pass == 0 ? ",utf8" : "");
      if (run_tool("mount", argv) == 0 && mount_find(MNT, 1, NULL, 0, ty, sizeof(ty), mo, sizeof(mo)))
      {
         snprintf(fs, fn, "%s", ty);
         *fuse = !strncmp(ty, "fuse", 4);
         logf_("transfer: mounted %s (%s, %s) on %s", node, ty, mo, MNT);
         return 0;
      }
   }
   return -1;
}

static void dev_unmount(const char *mnt)
{
   int i;
   sync();
   for (i = 0; i < 10; i++)
   {
      if (umount2(mnt, 0) == 0 || errno == EINVAL || errno == ENOENT) return;
      SDL_Delay(200);
   }
   if (umount2(mnt, MNT_DETACH) != 0) logf_("transfer: umount %s: %s", mnt, strerror(errno));
}

/* is a second drive there? its identity and device node */
static int dev_probe(char *id, size_t idn, char *node, size_t nn)
{
   char best[32] = "", path[300], link[400], buf[32];
   DIR *d;
   struct dirent *e;
   FILE *f;
   ssize_t l;
   int part = -1;
   if (s_sim[0])
   {
      struct stat st;
      if (stat(s_sim, &st) != 0 || !S_ISDIR(st.st_mode)) return 0;
      snprintf(id, idn, "sim:%lu", (unsigned long)st.st_ino);
      snprintf(node, nn, "%s", s_sim);
      return 1;
   }
   if (!s_root_disk[0] || !(d = opendir("/sys/block"))) return 0;
   while ((e = readdir(d)))
   {
      if (strncmp(e->d_name, "sd", 2) || !strcmp(e->d_name, s_root_disk) || strlen(e->d_name) > 8) continue;
      snprintf(path, sizeof(path), "/sys/block/%s/size", e->d_name);
      if (!(f = fopen(path, "r"))) continue;
      buf[0] = 0;
      if (!fgets(buf, sizeof(buf), f) || atoll(buf) <= 0) { fclose(f); continue; }   /* a reader with no card */
      fclose(f);
      if (!best[0] || strcmp(e->d_name, best) < 0) snprintf(best, sizeof(best), "%s", e->d_name);
   }
   closedir(d);
   if (!best[0]) return 0;
   /* its first partition; a drive formatted without a partition table is the disk itself */
   snprintf(path, sizeof(path), "/sys/block/%s", best);
   if ((d = opendir(path)))
   {
      size_t bl = strlen(best);
      while ((e = readdir(d)))
         if (!strncmp(e->d_name, best, bl) && e->d_name[bl] >= '0' && e->d_name[bl] <= '9')
         { int n = atoi(e->d_name + bl); if (part < 0 || n < part) part = n; }
      closedir(d);
   }
   if (part >= 0) snprintf(node, nn, "/dev/%s%d", best, part); else snprintf(node, nn, "/dev/%s", best);
   l = readlink(path, link, sizeof(link) - 1);
   link[l > 0 ? l : 0] = 0;
   snprintf(id, idn, "%.150s:%s", link, node);
   return 1;
}

static void dev_event(int ev) { s_events |= ev; }   /* s_mx held */

static int watcher_main(void *arg)
{
   (void)arg;
   while (!s_quit)
   {
      char id[200] = "", node[80] = "", mnt[300], fs[16] = "";
      int found = dev_probe(id, sizeof(id), node, sizeof(node)), gone = 0, ours = 0, arrive = 0, i;
      SDL_LockMutex(s_mx);
      if (s_dev.state != DEV_NONE && (!found || strcmp(id, s_dev.id)))
      {
         gone = s_dev.state == DEV_READY; ours = s_dev.ours;
         snprintf(mnt, sizeof(mnt), "%s", s_dev.mnt);
         if (gone) { s_lost = 1; dev_event(EV_GONE); }
         memset(&s_dev, 0, sizeof(s_dev));
      }
      if (s_dev.state == DEV_NONE && found)
      {
         s_dev.state = DEV_MOUNTING;
         snprintf(s_dev.id, sizeof(s_dev.id), "%s", id);
         snprintf(s_dev.node, sizeof(s_dev.node), "%s", node);
         arrive = 1;
      }
      SDL_UnlockMutex(s_mx);
      if (gone)
      {
         logf_("transfer: second drive gone");
         unlink(SEEN);
         if (ours && !s_sim[0]) { SDL_LockMutex(s_op_mx); umount2(mnt, MNT_DETACH); SDL_UnlockMutex(s_op_mx); }
      }
      if (arrive)
      {
         int ok = 0, fuse = 0, mine = 0;
         char opts[256] = "";
         if (s_sim[0])
         {
            const char *e = getenv("LUMEN_USB2_FS");
            snprintf(mnt, sizeof(mnt), "%s", s_sim); snprintf(fs, sizeof(fs), "%s", e ? e : "vfat"); ok = 1;
         }
         else
         {
            SDL_Delay(700);   /* let the kernel finish reading the partition table */
            SDL_LockMutex(s_op_mx);
            if (mount_find(node, 0, mnt, sizeof(mnt), fs, sizeof(fs), opts, sizeof(opts)))
            {  /* someone mounted it already (or a previous Lumen left it): use it as it is */
               ok = 1; fuse = !strncmp(fs, "fuse", 4); mine = !strcmp(mnt, MNT);
            }
            else if (dev_mount(node, 0, fs, sizeof(fs), &fuse) == 0) { ok = 1; mine = 1; snprintf(mnt, sizeof(mnt), "%s", MNT); snprintf(opts, sizeof(opts), "ro"); }
            SDL_UnlockMutex(s_op_mx);
         }
         SDL_LockMutex(s_mx);
         if (s_dev.state == DEV_MOUNTING && !strcmp(s_dev.id, id))
         {
            s_dev.state = ok ? DEV_READY : DEV_UNREADABLE;
            s_dev.ours = mine; s_dev.fuse = fuse;
            s_dev.rw = s_sim[0] ? 1 : !strncmp(opts, "rw", 2);
            snprintf(s_dev.mnt, sizeof(s_dev.mnt), "%s", mnt);
            snprintf(s_dev.fs, sizeof(s_dev.fs), "%s", fs);
            s_lost = 0;
            dev_event(ok ? EV_ARRIVED : EV_UNREADABLE);
         }
         SDL_UnlockMutex(s_mx);
         if (!ok) logf_("transfer: cannot mount %s", node);
      }
      for (i = 0; i < 10 && !s_quit; i++) SDL_Delay(100);
   }
   return 0;
}

/* worker: the second drive writable (1) or read-only again (0). 0 = done */
static int dev_set_rw(int rw)
{
   char node[80], mnt[300], fs[16], opts[256] = "";
   int fuse, cur, ok = 0;
   if (s_sim[0]) return 0;
   SDL_LockMutex(s_mx);
   cur = s_dev.rw; fuse = s_dev.fuse;
   snprintf(node, sizeof(node), "%s", s_dev.node); snprintf(mnt, sizeof(mnt), "%s", s_dev.mnt);
   ok = s_dev.state == DEV_READY;
   SDL_UnlockMutex(s_mx);
   if (!ok) return -1;
   if (cur == rw) return 0;
   if (fuse && !rw) return 0;   /* a FUSE mount cannot be flipped: it stays writable until ejected */
   SDL_LockMutex(s_op_mx);
   if (!fuse)
   {
      char o[32];
      char *argv[] = { "mount", "-o", o, mnt, NULL };
      snprintf(o, sizeof(o), "remount,%s", rw ? "rw" : "ro");
      run_tool("mount", argv);
   }
   else if (!strcmp(mnt, MNT))
   {  /* the helper opened the device read-only: mount it again */
      int f2 = 0;
      dev_unmount(mnt);
      if (dev_mount(node, 1, fs, sizeof(fs), &f2) != 0) dev_mount(node, 0, fs, sizeof(fs), &f2);
   }
   ok = mount_find(mnt, 1, NULL, 0, NULL, 0, opts, sizeof(opts)) && (!strncmp(opts, "rw", 2)) == rw;
   SDL_UnlockMutex(s_op_mx);
   SDL_LockMutex(s_mx);
   if (s_dev.state == DEV_READY) s_dev.rw = !strncmp(opts, "rw", 2);
   SDL_UnlockMutex(s_mx);
   logf_("transfer: second drive %s%s", rw ? "writable" : "read-only", ok ? "" : " FAILED");
   return ok ? 0 : -1;
}

static void dev_eject(void)
{
   char mnt[300];
   int ours, ready;
   SDL_LockMutex(s_mx);
   ready = s_dev.state == DEV_READY; ours = s_dev.ours;
   snprintf(mnt, sizeof(mnt), "%s", s_dev.mnt);
   SDL_UnlockMutex(s_mx);
   if (!ready) return;
   if (!s_sim[0])
   {
      SDL_LockMutex(s_op_mx);
      if (ours || mnt[0]) dev_unmount(mnt);
      SDL_UnlockMutex(s_op_mx);
   }
   SDL_LockMutex(s_mx);
   if (s_dev.state == DEV_READY) { s_dev.state = DEV_EJECTED; s_dev.rw = 0; dev_event(EV_EJECTED); }
   SDL_UnlockMutex(s_mx);
   logf_("transfer: second drive ejected");
}

/* ------------------------------------------------------------------------ */
/* worker: listings and the copy                                             */
enum { C_OK, C_STOPPED, C_SPACE, C_LOST, C_READ, C_WRITE, C_BIG, C_RO };
enum { PH_COUNT, PH_COPY, PH_FINISH };

static struct { int pending; unsigned id; char dir[PATH_LEN], select[NAME_LEN]; } s_lreq[2];
static struct { int ready, ok, n; unsigned id; long long free_b; char dir[PATH_LEN], select[NAME_LEN]; ent_t ent[MAX_ENT]; } s_lres[2];

static struct {
   int  pending, n, dst_side, del;   /* del: delete names from src (dst_side = that side) */
   char src[PATH_LEN], dst[PATH_LEN];
   char names[MAX_SEL][NAME_LEN];
} s_creq;
static struct {   /* progress and result of the copy, s_mx */
   int running, done, phase, status;
   int files, files_done, skipped;
   long long bytes, bytes_done, need, avail;
   char cur[NAME_LEN];
} s_cp;
static volatile int s_stop;
static int s_eject_req;

static int junk(const char *name)
{
   size_t n = strlen(name);
   return !strcmp(name, ".") || !strcmp(name, "..") || !strncmp(name, "._", 2) || !strcmp(name, ".DS_Store") ||
          !strcmp(name, ".Spotlight-V100") || !strcmp(name, ".fseventsd") || !strcmp(name, ".Trashes") ||
          !strcasecmp(name, "System Volume Information") || !strcasecmp(name, "$RECYCLE.BIN") ||
          !strcmp(name, "lost+found") || !strncasecmp(name, "FOUND.", 6) ||
          (n > sizeof(PART) - 1 && !strcmp(name + n - (sizeof(PART) - 1), PART));
}

static int hidden(const char *name) { return name[0] == '.' || name[0] == '$' || junk(name); }

static int cmp_ent(const void *a, const void *b)
{
   const ent_t *x = a, *y = b;
   int c;
   if (x->dir != y->dir) return y->dir - x->dir;
   return (c = strcasecmp(x->name, y->name)) ? c : strcmp(x->name, y->name);
}

static int join(char *out, size_t n, const char *dir, const char *name)
{
   return snprintf(out, n, "%s/%s", strcmp(dir, "/") ? dir : "", name) < (int)n;
}

static long long free_bytes(const char *dir)
{
   struct statvfs sv;
   if (statvfs(dir, &sv) != 0) return -1;
   return (long long)sv.f_bavail * (long long)sv.f_frsize;
}

static void do_list(int p)
{
   static ent_t ent[MAX_ENT];
   char dir[PATH_LEN], sel[NAME_LEN], path[PATH_LEN];
   unsigned id;
   DIR *d;
   struct dirent *e;
   struct stat st;
   int n = 0, ok = 0;
   long long fb;
   SDL_LockMutex(s_mx);
   id = s_lreq[p].id; s_lreq[p].pending = 0;
   snprintf(dir, sizeof(dir), "%s", s_lreq[p].dir); snprintf(sel, sizeof(sel), "%s", s_lreq[p].select);
   SDL_UnlockMutex(s_mx);
   if ((d = opendir(dir)))
   {
      ok = 1;
      while ((e = readdir(d)) && n < MAX_ENT)
      {
         int is_dir;
         if (hidden(e->d_name) || strlen(e->d_name) >= NAME_LEN) continue;
         if (e->d_type == DT_DIR) is_dir = 1;
         else if (e->d_type == DT_REG) is_dir = 0;
         else
         {
            if (!join(path, sizeof(path), dir, e->d_name) || stat(path, &st) != 0) continue;
            if (S_ISDIR(st.st_mode)) is_dir = 1; else if (S_ISREG(st.st_mode)) is_dir = 0; else continue;
         }
         snprintf(ent[n].name, NAME_LEN, "%s", e->d_name);
         ent[n].dir = (unsigned char)is_dir; ent[n++].mark = 0;
      }
      closedir(d);
      qsort(ent, (size_t)n, sizeof(ent_t), cmp_ent);
   }
   fb = ok ? free_bytes(dir) : -1;
   SDL_LockMutex(s_mx);
   s_lres[p].ready = 1; s_lres[p].ok = ok; s_lres[p].n = n; s_lres[p].id = id; s_lres[p].free_b = fb;
   snprintf(s_lres[p].dir, PATH_LEN, "%s", dir); snprintf(s_lres[p].select, NAME_LEN, "%s", sel);
   memcpy(s_lres[p].ent, ent, sizeof(ent_t) * (size_t)n);
   SDL_UnlockMutex(s_mx);
}

/* ---- the copy. One walk, twice: pass 0 counts what has to be written, pass 1 writes */
typedef struct {
   int pass, status, fat, other_is_src;
   int files, files_done, skipped;
   long long bytes, bytes_done;
   double t0;
   char err_name[NAME_LEN];
} walk_t;

static int cp_fail(walk_t *w, int status, const char *name)
{
   if (w->status == C_OK) { w->status = status; snprintf(w->err_name, sizeof(w->err_name), "%s", name); }
   return -1;
}

/* an I/O error: was it the drive going away? */
static int io_status(walk_t *w, int fallback)
{
   char id[200], node[80];
   (void)w;
   if (s_lost || !dev_probe(id, sizeof(id), node, sizeof(node))) return C_LOST;
   if (errno == ENOSPC) return C_SPACE;
   if (errno == EROFS) return C_RO;
   return fallback;
}

static int cp_file(walk_t *w, const char *src, const char *dst, const char *name, const struct stat *st)
{
   static char buf[256 * 1024];
   char tmp[PATH_LEN];
   size_t chunk = s_kbps > 0 ? 32 * 1024 : sizeof(buf);
   long long unsynced = 0;
   int in, out, rc = 0;
   if (snprintf(tmp, sizeof(tmp), "%s" PART, dst) >= (int)sizeof(tmp)) return cp_fail(w, C_WRITE, name);
   SDL_LockMutex(s_mx);
   snprintf(s_cp.cur, sizeof(s_cp.cur), "%s", name);
   SDL_UnlockMutex(s_mx);
   if ((in = open(src, O_RDONLY | O_CLOEXEC)) < 0) return cp_fail(w, io_status(w, C_READ), name);
   if ((out = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644)) < 0)
   { int s = io_status(w, C_WRITE); close(in); return cp_fail(w, s, name); }
   posix_fadvise(in, 0, 0, POSIX_FADV_SEQUENTIAL);
   for (;;)
   {
      ssize_t n, o = 0;
      if (s_stop) { rc = cp_fail(w, C_STOPPED, name); break; }
      if (s_lost) { rc = cp_fail(w, C_LOST, name); break; }
      n = read(in, buf, chunk);
      if (n < 0 && errno == EINTR) continue;
      if (n < 0) { rc = cp_fail(w, io_status(w, C_READ), name); break; }
      if (n == 0) break;
      while (o < n)
      {
         ssize_t k = write(out, buf + o, (size_t)(n - o));
         if (k < 0 && errno == EINTR) continue;
         if (k < 0) { rc = cp_fail(w, io_status(w, C_WRITE), name); break; }
         o += k;
      }
      if (rc) break;
      /* flushed every 4 MB: the bar shows what is on the drive, not what is in the cache */
      if ((unsynced += n) >= 4 * 1024 * 1024) { if (fdatasync(out) != 0 && errno == EIO) { rc = cp_fail(w, io_status(w, C_WRITE), name); break; } unsynced = 0; }
      w->bytes_done += n;
      SDL_LockMutex(s_mx);
      s_cp.bytes_done = w->bytes_done;
      SDL_UnlockMutex(s_mx);
      if (s_kbps > 0) SDL_Delay((Uint32)(n * 1000 / 1024 / s_kbps));
   }
   close(in);
   if (!rc && fsync(out) != 0 && errno != EINVAL && errno != ENOSYS) rc = cp_fail(w, io_status(w, C_WRITE), name);
   if (!rc)
   {
      struct timespec ts[2] = { st->st_atim, st->st_mtim };
      fchmod(out, (st->st_mode & 0777) | 0644);   /* FAT ignores it */
      futimens(out, ts);
   }
   if (close(out) != 0 && !rc) rc = cp_fail(w, io_status(w, C_WRITE), name);
   if (!rc && rename(tmp, dst) != 0) rc = cp_fail(w, io_status(w, C_WRITE), name);
   if (rc) unlink(tmp);
   return rc;
}

static int cp_walk(walk_t *w, const char *src, const char *dst, const char *name, int depth)
{
   struct stat st, dt;
   if (s_stop) return cp_fail(w, C_STOPPED, name);
   if (s_lost) return cp_fail(w, C_LOST, name);
   if (lstat(src, &st) != 0) return cp_fail(w, io_status(w, C_READ), name);
   if (S_ISDIR(st.st_mode))
   {
      DIR *d;
      struct dirent *e;
      int rc = 0;
      if (depth > 24) return 0;
      if (w->pass == 1 && mkdir(dst, 0755) != 0 && errno != EEXIST) return cp_fail(w, io_status(w, C_WRITE), name);
      if (!(d = opendir(src))) return cp_fail(w, io_status(w, C_READ), name);
      while (!rc)
      {
         char s2[PATH_LEN], d2[PATH_LEN];
         errno = 0;
         if (!(e = readdir(d))) { if (errno == EIO || errno == ENODEV) rc = cp_fail(w, io_status(w, C_READ), name); break; }
         if (junk(e->d_name)) continue;
         if (!join(s2, sizeof(s2), src, e->d_name) || !join(d2, sizeof(d2), dst, e->d_name)) continue;
         rc = cp_walk(w, s2, d2, e->d_name, depth + 1);
      }
      closedir(d);
      return rc;
   }
   if (!S_ISREG(st.st_mode)) return 0;   /* links, devices: not copied */
   if (stat(dst, &dt) == 0 && S_ISREG(dt.st_mode) && dt.st_size == st.st_size)
   {  /* already there */
      if (w->pass == 0) w->skipped++;
      return 0;
   }
   if (w->pass == 0)
   {
      if (w->fat && st.st_size >= 0xFFFFFFFFLL) return cp_fail(w, C_BIG, name);
      w->files++; w->bytes += st.st_size;
      return 0;
   }
   if (cp_file(w, src, dst, name, &st) != 0) return -1;
   w->files_done++;
   SDL_LockMutex(s_mx);
   s_cp.files_done = w->files_done;
   SDL_UnlockMutex(s_mx);
   return 0;
}

/* dst and every missing folder above it */
static int mkdirs(const char *dst)
{
   char p[PATH_LEN], *s;
   struct stat st;
   snprintf(p, sizeof(p), "%s", dst);
   for (s = p + 1; *s; s++)
      if (*s == '/') { *s = 0; if (stat(p, &st) != 0) mkdir(p, 0755); *s = '/'; }
   return (mkdir(p, 0755) == 0 || errno == EEXIST) ? 0 : -1;
}

/* everything under path, then path itself; counts the files removed */
static int rm_tree(const char *path, int depth, int *count)
{
   struct stat st;
   if (s_lost) return -1;
   if (lstat(path, &st) != 0) return errno == ENOENT ? 0 : -1;
   if (S_ISDIR(st.st_mode))
   {
      DIR *d;
      struct dirent *e;
      int rc = 0;
      if (depth > 32 || !(d = opendir(path))) return -1;
      while (!rc && (e = readdir(d)))
      {
         char p[PATH_LEN];
         if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
         rc = join(p, sizeof(p), path, e->d_name) ? rm_tree(p, depth + 1, count) : -1;
      }
      closedir(d);
      return rc ? rc : rmdir(path);
   }
   if (unlink(path) != 0) return -1;
   (*count)++;
   return 0;
}

static void do_delete(const char *dir, char names[][NAME_LEN], int n, int side)
{
   char p[PATH_LEN];
   walk_t w;
   int i, fd, count = 0;
   memset(&w, 0, sizeof(w));
   logf_("transfer: delete %d item(s) in %s", n, dir);
   if (side == SIDE_OTHER && dev_set_rw(1) != 0) cp_fail(&w, s_lost ? C_LOST : C_RO, "");
   for (i = 0; i < n && w.status == C_OK; i++)
      if (join(p, sizeof(p), dir, names[i]) && rm_tree(p, 0, &count) != 0) cp_fail(&w, io_status(&w, C_WRITE), names[i]);
   if (w.status != C_LOST)
   {
      if ((fd = open(dir, O_RDONLY | O_CLOEXEC)) >= 0) { if (syncfs(fd) != 0) sync(); close(fd); }
      if (side == SIDE_OTHER) dev_set_rw(0);
   }
   logf_("transfer: delete ended status %d: %d file(s) removed%s%s", w.status, count, w.err_name[0] ? ", at " : "", w.err_name);
   SDL_LockMutex(s_mx);
   s_cp.running = 0; s_cp.done = 1; s_cp.status = w.status; s_cp.files_done = count;
   snprintf(s_cp.cur, sizeof(s_cp.cur), "%s", w.err_name);
   SDL_UnlockMutex(s_mx);
}

static void do_copy(void)
{
   static char names[MAX_SEL][NAME_LEN];
   char src[PATH_LEN], dst[PATH_LEN], s2[PATH_LEN], d2[PATH_LEN], fs[16] = "";
   walk_t w;
   int n, i, dst_side, fd, del;
   long long avail = 0, need = 0;
   memset(&w, 0, sizeof(w));
   SDL_LockMutex(s_mx);
   n = s_creq.n; dst_side = s_creq.dst_side; del = s_creq.del; s_creq.pending = 0;
   snprintf(src, sizeof(src), "%s", s_creq.src); snprintf(dst, sizeof(dst), "%s", s_creq.dst);
   memcpy(names, s_creq.names, sizeof(names[0]) * (size_t)n);
   snprintf(fs, sizeof(fs), "%s", dst_side == SIDE_OTHER ? s_dev.fs : s_root_fs);
   SDL_UnlockMutex(s_mx);
   if (del) { do_delete(src, names, n, dst_side); return; }
   w.fat = !strcmp(fs, "vfat") || !strcmp(fs, "msdos");
   logf_("transfer: copy %d item(s) from %s to %s", n, src, dst);

   if (dst_side == SIDE_OTHER && dev_set_rw(1) != 0) cp_fail(&w, s_lost ? C_LOST : C_RO, "");
   for (i = 0; i < n && w.status == C_OK; i++)   /* pass 0: what and how much */
      if (join(s2, sizeof(s2), src, names[i]) && join(d2, sizeof(d2), dst, names[i])) cp_walk(&w, s2, d2, names[i], 0);
   if (w.status == C_OK && w.files)
   {
      if (mkdirs(dst) != 0) cp_fail(&w, io_status(&w, C_WRITE), "");
      else
      {  /* room for the data, one block of slack per file, and a margin for the folders */
         struct statvfs sv;
         if (statvfs(dst, &sv) == 0)
         {
            avail = (long long)sv.f_bavail * (long long)sv.f_frsize;
            if (s_free_mb >= 0) avail = s_free_mb * 1024LL * 1024;   /* test hook */
            need = w.bytes + (long long)w.files * (long long)sv.f_frsize + 8LL * 1024 * 1024;
            if (need > avail) cp_fail(&w, C_SPACE, "");
         }
      }
   }
   SDL_LockMutex(s_mx);
   s_cp.phase = PH_COPY; s_cp.files = w.files; s_cp.bytes = w.bytes; s_cp.skipped = w.skipped;
   s_cp.need = need; s_cp.avail = avail;
   SDL_UnlockMutex(s_mx);
   w.pass = 1; w.t0 = now_s();
   for (i = 0; i < n && w.status == C_OK && w.files; i++)
      if (join(s2, sizeof(s2), src, names[i]) && join(d2, sizeof(d2), dst, names[i])) cp_walk(&w, s2, d2, names[i], 0);

   SDL_LockMutex(s_mx);
   s_cp.phase = PH_FINISH;
   SDL_UnlockMutex(s_mx);
   if (w.status != C_LOST || dst_side == SIDE_LUMEN)
   {  /* renames and folders on the drive before anyone is told it is done */
      if ((fd = open(dst, O_RDONLY | O_CLOEXEC)) >= 0) { if (syncfs(fd) != 0) sync(); close(fd); }
      else if (w.files_done) sync();
   }
   if (dst_side == SIDE_OTHER && w.status != C_LOST) dev_set_rw(0);
   logf_("transfer: copy ended status %d: %d of %d file(s), %lld of %lld bytes, %d skipped, %.1f s%s%s", w.status,
         w.files_done, w.files, w.bytes_done, w.bytes, w.skipped, w.t0 > 0 ? now_s() - w.t0 : 0.0,
         w.err_name[0] ? ", at " : "", w.err_name);
   SDL_LockMutex(s_mx);
   s_cp.running = 0; s_cp.done = 1; s_cp.status = w.status;
   s_cp.files_done = w.files_done; s_cp.bytes_done = w.bytes_done; s_cp.skipped = w.skipped;
   snprintf(s_cp.cur, sizeof(s_cp.cur), "%s", w.err_name);
   SDL_UnlockMutex(s_mx);
}

static int worker_main(void *arg)
{
   (void)arg;
   SDL_LockMutex(s_mx);
   while (!s_quit)
   {
      int job = 0, p = 0;
      if (s_eject_req) { s_eject_req = 0; job = 1; }
      else if (s_creq.pending) job = 2;
      else for (p = 0; p < 2; p++) if (s_lreq[p].pending && !s_lres[p].ready) { job = 3; break; }
      if (!job) { SDL_CondWaitTimeout(s_cv, s_mx, 200); continue; }
      SDL_UnlockMutex(s_mx);
      if (job == 1) dev_eject(); else if (job == 2) do_copy(); else do_list(p);
      SDL_LockMutex(s_mx);
   }
   SDL_UnlockMutex(s_mx);
   return 0;
}

/* ------------------------------------------------------------------------ */
/* UI state                                                                  */
enum { V_BROWSE, V_DEST, V_COPY, V_STOP, V_RESULT, V_EJECT, V_MENU, V_DELQ, V_DEL };

typedef struct {
   int     side;
   char    root[300], dir[PATH_LEN];
   ent_t   ent[MAX_ENT];
   int     n, sel, top, nmark, loading, gen;
   unsigned want;
   double  load_t;
   long long free_b;
   spring  pill, scroll;
   label_t name, info, path, count, rl[POOL];
   int     slot_ent[POOL], slot_gen[POOL];
   char    path_txt[500];
} pane_t;

static pane_t  s_pane[2];
static int     s_act;                      /* pane with the focus */
static int     s_open, s_view, s_btn, s_msel, s_del;   /* s_del: the running job deletes */
static unsigned s_req_id;
static double  s_t0, s_offer_t = -100.0, s_copy_t0;
static int     s_offer_wait, s_home;
static float   s_dt = 1.0f / 60.0f;
static spring  s_fade, s_sub, s_dpill, s_dscroll;
static int     s_dev_state;                /* this frame's copy of s_dev */
static char    s_dev_mnt[300], s_dev_fs[16];

/* destination list */
static struct { char label[64], path[PATH_LEN], shown[200]; } s_dest[MAX_DEST];
static int     s_ndest, s_dsel, s_dtop, s_dgen;
static int     s_nsel;                     /* how many entries the copy takes */
static char    s_dst_txt[300];             /* "Lumen USB / games / psx" */

/* result of the last copy */
static struct { int status, files, files_done, skipped, dst_side; long long bytes_done, need, avail; char name[NAME_LEN]; } s_res;

static label_t s_kick, s_title, s_selc, s_empty, s_busy_l;
static label_t s_sk, s_st, s_l1, s_l2, s_l3, s_l4, s_b[2], s_offer;
static label_t s_dl[POOL], s_dv[POOL];
static int     s_dslot[POOL], s_dslot_gen[POOL];

static const char *side_name(int side) { return tr(side == SIDE_LUMEN ? "xfer.lumen" : "xfer.other"); }

static void size_text(char *out, size_t n, long long b)
{
   char num[32];
   if (b < 0) { out[0] = 0; return; }
   if (b < 1024LL * 1024)
   { snprintf(num, sizeof(num), "%lld", (b + 1023) / 1024); trf(out, n, "xfer.kb", "n", num, NULL); }
   else if (b < 1024LL * 1024 * 1024)
   { snprintf(num, sizeof(num), "%lld", (b + 512 * 1024) / (1024 * 1024)); trf(out, n, "about.mb", "n", num, NULL); }
   else
   {
      long long tenths = (long long)((double)b / (1024.0 * 1024.0 * 1024.0) * 10.0 + 0.5);
      snprintf(num, sizeof(num), "%lld%s%lld", tenths / 10, tr("num.decimal"), tenths % 10);
      trf(out, n, "about.gb", "n", num, NULL);
   }
}

/* "Second USB / games / psx" for dir on that side */
static void path_text(char *out, size_t n, int side, const char *root, const char *dir)
{
   const char *p = dir + (strncmp(dir, root, strlen(root)) ? 0 : strlen(root));
   size_t o = (size_t)snprintf(out, n, "%s", side_name(side));
   if (o >= n) return;
   while (*p && o + 8 < n)
   {
      if (*p == '/') { o += (size_t)snprintf(out + o, n - o, "  /  "); p++; }
      else out[o++] = *p++;
      out[o] = 0;
   }
}

static void pane_request(int p, const char *dir, const char *select, double t)
{
   pane_t *pn = &s_pane[p];
   SDL_LockMutex(s_mx);
   s_lreq[p].pending = 1; s_lreq[p].id = pn->want = ++s_req_id;
   snprintf(s_lreq[p].dir, PATH_LEN, "%s", dir);
   snprintf(s_lreq[p].select, NAME_LEN, "%s", select ? select : "");
   SDL_CondSignal(s_cv);
   SDL_UnlockMutex(s_mx);
   pn->loading = 1; pn->load_t = t;
}

static void pane_reload(int p, double t)
{
   pane_t *pn = &s_pane[p];
   pane_request(p, pn->dir, pn->n ? pn->ent[pn->sel].name : NULL, t);
}

static int pane_at_root(const pane_t *pn) { return !strcmp(pn->dir, pn->root); }

static void pane_up(int p, double t)
{
   pane_t *pn = &s_pane[p];
   char parent[PATH_LEN], from[NAME_LEN], *slash;
   if (pane_at_root(pn) || pn->loading) return;
   snprintf(parent, sizeof(parent), "%s", pn->dir);
   if (!(slash = strrchr(parent, '/'))) return;
   snprintf(from, sizeof(from), "%s", slash + 1);
   *slash = 0;
   if (strlen(parent) < strlen(pn->root)) snprintf(parent, sizeof(parent), "%s", pn->root);
   pane_request(p, parent, from, t);
}

static void pane_enter(int p, double t)
{
   pane_t *pn = &s_pane[p];
   char path[PATH_LEN];
   if (pn->loading || !pn->n) return;
   if (join(path, sizeof(path), pn->dir, pn->ent[pn->sel].name)) pane_request(p, path, NULL, t);
}

static void pane_mark(pane_t *pn)
{
   ent_t *e;
   if (!pn->n) return;
   e = &pn->ent[pn->sel];
   if (!e->mark && pn->nmark >= MAX_SEL) return;
   e->mark = !e->mark;
   pn->nmark += e->mark ? 1 : -1;
}

static void pane_init(int p, int side, const char *root, const char *dir, double t)
{
   pane_t *pn = &s_pane[p];
   int i;
   pn->side = side; pn->n = pn->sel = pn->top = pn->nmark = 0; pn->free_b = -1;
   snprintf(pn->root, sizeof(pn->root), "%s", root);
   snprintf(pn->dir, sizeof(pn->dir), "%s", dir);
   path_text(pn->path_txt, sizeof(pn->path_txt), side, pn->root, pn->dir);
   pn->pill.x = pn->pill.v = pn->scroll.x = pn->scroll.v = 0;
   for (i = 0; i < POOL; i++) pn->slot_ent[i] = -1;
   pane_request(p, dir, NULL, t);
}

/* listings the worker finished */
static void collect_lists(void)
{
   int p, i;
   for (p = 0; p < 2; p++)
   {
      pane_t *pn = &s_pane[p];
      int ready, ok = 0, mine = 0;
      char select[NAME_LEN] = "";
      SDL_LockMutex(s_mx);
      ready = s_lres[p].ready;
      if (ready)
      {
         ok = s_lres[p].ok; mine = s_lres[p].id == pn->want;
         if (ok && mine)
         {
            pn->n = s_lres[p].n; pn->free_b = s_lres[p].free_b;
            memcpy(pn->ent, s_lres[p].ent, sizeof(ent_t) * (size_t)pn->n);
            snprintf(pn->dir, sizeof(pn->dir), "%s", s_lres[p].dir);
            snprintf(select, sizeof(select), "%s", s_lres[p].select);
         }
         s_lres[p].ready = 0;
         SDL_CondSignal(s_cv);
      }
      SDL_UnlockMutex(s_mx);
      if (!ready || !mine) continue;
      pn->loading = 0;
      if (!ok) { if (s_open) panel_toast("fp.unreadable"); continue; }
      pn->sel = 0; pn->nmark = 0;
      for (i = 0; select[0] && i < pn->n; i++) if (!strcmp(pn->ent[i].name, select)) { pn->sel = i; break; }
      pn->top = pn->sel - VIS / 2;
      if (pn->top > pn->n - VIS) pn->top = pn->n - VIS;
      if (pn->top < 0) pn->top = 0;
      pn->scroll.x = pn->top * ROW; pn->scroll.v = 0;
      pn->pill.x = pn->sel * ROW; pn->pill.v = 0;
      pn->gen++;
      path_text(pn->path_txt, sizeof(pn->path_txt), pn->side, pn->root, pn->dir);
   }
}

static void close_card(void) { s_open = 0; s_view = V_BROWSE; }

void transfer_open(void)
{
   char games[PATH_LEN];
   struct stat st;
   double t = now_s();
   if (s_open) return;
   if (!s_threads || s_dev_state != DEV_READY) { panel_toast(s_dev_state == DEV_UNREADABLE ? "xfer.unreadable" : "xfer.none"); return; }
   snprintf(games, sizeof(games), "%s/games", g_root);
   pane_init(0, SIDE_OTHER, s_dev_mnt, s_dev_mnt, t);
   pane_init(1, SIDE_LUMEN, g_root, stat(games, &st) == 0 && S_ISDIR(st.st_mode) ? games : g_root, t);
   s_act = 0; s_open = 1; s_view = V_BROWSE; s_t0 = t; s_offer_t = -100.0;
   if (s_fade.x < 0.02f) { s_fade.x = 0; s_fade.v = 0; }
   s_sub.x = s_sub.v = 0;
   music_sfx(SFX_OPEN);
   logf_("transfer: open (%s, %s)", s_dev_mnt, s_dev_fs);
}

int   transfer_active(void) { return s_open || s_fade.x > 0.01f; }
int   transfer_open_now(void) { return s_open; }
float transfer_amount(void) { return clampf(s_fade.x, 0.0f, 1.0f); }

int transfer_hint_set(void)
{
   if (s_view == V_BROWSE || s_view == V_EJECT || s_view == V_DEL) return HINTS_TRANSFER;
   if (s_view == V_COPY) return HINTS_TRANSFER_COPY;
   return HINTS_CONFIRM;
}

/* ---- the systems declared on the stick (<home>/systems/<id>.ini) and their game folder.
 * Read here and not taken from g_plat: a system without games yet is not loaded, and that
 * is exactly the one a first copy is for. Read once, at start. */
typedef struct { char name[48], path[300]; int order; } sys_t;
static sys_t s_sys[MAX_PLATFORMS];
static int   s_nsys;

static int cmp_sys(const void *a, const void *b)
{
   const sys_t *x = a, *y = b;
   return x->order != y->order ? x->order - y->order : strcmp(x->name, y->name);
}

static void systems_load(void)
{
   char dir[400], path[700], line[700], sec[32];
   DIR *d;
   struct dirent *e;
   size_t rl = strlen(g_root);
   snprintf(dir, sizeof(dir), "%s/systems", g_home);
   if (!(d = opendir(dir))) return;
   while ((e = readdir(d)) && s_nsys < MAX_PLATFORMS)
   {
      char id[32] = "", name[48] = "", paths[300] = "", layout[16] = "";
      size_t l = strlen(e->d_name);
      int order = 1000;
      FILE *f;
      if (l < 5 || strcmp(e->d_name + l - 4, ".ini") || e->d_name[0] == '.') continue;
      snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
      if (!(f = fopen(path, "r"))) continue;
      sec[0] = 0;
      while (fgets(line, sizeof(line), f))
      {
         char *k = line, *v, *end;
         while (*k == ' ' || *k == '\t') k++;
         if (*k == '[') { if ((end = strchr(k, ']'))) { *end = 0; snprintf(sec, sizeof(sec), "%s", k + 1); } continue; }
         if (!(v = strchr(k, '=')) || *k == ';' || *k == '#') continue;
         for (end = v; end > k && (end[-1] == ' ' || end[-1] == '\t'); ) end--;
         *end = 0; v++;
         while (*v == ' ' || *v == '\t') v++;
         if ((end = strchr(v, ';')) && strcmp(k, "paths")) *end = 0;   /* comment; paths are ';'-separated */
         for (end = v + strlen(v); end > v && strchr(" \t\r\n", end[-1]); ) end--;
         *end = 0;
         if (!strcmp(sec, "system") && !strcmp(k, "id")) snprintf(id, sizeof(id), "%s", v);
         else if (!strcmp(sec, "system") && !strcmp(k, "name")) snprintf(name, sizeof(name), "%s", v);
         else if (!strcmp(sec, "system") && !strcmp(k, "order")) order = atoi(v);
         else if (!strcmp(sec, "library") && !strcmp(k, "layout")) snprintf(layout, sizeof(layout), "%s", v);
         else if (!strcmp(sec, "library") && !strcmp(k, "paths"))
         {  /* the first folder; a trailing "; comment" is not one (it has spaces around it) */
            if ((end = strchr(v, ';'))) *end = 0;
            for (end = v + strlen(v); end > v && (end[-1] == ' ' || end[-1] == '/'); ) end--;
            *end = 0;
            snprintf(paths, sizeof(paths), "%s", v);
         }
      }
      fclose(f);
      if (!id[0]) snprintf(id, sizeof(id), "%.*s", (int)(l - 4 < 31 ? l - 4 : 31), e->d_name);
      if (!strcmp(layout, "stock")) continue;   /* the console's own games: nothing is copied there */
      if (!paths[0]) snprintf(paths, sizeof(paths), "%s/games/%s", g_root, id);
      if (strncmp(paths, g_root, rl) || paths[rl] != '/') continue;   /* not on the stick */
      snprintf(s_sys[s_nsys].name, sizeof(s_sys[0].name), "%s", name[0] ? name : id);
      snprintf(s_sys[s_nsys].path, sizeof(s_sys[0].path), "%s", paths);
      s_sys[s_nsys++].order = order;
   }
   closedir(d);
   qsort(s_sys, (size_t)s_nsys, sizeof(s_sys[0]), cmp_sys);
}

/* ---- destinations: the folder open in the other pane, then one per system */
static void dest_build(void)
{
   const pane_t *o = &s_pane[!s_act];
   int i, k;
   s_ndest = 0;
   snprintf(s_dest[0].label, sizeof(s_dest[0].label), "%s", tr("xfer.dest_open"));
   snprintf(s_dest[0].path, PATH_LEN, "%s", o->dir);
   s_ndest = 1;
   for (i = 0; o->side == SIDE_LUMEN && i < s_nsys && s_ndest < MAX_DEST; i++)
   {
      for (k = 1; k < s_ndest; k++) if (!strcmp(s_dest[k].path, s_sys[i].path)) break;
      if (k < s_ndest) continue;
      snprintf(s_dest[s_ndest].label, sizeof(s_dest[0].label), "%s", s_sys[i].name);
      snprintf(s_dest[s_ndest++].path, PATH_LEN, "%s", s_sys[i].path);
   }
   for (i = 0; i < s_ndest; i++)
   {  /* the path below the drive: "games / psx" */
      const char *p = s_dest[i].path + strlen(o->root);
      size_t n = 0;
      char *out = s_dest[i].shown;
      while (*p == '/') p++;
      out[0] = 0;
      for (; *p && n + 6 < sizeof(s_dest[i].shown); p++)
      {
         if (*p == '/') n += (size_t)snprintf(out + n, sizeof(s_dest[i].shown) - n, " / ");
         else { out[n++] = *p; out[n] = 0; }
      }
      if (!out[0]) snprintf(out, sizeof(s_dest[i].shown), "/");
   }
   s_dsel = s_dtop = 0; s_dgen++;
   s_dpill.x = s_dpill.v = s_dscroll.x = s_dscroll.v = 0;
}

static void copy_start(double t)
{
   const pane_t *a = &s_pane[s_act], *o = &s_pane[!s_act];
   int i, n = 0;
   SDL_LockMutex(s_mx);
   for (i = 0; i < a->n && n < MAX_SEL; i++)
      if (a->nmark ? a->ent[i].mark : i == a->sel) snprintf(s_creq.names[n++], NAME_LEN, "%s", a->ent[i].name);
   s_creq.n = n; s_creq.dst_side = o->side; s_creq.del = 0;
   snprintf(s_creq.src, PATH_LEN, "%s", a->dir);
   snprintf(s_creq.dst, PATH_LEN, "%s", s_dest[s_dsel].path);
   memset(&s_cp, 0, sizeof(s_cp));
   s_cp.running = 1; s_cp.phase = PH_COUNT;
   s_stop = 0;
   s_creq.pending = 1;
   SDL_CondSignal(s_cv);
   SDL_UnlockMutex(s_mx);
   path_text(s_dst_txt, sizeof(s_dst_txt), o->side, o->root, s_dest[s_dsel].path);
   s_res.dst_side = o->side; s_del = 0;
   s_view = V_COPY; s_copy_t0 = t;
}

/* the Lumen stick's own folder, and anything that holds it, is not deleted from here */
static int delete_refused(void)
{
   const pane_t *a = &s_pane[s_act];
   size_t hl = strlen(g_home);
   int i;
   if (a->side != SIDE_LUMEN) return 0;
   for (i = 0; i < a->n; i++)
   {
      char p[PATH_LEN];
      size_t pl;
      if (!(a->nmark ? a->ent[i].mark : i == a->sel) || !join(p, sizeof(p), a->dir, a->ent[i].name)) continue;
      pl = strlen(p);
      if (!strncmp(p, g_home, hl) && (p[hl] == 0 || p[hl] == '/')) return 1;      /* in it */
      if (!strncmp(g_home, p, pl) && g_home[pl] == '/') return 1;                   /* around it */
   }
   return 0;
}

static void delete_start(void)
{
   const pane_t *a = &s_pane[s_act];
   int i, n = 0;
   SDL_LockMutex(s_mx);
   for (i = 0; i < a->n && n < MAX_SEL; i++)
      if (a->nmark ? a->ent[i].mark : i == a->sel) snprintf(s_creq.names[n++], NAME_LEN, "%s", a->ent[i].name);
   s_creq.n = n; s_creq.dst_side = a->side; s_creq.del = 1;
   snprintf(s_creq.src, PATH_LEN, "%s", a->dir);
   memset(&s_cp, 0, sizeof(s_cp));
   s_cp.running = 1;
   s_stop = 0;
   s_creq.pending = 1;
   SDL_CondSignal(s_cv);
   SDL_UnlockMutex(s_mx);
   s_res.dst_side = a->side; s_del = 1;
   s_view = V_DEL;
}

static void copy_collect(double t)
{
   int done;
   SDL_LockMutex(s_mx);
   done = s_cp.done;
   if (done)
   {
      s_cp.done = 0;
      s_res.status = s_cp.status; s_res.files = s_cp.files; s_res.files_done = s_cp.files_done;
      s_res.skipped = s_cp.skipped; s_res.bytes_done = s_cp.bytes_done; s_res.need = s_cp.need; s_res.avail = s_cp.avail;
      snprintf(s_res.name, sizeof(s_res.name), "%s", s_cp.cur);
   }
   SDL_UnlockMutex(s_mx);
   if (!done) return;
   if (s_del && s_res.status == C_OK)
   {  /* a delete that went well needs no card: back to the list */
      s_view = V_BROWSE;
      pane_reload(0, t); pane_reload(1, t);
      if (s_res.dst_side == SIDE_LUMEN) ui_library_changed();
      panel_toast("xfer.deleted");
      music_sfx(SFX_CONFIRM);
      return;
   }
   s_view = V_RESULT; s_btn = 0;
   s_sub.x = 0.6f;
   if (s_res.status != C_LOST) { pane_reload(0, t); pane_reload(1, t); }
   if (s_res.dst_side == SIDE_LUMEN && s_res.files_done > 0) ui_library_changed();
   music_sfx(s_res.status == C_OK ? SFX_CONFIRM : SFX_BACK);
}

static void eject_start(void)
{
   SDL_LockMutex(s_mx);
   s_eject_req = 1;
   SDL_CondSignal(s_cv);
   SDL_UnlockMutex(s_mx);
   s_view = V_EJECT;
}

static void browse_input(const ui_nav_t *nav, double t)
{
   pane_t *pn = &s_pane[s_act];
   int before = pn->sel, circle = nav->back && !nav->erase;
   if (nav->options)
   {  /* Triangle: the options (eject, delete what is marked) */
      s_nsel = pn->nmark ? pn->nmark : (pn->n ? 1 : 0);
      s_msel = 0; s_view = V_MENU; s_sub.x = s_sub.v = 0;
      music_sfx(SFX_OPEN);
      return;
   }
   if (circle)
   {
      music_sfx(SFX_BACK);
      if (!pane_at_root(pn)) pane_up(s_act, t); else close_card();
      return;
   }
   if (nav->left && s_act == 1) { s_act = 0; music_sfx(SFX_MOVE_PREV); return; }
   if (nav->right && s_act == 0) { s_act = 1; music_sfx(SFX_MOVE_NEXT); return; }
   if (pn->loading || !pn->n) return;
   if (nav->up && pn->sel > 0) pn->sel--;
   if (nav->down && pn->sel < pn->n - 1) pn->sel++;
   if (nav->l1) pn->sel = pn->sel - VIS < 0 ? 0 : pn->sel - VIS;
   if (nav->r1) pn->sel = pn->sel + VIS > pn->n - 1 ? pn->n - 1 : pn->sel + VIS;
   if (pn->sel != before) { music_sfx(SFX_FOCUS); return; }
   if (nav->erase)
   {  /* Square marks and steps down: a run of games is Square, Square, Square */
      pane_mark(pn); music_sfx(SFX_CONFIRM);
      if (pn->sel < pn->n - 1) pn->sel++;
   }
   else if (nav->confirm)
   {
      music_sfx(SFX_CONFIRM);
      if (pn->ent[pn->sel].dir) pane_enter(s_act, t); else pane_mark(pn);
   }
   else if (nav->start)
   {
      s_nsel = pn->nmark ? pn->nmark : 1;
      dest_build();
      s_view = V_DEST; s_sub.x = s_sub.v = 0;
      music_sfx(SFX_OPEN);
   }
}

void transfer_update(const ui_nav_t *nav, double t, float dt, int home_free)
{
   int ev;
   s_dt = dt > 0.0f ? dt : s_dt;
   s_home = home_free;
   if (!s_threads) return;
   SDL_LockMutex(s_mx);
   ev = s_events; s_events = 0;
   s_dev_state = s_dev.state;
   snprintf(s_dev_mnt, sizeof(s_dev_mnt), "%s", s_dev.mnt);
   snprintf(s_dev_fs, sizeof(s_dev_fs), "%s", s_dev.fs);
   SDL_UnlockMutex(s_mx);

   if (ev & EV_ARRIVED)
   {  /* offered once per insertion: coming back from a game is not a new drive */
      char seen[200] = "", id[200];
      FILE *f = fopen(SEEN, "r");
      SDL_LockMutex(s_mx); snprintf(id, sizeof(id), "%s", s_dev.id); SDL_UnlockMutex(s_mx);
      if (f) { if (fgets(seen, sizeof(seen), f)) seen[strcspn(seen, "\n")] = 0; fclose(f); }
      if (strcmp(seen, id))
      {
         if ((f = fopen(SEEN, "w"))) { fprintf(f, "%s\n", id); fclose(f); }
         s_offer_wait = 1;   /* shown once the Home has the input (not over the welcome or a menu) */
         if (panel_active() && !s_open) panel_toast("xfer.connected");
      }
   }
   if (s_offer_wait && home_free && s_dev_state == DEV_READY) { s_offer_wait = 0; s_offer_t = t; }
   if (s_dev_state != DEV_READY || s_open) s_offer_wait = 0;
   if (ev & EV_UNREADABLE) panel_toast("xfer.unreadable");
   if (ev & EV_EJECTED) { s_offer_t = -100.0; if (s_open) close_card(); panel_toast("xfer.ejected"); }
   if (ev & EV_GONE)
   {
      s_offer_t = -100.0;
      if (!s_open) panel_toast("xfer.removed");
   }

   spring_step(&s_fade, s_open ? 1.0f : 0.0f, 120, 18, dt);
   spring_step(&s_sub, s_open && s_view != V_BROWSE ? 1.0f : 0.0f, 160, 20, dt);
   collect_lists();
   if (!s_open)
   {
      if (s_fade.x <= 0.01f && s_title.tex)
      {
         int p, i;
         label_t *all[] = { &s_kick, &s_title, &s_selc, &s_empty, &s_busy_l, &s_sk, &s_st, &s_l1, &s_l2, &s_l3, &s_l4, &s_b[0], &s_b[1] };
         for (i = 0; i < (int)(sizeof(all) / sizeof(all[0])); i++) label_free(all[i]);
         for (i = 0; i < POOL; i++) { label_free(&s_dl[i]); label_free(&s_dv[i]); s_dslot[i] = -1; }
         for (p = 0; p < 2; p++)
         {
            pane_t *pn = &s_pane[p];
            label_free(&pn->name); label_free(&pn->info); label_free(&pn->path); label_free(&pn->count);
            for (i = 0; i < POOL; i++) { label_free(&pn->rl[i]); pn->slot_ent[i] = -1; }
         }
      }
      /* the offer: Square on the Home while the pill is up */
      if (nav && home_free && nav->erase && t - s_offer_t < OFFER_S && s_dev_state == DEV_READY) transfer_open();
      return;
   }

   if (s_view == V_COPY || s_view == V_STOP || s_view == V_DEL) copy_collect(t);
   /* the drive went away under the card: a copy reports it itself, anything else closes */
   if (s_dev_state != DEV_READY && s_view != V_COPY && s_view != V_STOP && s_view != V_RESULT && s_view != V_EJECT && s_view != V_DEL)
   { close_card(); panel_toast("xfer.removed"); return; }
   if (s_view == V_EJECT && s_dev_state == DEV_NONE) { close_card(); panel_toast("xfer.removed"); return; }

   if (nav && t - s_t0 >= 0.15)
   {
      const int circle = nav->back && !nav->erase;
      switch (s_view)
      {
      case V_BROWSE: browse_input(nav, t); break;
      case V_DEST:
      {
         int before = s_dsel;
         if (circle || nav->options) { s_view = V_BROWSE; music_sfx(SFX_BACK); break; }
         if (nav->up && s_dsel > 0) s_dsel--;
         if (nav->down && s_dsel < s_ndest - 1) s_dsel++;
         if (s_dsel != before) music_sfx(SFX_FOCUS);
         else if (nav->confirm) { music_sfx(SFX_CONFIRM); copy_start(t); }
         if (s_dsel < s_dtop) s_dtop = s_dsel;
         if (s_dsel > s_dtop + DVIS - 1) s_dtop = s_dsel - DVIS + 1;
         break;
      }
      case V_COPY:
         if (circle) { s_view = V_STOP; s_btn = 0; music_sfx(SFX_OPEN); }
         break;
      case V_MENU:
         if (circle || nav->options) { s_view = V_BROWSE; music_sfx(SFX_BACK); break; }
         if (nav->up && s_msel) { s_msel = 0; music_sfx(SFX_FOCUS); }
         if (nav->down && !s_msel && s_nsel) { s_msel = 1; music_sfx(SFX_FOCUS); }
         if (nav->confirm)
         {
            music_sfx(SFX_CONFIRM);
            if (!s_msel) eject_start();
            else if (delete_refused()) { s_view = V_BROWSE; panel_toast("xfer.del_protected"); }
            else { s_view = V_DELQ; s_btn = 0; }
         }
         break;
      case V_DELQ:   /* Cancel has the focus; the destructive answer is never the default */
         if (nav->left && s_btn) { s_btn = 0; music_sfx(SFX_MOVE_PREV); }
         if (nav->right && !s_btn) { s_btn = 1; music_sfx(SFX_MOVE_NEXT); }
         if (circle) { s_view = V_BROWSE; music_sfx(SFX_BACK); }
         else if (nav->confirm)
         {
            music_sfx(s_btn ? SFX_CONFIRM : SFX_BACK);
            if (s_btn) delete_start(); else s_view = V_BROWSE;
         }
         break;
      case V_STOP:
         if (nav->left && s_btn) { s_btn = 0; music_sfx(SFX_MOVE_PREV); }
         if (nav->right && !s_btn) { s_btn = 1; music_sfx(SFX_MOVE_NEXT); }
         if (circle) { s_view = V_COPY; music_sfx(SFX_BACK); }
         else if (nav->confirm)
         {
            if (s_btn) { s_stop = 1; logf_("transfer: stop asked"); }
            s_view = V_COPY; music_sfx(s_btn ? SFX_CONFIRM : SFX_BACK);
         }
         break;
      case V_RESULT:
      {
         const int can_eject = s_res.status != C_LOST && s_dev_state == DEV_READY;
         if (!can_eject) s_btn = 0;
         if (nav->left && s_btn) { s_btn = 0; music_sfx(SFX_MOVE_PREV); }
         if (nav->right && !s_btn && can_eject) { s_btn = 1; music_sfx(SFX_MOVE_NEXT); }
         if (nav->confirm || circle)
         {
            music_sfx(nav->confirm ? SFX_CONFIRM : SFX_BACK);
            if (s_dev_state != DEV_READY) close_card();
            else if (nav->confirm && s_btn) eject_start();
            else s_view = V_BROWSE;
         }
         break;
      }
      default: break;
      }
   }
   {
      int p;
      for (p = 0; p < 2; p++)
      {
         pane_t *pn = &s_pane[p];
         if (pn->sel < pn->top) pn->top = pn->sel;
         if (pn->sel > pn->top + VIS - 1) pn->top = pn->sel - VIS + 1;
         spring_step(&pn->scroll, pn->top * ROW, 240, 26, dt);
         spring_step(&pn->pill, pn->sel * ROW, 260, 26, dt);
      }
      spring_step(&s_dscroll, s_dtop * ROW, 240, 26, dt);
      spring_step(&s_dpill, s_dsel * ROW, 260, 26, dt);
   }
}

/* ------------------------------------------------------------------------ */
/* drawing                                                                   */
static void col4(float o[4], int tc, float a) { theme_color(tc, o); o[3] = a; }

static void draw_label(const label_t *l, float x, float y, const float c[4])
{
   if (l->tex && c[3] > 0.003f) draw_2d(l->tex, floorf(x), floorf(y), l->w, l->h, c, 0);
}

static void draw_folder(float x, float y, const float c[4])
{
   draw_round_rect(x, y, 10, 7, 2.5f, c, 0.0f);
   draw_round_rect(x, y + 3, 22, 15, 3, c, 0.0f);
}

static void draw_file(float x, float y, const float c[4])
{
   const float in[4] = { c[0], c[1], c[2], c[3] * 0.28f };
   draw_round_rect(x + 2, y, 18, 18, 4, in, 0.55f * c[3]);
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

/* a path too long for its line keeps its end */
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
   return ui_fit(f, s, max_w, buf, n);
}

static void draw_pane(int p, float x, float y, double t, float a)
{
   pane_t *pn = &s_pane[p];
   const int act = p == s_act;
   const float k = a * (act ? 1.0f : 0.55f), ly = y + LIST_DY, lh = VIS * ROW;
   float txt[4], dim[4], acc[4];
   char buf[560], sz[48], info[120], n1[16], n2[16];
   const char *fs = fs_label(pn->side == SIDE_LUMEN ? s_root_fs : s_dev_fs);
   int i;
   col4(txt, TC_TEXT, k); col4(dim, TC_DIM, k); col4(acc, TC_ACCENT, k);

   label_set(&pn->name, g_font_medium, side_name(pn->side));
   draw_label(&pn->name, x + 20, y, txt);
   info[0] = 0;
   if (pn->free_b >= 0)
   {
      size_text(sz, sizeof(sz), pn->free_b);
      trf(buf, sizeof(buf), "xfer.free", "size", sz, NULL);
      snprintf(info, sizeof(info), "%s%s%s", fs, fs[0] ? "  \xC2\xB7  " : "", buf);
   }
   label_set(&pn->info, g_font_small, info);
   draw_label(&pn->info, x + PANE_W - 20 - pn->info.w, y + 5, dim);
   label_set(&pn->path, g_font_small, fit_path(g_font_small, pn->path_txt, PANE_W - 40, buf, sizeof(buf)));
   draw_label(&pn->path, x + 20, y + 30, dim);

   if (pn->loading && (!pn->n || t - pn->load_t > 0.25)) { if (!pn->n) draw_dots(x + PANE_W * 0.5f, ly + lh * 0.5f, t, dim); }
   else if (!pn->n)
   {
      label_set(&s_empty, g_font_sub, tr("xfer.empty"));
      draw_label(&s_empty, x + (PANE_W - s_empty.w) * 0.5f, ly + lh * 0.5f - 14, dim);
   }
   if (!pn->n) return;

   glEnable(GL_SCISSOR_TEST);
   gl_scissor(x - 8, ly - 8, PANE_W + 16, lh + 16);
   if (act) ui_focus(x, ly + pn->pill.x - pn->scroll.x + 1, PANE_W, ROW - 2, (ROW - 2) * 0.5f, a);
   else ui_rest(x, ly + pn->pill.x - pn->scroll.x + 1, PANE_W, ROW - 2, (ROW - 2) * 0.5f, a * 0.8f);
   for (i = (int)(pn->scroll.x / ROW) - 1; i < pn->n; i++)
   {
      const ent_t *e;
      float ry, c[4];
      int slot;
      if (i < 0) continue;
      ry = ly + i * ROW - pn->scroll.x;
      if (ry >= ly + lh - 1) break;
      if (ry + ROW <= ly + 1) continue;
      e = &pn->ent[i];
      slot = i % POOL;
      if (pn->slot_ent[slot] != i || pn->slot_gen[slot] != pn->gen)
      {
         label_set(&pn->rl[slot], g_font_sub, ui_fit(g_font_sub, e->name, PANE_W - 150, buf, sizeof(buf)));
         pn->slot_ent[slot] = i; pn->slot_gen[slot] = pn->gen;
      }
      memcpy(c, txt, sizeof(c));
      if (i != pn->sel) c[3] *= 0.86f;
      if (e->dir) draw_folder(x + 22, ry + (ROW - 18) * 0.5f, txt);
      else draw_file(x + 22, ry + (ROW - 18) * 0.5f, i == pn->sel ? txt : dim);
      draw_label(&pn->rl[slot], x + 58, ry + (ROW - pn->rl[slot].h) * 0.5f, c);
      if (e->mark) draw_round_rect(x + PANE_W - 70, ry + ROW * 0.5f - 6, 12, 12, 6, acc, 0.0f);
      if (e->dir)
      {  /* chevron: it opens */
         float cv[4] = { c[0], c[1], c[2], c[3] * 0.6f }, ccx = x + PANE_W - 30, ccy = ry + ROW * 0.5f;
         int q;
         for (q = 0; q < 4; q++)
         {
            draw_2d(0, ccx + q * 2, ccy - 7 + q * 2, 2.5f, 2.5f, cv, 0);
            draw_2d(0, ccx + q * 2, ccy + 7 - q * 2 - 2.5f, 2.5f, 2.5f, cv, 0);
         }
      }
   }
   glDisable(GL_SCISSOR_TEST);
   if (pn->n > VIS)
   {
      float th = lh * VIS / (float)pn->n, ty = ly + (lh - th) * clampf(pn->scroll.x / ((pn->n - VIS) * ROW), 0.0f, 1.0f);
      float tc[4]; col4(tc, TC_TEXT, 0.22f * k);
      draw_round_rect(x + PANE_W + 4, ty, 4, th, 2, tc, 0.0f);
   }
   if (act)
   {
      snprintf(n1, sizeof(n1), "%d", pn->sel + 1); snprintf(n2, sizeof(n2), "%d", pn->n);
      label_set(&pn->count, g_font_small, trf(buf, sizeof(buf), "fp.count", "i", n1, "n", n2, NULL));
      draw_label(&pn->count, x + 20, y + (CH - PANE_Y - 14) - pn->count.h, dim);
   }
}

/* a pill button: the primary is white with dark text, a danger one is tinted */
static void draw_btn(label_t *l, const char *text, float x, float y, float w, int focus, int primary, int danger, float a)
{
   static const float white[3] = { 1, 1, 1 };
   float c[4];
   label_set(l, g_font_medium, text);
   if (primary)
   {
      ui_fill(x, y, w, 52, 26, white, a);
      if (focus) ui_ring(x, y, w, 52, 26, 4.0f, a);
      c[0] = 0.043f; c[1] = 0.047f; c[2] = 0.063f; c[3] = a;
   }
   else
   {
      if (focus) ui_focus(x, y, w, 52, 26, a);
      ui_rest(x, y, w, 52, 26, a);
      col4(c, danger ? TC_DANGER : TC_TEXT, a);
   }
   draw_label(l, x + (w - l->w) * 0.5f, y + (52 - l->h) * 0.5f, c);
}

static void draw_dest(float a)
{
   const float w = 620, h = 432, x = (W - w) * 0.5f, y = 150 + (1 - a) * 12, lx = x + 14, ly = y + 110, lw = w - 28;
   float txt[4], dim[4], sep[4];
   char buf[300], num[16];
   int i;
   col4(txt, TC_TEXT, a); col4(dim, TC_DIM, a); col4(sep, TC_TEXT, 0.08f * a);
   ui_card(x, y, w, h, a);
   snprintf(num, sizeof(num), "%d", s_nsel);
   ui_kicker(&s_sk, s_nsel == 1 ? tr("xfer.copy_one") : trf(buf, sizeof(buf), "xfer.copy_n", "n", num, NULL), x + PADX, y + 24, a);
   label_set(&s_st, g_font_title, side_name(s_pane[!s_act].side));
   draw_label(&s_st, x + PADX, y + 42, txt);
   draw_2d(0, x + PADX, y + 96, w - 2 * PADX, 1, sep, 0);
   glEnable(GL_SCISSOR_TEST);
   gl_scissor(lx - 8, ly - 8, lw + 16, DVIS * ROW + 16);
   ui_focus(lx, ly + s_dpill.x - s_dscroll.x + 1, lw, ROW - 2, (ROW - 2) * 0.5f, a);
   for (i = 0; i < s_ndest; i++)
   {
      float ry = ly + i * ROW - s_dscroll.x;
      int slot = i % POOL;
      if (ry >= ly + DVIS * ROW - 1) break;
      if (ry + ROW <= ly + 1) continue;
      if (s_dslot[slot] != i || s_dslot_gen[slot] != s_dgen)
      {
         label_set(&s_dl[slot], g_font_sub, ui_fit(g_font_sub, s_dest[i].label, lw * 0.5f - 30, buf, sizeof(buf)));
         label_set(&s_dv[slot], g_font_sub, ui_fit(g_font_sub, s_dest[i].shown, lw * 0.5f - 20, buf, sizeof(buf)));
         s_dslot[slot] = i; s_dslot_gen[slot] = s_dgen;
      }
      draw_label(&s_dl[slot], lx + 20, ry + (ROW - s_dl[slot].h) * 0.5f, txt);
      draw_label(&s_dv[slot], lx + lw - 20 - s_dv[slot].w, ry + (ROW - s_dv[slot].h) * 0.5f, dim);
   }
   glDisable(GL_SCISSOR_TEST);
   label_set(&s_l1, g_font_small, tr("xfer.dest_note"));
   draw_label(&s_l1, x + PADX, y + h - 20 - s_l1.h, dim);
}

static void draw_copy(double t, float a)
{
   const float w = 620, h = 240, x = (W - w) * 0.5f, y = 232 + (1 - a) * 12;
   static const float white[3] = { 1, 1, 1 };
   float txt[4], dim[4];
   char buf[420], b1[48], b2[48], n1[16], n2[16], right[160], name[NAME_LEN];
   long long bytes, done;
   int phase, files, fdone;
   SDL_LockMutex(s_mx);
   phase = s_cp.phase; bytes = s_cp.bytes; done = s_cp.bytes_done; files = s_cp.files; fdone = s_cp.files_done;
   snprintf(name, sizeof(name), "%s", s_cp.cur);
   SDL_UnlockMutex(s_mx);
   col4(txt, TC_TEXT, a); col4(dim, TC_DIM, a);
   ui_card(x, y, w, h, a);
   trf(buf, sizeof(buf), "xfer.copying", "path", s_dst_txt, NULL);
   ui_kicker(&s_sk, fit_path(g_font_caps, buf, w - 2 * PADX - 90, right, sizeof(right)), x + PADX, y + 24, a);
   label_set(&s_st, g_font_medium, phase == PH_COUNT ? tr("xfer.counting") : phase == PH_FINISH ? tr("xfer.finishing") :
             ui_fit(g_font_medium, name, w - 2 * PADX, buf, sizeof(buf)));
   draw_label(&s_st, x + PADX, y + 54, txt);
   ui_fill(x + PADX, y + 112, w - 2 * PADX, 8, 4, white, UI_REST * 1.6f * a);
   if (phase != PH_COUNT && bytes > 0)
   {
      float f = clampf((float)((double)done / (double)bytes), 0.0f, 1.0f), fw = (w - 2 * PADX) * f;
      if (fw >= 8) ui_fill(x + PADX, y + 112, fw, 8, 4, white, a);
   }
   else if (phase == PH_COUNT) draw_dots(x + w * 0.5f, y + 116, t, txt);
   b1[0] = right[0] = 0;
   if (phase != PH_COUNT && files > 0)
   {
      double el = t - s_copy_t0, eta;
      size_text(b1, sizeof(b1), done); size_text(b2, sizeof(b2), bytes);
      trf(buf, sizeof(buf), "xfer.bytes", "done", b1, "total", b2, NULL);
      label_set(&s_l1, g_font_small, buf);
      draw_label(&s_l1, x + PADX, y + 136, dim);
      snprintf(n1, sizeof(n1), "%d", fdone + 1 > files ? files : fdone + 1); snprintf(n2, sizeof(n2), "%d", files);
      trf(right, sizeof(right), "xfer.file_n", "i", n1, "n", n2, NULL);
      if (phase == PH_COPY && el > 4.0 && done > 0 && bytes > done)
      {
         size_t o = strlen(right);
         eta = el * (double)(bytes - done) / (double)done;
         snprintf(n1, sizeof(n1), "%d", (int)(eta / 60.0 + 0.5));
         snprintf(right + o, sizeof(right) - o, "  \xC2\xB7  %s", eta < 50.0 ? tr("xfer.eta_soon") : trf(buf, sizeof(buf), "xfer.eta_min", "n", n1, NULL));
      }
      label_set(&s_l2, g_font_small, right);
      draw_label(&s_l2, x + w - PADX - s_l2.w, y + 136, dim);
   }
   label_set(&s_l3, g_font_small, tr("xfer.keep"));
   draw_label(&s_l3, x + PADX, y + h - 24 - s_l3.h, dim);
}

/* the two-button card: a question, or the result of the copy */
static void draw_dialog(const char *title, const char *l1, const char *l2, const char *b0, const char *b1, int primary0, int danger1, float a)
{
   const float w = 580, h = 228, x = (W - w) * 0.5f, y = 238 + (1 - a) * 12, by = y + h - 28 - 52;
   float txt[4], dim[4], bw = b1 ? (w - 80) * 0.5f : w - 64;
   char buf[420];
   col4(txt, TC_TEXT, a); col4(dim, TC_DIM, a);
   ui_card(x, y, w, h, a);
   label_set(&s_st, g_font_medium, ui_fit(g_font_medium, title, w - 64, buf, sizeof(buf)));
   draw_label(&s_st, x + 32, y + 28, txt);
   if (!l1) { l1 = l2; l2 = NULL; }
   label_set(&s_l1, g_font_small, l1 ? ui_fit(g_font_small, l1, w - 64, buf, sizeof(buf)) : "");
   draw_label(&s_l1, x + 32, y + 66, dim);
   label_set(&s_l2, g_font_small, l2 ? ui_fit(g_font_small, l2, w - 64, buf, sizeof(buf)) : "");
   draw_label(&s_l2, x + 32, y + 92, dim);
   draw_btn(&s_b[0], b0, x + 32, by, bw, s_btn == 0, primary0, 0, a);
   if (b1) draw_btn(&s_b[1], b1, x + 32 + bw + 16, by, bw, s_btn == 1, 0, danger1, a);
}

static void draw_result(float a)
{
   char title[200], l1[300], sz[48], n[16], s1[48], s2[48];
   const char *l2 = NULL, *b1 = s_res.status != C_LOST && s_dev_state == DEV_READY ? tr("xfer.eject") : NULL;
   int ok = 0;
   l1[0] = 0;
   snprintf(n, sizeof(n), "%d", s_res.files_done);
   switch (s_res.status)
   {
   case C_OK:
      ok = 1;
      size_text(sz, sizeof(sz), s_res.bytes_done);
      if (!s_res.files_done) snprintf(title, sizeof(title), "%s", tr("xfer.done_none"));
      else if (s_res.files_done == 1) trf(title, sizeof(title), "xfer.done_one", "size", sz, NULL);
      else trf(title, sizeof(title), "xfer.done_n", "n", n, "size", sz, NULL);
      if (s_res.skipped && s_res.files_done) { snprintf(s1, sizeof(s1), "%d", s_res.skipped); trf(l1, sizeof(l1), "xfer.skipped", "n", s1, NULL); }
      l2 = tr("xfer.eject_note");
      break;
   case C_STOPPED:
      snprintf(title, sizeof(title), "%s", tr("xfer.stopped"));
      trf(l1, sizeof(l1), "xfer.stopped_note", "n", n, NULL);
      l2 = tr("xfer.eject_note");
      break;
   case C_SPACE:
      snprintf(title, sizeof(title), "%s", tr("xfer.err_space"));
      if (s_res.need > 0) { size_text(s1, sizeof(s1), s_res.need); size_text(s2, sizeof(s2), s_res.avail); trf(l1, sizeof(l1), "xfer.err_space_note", "need", s1, "free", s2, NULL); }
      break;
   case C_LOST:
      snprintf(title, sizeof(title), "%s", tr("xfer.err_lost"));
      snprintf(l1, sizeof(l1), "%s", tr("xfer.err_lost_note"));
      break;
   case C_BIG: snprintf(title, sizeof(title), "%s", tr("xfer.err_big")); snprintf(l1, sizeof(l1), "%s", s_res.name); break;
   case C_RO:  snprintf(title, sizeof(title), "%s", tr("xfer.err_ro")); break;
   case C_READ: snprintf(title, sizeof(title), "%s", tr("xfer.err_read")); snprintf(l1, sizeof(l1), "%s", s_res.name); break;
   default:    snprintf(title, sizeof(title), "%s", tr(s_del ? "xfer.err_del" : "xfer.err_write")); snprintf(l1, sizeof(l1), "%s", s_res.name); break;
   }
   if (!ok && !s_del && s_res.status != C_STOPPED && s_res.status != C_SPACE && s_res.status != C_RO && s_res.files_done > 0)
   {  /* what did get across before the error */
      static char kept[200];
      trf(kept, sizeof(kept), "xfer.stopped_note", "n", n, NULL);
      l2 = kept;
   }
   draw_dialog(title, l1[0] ? l1 : NULL, l2, tr("xfer.ok"), b1, 1, 0, a);
}

static void draw_menu(float a)
{
   const float w = 480, h = 212, x = (W - w) * 0.5f, y = 246 + (1 - a) * 12, lx = x + 14, ly = y + 58, lw = w - 28;
   float txt[4], dim[4], red[4];
   char buf[560], num[16];
   const pane_t *pn = &s_pane[s_act];
   col4(txt, TC_TEXT, a); col4(dim, TC_DIM, a); col4(red, TC_DANGER, a * (s_nsel ? 1.0f : 0.4f));
   ui_card(x, y, w, h, a);
   ui_kicker(&s_sk, tr("hint.options"), x + PADX, y + 24, a);
   ui_focus(lx, ly + s_msel * ROW + 1, lw, ROW - 2, (ROW - 2) * 0.5f, a);
   label_set(&s_st, g_font_sub, tr("xfer.eject"));
   draw_label(&s_st, lx + 20, ly + (ROW - s_st.h) * 0.5f, txt);
   snprintf(num, sizeof(num), "%d", s_nsel);
   label_set(&s_l2, g_font_sub, s_nsel == 1 ? tr("xfer.del_one") : trf(buf, sizeof(buf), "xfer.del_n", "n", num, NULL));
   draw_label(&s_l2, lx + 20, ly + ROW + (ROW - s_l2.h) * 0.5f, red);
   label_set(&s_l1, g_font_small, fit_path(g_font_small, pn->path_txt, w - 2 * PADX, buf, sizeof(buf)));
   draw_label(&s_l1, x + PADX, y + h - 20 - s_l1.h, dim);
}

static void draw_delq(float a)
{
   const pane_t *pn = &s_pane[s_act];
   char title[300], num[16];
   const char *one = NULL;
   int i;
   snprintf(num, sizeof(num), "%d", s_nsel);
   if (s_nsel == 1)
   {  /* one entry: its name goes on the second line */
      for (i = 0; i < pn->n; i++) if (pn->nmark ? pn->ent[i].mark : i == pn->sel) { one = pn->ent[i].name; break; }
      trf(title, sizeof(title), "xfer.del_q_one", "drive", side_name(pn->side), NULL);
   }
   else trf(title, sizeof(title), "xfer.del_q", "n", num, "drive", side_name(pn->side), NULL);
   draw_dialog(title, one, tr("xfer.del_note"), tr("confirm.cancel"), tr("confirm.delete"), 0, 1, a);
}

static void draw_offer(double t)
{
   float e = (float)(t - s_offer_t), a, w, h = 44, x, y, f[4], c[4];
   if (e < 0 || e > OFFER_S || s_open || !s_home) return;   /* only over the free Home */
   a = clampf(e / 0.15f, 0, 1) * clampf(((float)OFFER_S - e) / 0.3f, 0, 1);
   gl_use_2d();
   /* "□  USB drive connected: transfer files": the button's own sign, no icon to load */
   label_set(&s_offer, g_font_sub, tr("xfer.offer"));
   w = s_offer.w + 48 + 36; x = floorf((W - w) * 0.5f); y = 610 + (1.0f - a) * 8.0f;
   f[0] = 0.10f; f[1] = 0.10f; f[2] = 0.14f; f[3] = 0.88f * a;
   draw_round_rect(x, y, w, h, h * 0.5f, f, 0.14f * a);
   col4(c, TC_TEXT, a);
   /* the Square button, drawn: a ring with a square in it */
   ui_shape(x + 20, y + 9, 26, 26, 13, 2.0f, c);
   ui_shape(x + 28, y + 17, 10, 10, 1.5f, 2.0f, c);
   draw_label(&s_offer, x + 24 + 36, y + (h - s_offer.h) * 0.5f, c);
}

void transfer_draw(double t)
{
   const float a = transfer_amount(), cx = (W - CW) * 0.5f, cy = CY + (1 - a) * 16, sub = clampf(s_sub.x, 0.0f, 1.0f);
   float txt[4], acc[4], sep[4];
   char buf[420], num[16];
   const pane_t *pn = &s_pane[s_act];
   const char *here;
   if (a <= 0.003f) { draw_offer(t); return; }
   col4(txt, TC_TEXT, a); col4(acc, TC_ACCENT, a); col4(sep, TC_TEXT, 0.08f * a);
   gl_use_2d();
   ui_scrim(a * (1.0f - panel_amount()));
   ui_card(cx, cy, CW, CH, a);
   here = pane_at_root(pn) || !strrchr(pn->dir, '/') ? side_name(pn->side) : strrchr(pn->dir, '/') + 1;
   ui_kicker(&s_kick, tr("xfer.title"), cx + PADX, cy + 24, a);
   label_set(&s_title, g_font_title, ui_fit(g_font_title, here, CW * 0.6f, buf, sizeof(buf)));
   draw_label(&s_title, cx + PADX, cy + 42, txt);
   if (pn->nmark)
   {
      snprintf(num, sizeof(num), "%d", pn->nmark);
      label_set(&s_selc, g_font_small, trf(buf, sizeof(buf), "xfer.selected", "n", num, NULL));
      draw_label(&s_selc, cx + CW - PADX - s_selc.w, cy + 58, acc);
   }
   draw_2d(0, cx + PADX, cy + 96, CW - 2 * PADX, 1, sep, 0);
   draw_pane(0, cx + 14, cy + PANE_Y, t, a);
   draw_2d(0, cx + CW * 0.5f, cy + PANE_Y, 1, CH - PANE_Y - 12, sep, 0);
   draw_pane(1, cx + CW - 14 - PANE_W, cy + PANE_Y, t, a);
   gl_use_2d();

   if (sub > 0.01f)
   {  /* a smaller card takes the stage: the panes stay behind a veil */
      float veil[4], sa = sub * a;
      col4(veil, TC_CARD, 0.80f * sa);
      draw_round_rect(cx, cy, CW, CH, UI_CARD_R, veil, 0.0f);
      switch (s_view)
      {
      case V_DEST: draw_dest(sa); break;
      case V_COPY: draw_copy(t, sa); break;
      case V_STOP: draw_dialog(tr("xfer.stop_q"), tr("xfer.stop_note"), NULL, tr("xfer.stop_no"), tr("xfer.stop_yes"), 0, 1, sa); break;
      case V_RESULT: draw_result(sa); break;
      case V_MENU: draw_menu(sa); break;
      case V_DELQ: draw_delq(sa); break;
      case V_EJECT: case V_DEL:
         label_set(&s_busy_l, g_font_sub, tr(s_view == V_DEL ? "xfer.deleting" : "xfer.ejecting"));
         draw_label(&s_busy_l, cx + (CW - s_busy_l.w) * 0.5f, cy + CH * 0.5f - 34, txt);
         draw_dots(cx + CW * 0.5f, cy + CH * 0.5f + 18, t, txt);
         break;
      default: break;
      }
      gl_use_2d();
   }
}

/* ------------------------------------------------------------------------ */
void transfer_init(void)
{
   const char *e = getenv("LUMEN_USB2");
   SDL_Thread *a, *b;
   int i;
   for (i = 0; i < POOL; i++) s_dslot[i] = -1;
   if (e && e[0]) snprintf(s_sim, sizeof(s_sim), "%s", e);
   if ((e = getenv("LUMEN_USB2_KBPS"))) s_kbps = atol(e);
   if ((e = getenv("LUMEN_USB2_FREE_MB"))) s_free_mb = atol(e);
   root_find();
   systems_load();
   if (!s_sim[0] && !s_root_disk[0])
   { logf_("transfer: off (%s is not on a USB disk)", g_root); return; }
   s_mx = SDL_CreateMutex(); s_op_mx = SDL_CreateMutex(); s_cv = SDL_CreateCond();
   if (!s_mx || !s_op_mx || !s_cv) return;
   a = SDL_CreateThread(watcher_main, "usb-watch", NULL);
   b = SDL_CreateThread(worker_main, "usb-copy", NULL);
   if (!a || !b) { logf_("transfer: no threads (%s)", SDL_GetError()); s_quit = 1; return; }
   SDL_DetachThread(a); SDL_DetachThread(b);
   s_threads = 1;
   logf_("transfer: watching for a second drive (%s, root disk %s %s)", s_sim[0] ? s_sim : "/sys/block",
         s_root_disk[0] ? s_root_disk : "-", s_root_fs);
}

void transfer_shutdown(void)
{
   char mnt[300];
   int i, busy, ours;
   if (!s_threads) return;
   s_stop = 1;   /* a copy still running stops at the next block and removes its part */
   for (i = 0; i < 100; i++)
   {
      SDL_LockMutex(s_mx);
      busy = s_cp.running || s_creq.pending;
      SDL_UnlockMutex(s_mx);
      if (!busy) break;
      SDL_Delay(50);
   }
   s_quit = 1;
   SDL_LockMutex(s_mx);
   ours = s_dev.state == DEV_READY && s_dev.ours && !s_sim[0];
   snprintf(mnt, sizeof(mnt), "%s", s_dev.mnt);
   SDL_UnlockMutex(s_mx);
   if (ours)
   {  /* a game is about to start, or the console to switch off: the drive can be pulled */
      SDL_LockMutex(s_op_mx);
      dev_unmount(mnt);
      SDL_UnlockMutex(s_op_mx);
      logf_("transfer: second drive unmounted");
   }
}
