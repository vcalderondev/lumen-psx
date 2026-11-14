/*
 * lumen-buttons — console buttons for a Lumen session (PlayStation Classic).
 *
 * The console's front buttons are a gpio-keys input device:
 *   POWER = KEY_SLEEP (142, wake source), RESET = KEY_PLAYPAUSE (164), OPEN = KEY_EJECTCD (161)
 *
 * POWER is Sony's: its power manager (power_manage) suspends the console to RAM (red
 * LED; the PSC has no software power-off: a halted console stays orange until it is
 * unplugged) after a handshake: it creates <power>/prepare_suspend and waits for
 * <power>/apps_okay. Before answering, this daemon runs power.sh suspend (sync, pause
 * every process on the USB stick: a task waiting on the exFAT/FUSE stick cannot be
 * frozen, the suspend would hang); when <power>/resume_count changes (the console woke
 * up) it runs power.sh resume.
 * RESET quits a running RetroArch, which returns the session to Lumen.
 * OPEN is logged (reserved for disc swapping).
 *
 * Usage: lumen-buttons [--log file] [--power-dir /data/power]
 *
 * Demo watcher (attract mode, docs/settings-design.md 8.7):
 *   lumen-buttons --demo-watch <seconds> --pid <pid> [--log file]
 * watches every pad/keyboard/gpio-keys device (never grabbed: RetroArch reads them
 * too) and stops the RetroArch started as <pid> when the demo time is over or a
 * button is pressed. Exit: 0 <pid> ended by itself, 1 input, 2 timeout, 3 error.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* The log is on the USB stick (exFAT through FUSE), where a file's size is only
 * committed on close/fsync: every line is flushed, fsync runs at most once per second
 * (log_tick() in the loops catches up), at once for error lines, and at exit. */
static FILE  *g_log;
static double g_log_synced = -10.0;
static int    g_log_pending;

static double log_clock(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void log_sync(int force)
{
   double t;
   if (!g_log || g_log == stderr || !g_log_pending) return;
   t = log_clock();
   if (!force && t - g_log_synced < 1.0) return;
   fsync(fileno(g_log));
   g_log_synced = t; g_log_pending = 0;
}

static void log_tick(void) { log_sync(0); }
static void log_sync_atexit(void) { if (g_log) fflush(g_log); log_sync(1); }

static int log_is_error(const char *s)
{
   for (; *s; s++)
      if (!strncasecmp(s, "error", 5) || !strncasecmp(s, "fail", 4)) return 1;
   return 0;
}

static void logf_(const char *fmt, ...)
{
   va_list ap; time_t now = time(NULL); char ts[32], line[512];
   strftime(ts, sizeof(ts), "%H:%M:%S", localtime(&now));
   va_start(ap, fmt); vsnprintf(line, sizeof(line), fmt, ap); va_end(ap);
   fprintf(g_log, "%s %s\n", ts, line); fflush(g_log);
   g_log_pending = 1;
   log_sync(log_is_error(line));
}

/* /dev/input/eventN of the device named "gpio-keys" */
static int open_buttons(char *path, size_t n)
{
   DIR *d = opendir("/sys/class/input");
   struct dirent *de; int fd = -1;
   if (!d) return -1;
   while ((de = readdir(d)) && fd < 0)
   {
      char np[600], name[64] = ""; FILE *f;
      if (strncmp(de->d_name, "event", 5)) continue;
      snprintf(np, sizeof(np), "/sys/class/input/%s/device/name", de->d_name);
      if (!(f = fopen(np, "r"))) continue;
      if (fgets(name, sizeof(name), f)) name[strcspn(name, "\n")] = 0;
      fclose(f);
      if (strcmp(name, "gpio-keys")) continue;
      snprintf(path, n, "/dev/input/%s", de->d_name);
      fd = open(path, O_RDONLY | O_NONBLOCK);
   }
   closedir(d);
   return fd;
}

/* pid of a running process whose comm is `name`, or 0 */
static pid_t find_proc(const char *name)
{
   DIR *d = opendir("/proc");
   struct dirent *de; pid_t pid = 0;
   if (!d) return 0;
   while ((de = readdir(d)) && !pid)
   {
      char p[300], comm[64] = ""; FILE *f;
      if (de->d_name[0] < '0' || de->d_name[0] > '9') continue;
      snprintf(p, sizeof(p), "/proc/%s/comm", de->d_name);
      if (!(f = fopen(p, "r"))) continue;
      if (fgets(comm, sizeof(comm), f)) comm[strcspn(comm, "\n")] = 0;
      fclose(f);
      if (!strcmp(comm, name)) pid = (pid_t)atoi(de->d_name);
   }
   closedir(d);
   return pid;
}

static int exists(const char *p) { struct stat st; return stat(p, &st) == 0; }

/* ------------------------------------------------------------------------ */
/* demo watcher                                                              */
#define DW_MAX 16
#define BITS_LONG (8 * (int)sizeof(long))
#define TEST_BIT(b, a) (((a)[(b) / BITS_LONG] >> ((b) % BITS_LONG)) & 1UL)
static const int dw_axes[4] = { ABS_X, ABS_Y, ABS_HAT0X, ABS_HAT0Y };
typedef struct { int fd, num; int has_abs[4], abs0[4], range[4]; } dw_dev_t;
static dw_dev_t dw_dev[DW_MAX];
static int      dw_n;

static double mono_s(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* a process that exists and is not a zombie (run.sh only reaps it after we exit) */
static int proc_alive(pid_t pid)
{
   char p[64], buf[300], *rp;
   FILE *f;
   size_t n;
   if (kill(pid, 0) != 0 && errno == ESRCH) return 0;
   snprintf(p, sizeof(p), "/proc/%d/stat", (int)pid);
   if (!(f = fopen(p, "r"))) return 1;
   n = fread(buf, 1, sizeof(buf) - 1, f); buf[n] = 0;
   fclose(f);
   rp = strrchr(buf, ')');
   return !(rp && rp[1] == ' ' && (rp[2] == 'Z' || rp[2] == 'X'));
}

/* comm and parent of a process from /proc/<pid>/stat */
static int proc_stat(pid_t pid, char *comm, size_t n, pid_t *ppid)
{
   char p[64], buf[400], *lp, *rp;
   FILE *f;
   size_t len;
   int pp = 0;
   snprintf(p, sizeof(p), "/proc/%d/stat", (int)pid);
   if (!(f = fopen(p, "r"))) return -1;
   len = fread(buf, 1, sizeof(buf) - 1, f); buf[len] = 0;
   fclose(f);
   if (!(lp = strchr(buf, '(')) || !(rp = strrchr(buf, ')'))) return -1;
   *rp = 0;
   snprintf(comm, n, "%s", lp + 1);
   if (sscanf(rp + 2, "%*c %d", &pp) != 1) return -1;
   *ppid = (pid_t)pp;
   return 0;
}

/* processes named "retroarch" that descend from root (or root itself), up to max */
static int find_descendants(pid_t root, const char *name, pid_t *out, int max)
{
   DIR *d = opendir("/proc");
   struct dirent *de;
   int n = 0;
   if (!d) return 0;
   while ((de = readdir(d)) && n < max)
   {
      char comm[64]; pid_t pid, pp, cur; int depth;
      if (de->d_name[0] < '0' || de->d_name[0] > '9') continue;
      pid = (pid_t)atoi(de->d_name);
      if (proc_stat(pid, comm, sizeof(comm), &pp) != 0 || strcmp(comm, name)) continue;
      for (cur = pid, depth = 0; cur > 1 && depth < 32; depth++)
      {
         if (cur == root) { out[n++] = pid; break; }
         if (proc_stat(cur, comm, sizeof(comm), &pp) != 0) break;
         cur = pp;
      }
   }
   closedir(d);
   return n;
}

/* SIGTERM RetroArch (descendants of pid, else pid), SIGKILL after 3 s */
static void stop_game(pid_t pid, const char *why)
{
   pid_t t[8];
   int n = find_descendants(pid, "retroarch", t, 8), i, alive = 1;
   double until;
   if (!n) { t[0] = pid; n = 1; }
   logf_("demo-watch: %s -> SIGTERM %d process(es) (first %d)", why, n, (int)t[0]);
   for (i = 0; i < n; i++) kill(t[i], SIGTERM);
   until = mono_s() + 3.0;
   while (alive && mono_s() < until)
   {
      alive = 0;
      for (i = 0; i < n; i++) alive |= proc_alive(t[i]);
      if (alive) usleep(50000);
   }
   if (alive)
   {
      logf_("demo-watch: still running after 3 s -> SIGKILL");
      for (i = 0; i < n; i++) if (proc_alive(t[i])) kill(t[i], SIGKILL);
   }
}

static void dw_close(int i)
{
   close(dw_dev[i].fd);
   dw_dev[i] = dw_dev[--dw_n];
}

/* open every new /dev/input/eventN that is a pad, a keyboard or the console buttons */
static void dw_scan(void)
{
   DIR *d = opendir("/dev/input");
   struct dirent *de;
   if (!d) return;
   while ((de = readdir(d)) && dw_n < DW_MAX)
   {
      char path[300], name[64] = "";
      unsigned long keys[KEY_MAX / (8 * sizeof(long)) + 1], abs_bits[ABS_MAX / (8 * sizeof(long)) + 1];
      int num, fd, i, keep = 0, b;
      if (strncmp(de->d_name, "event", 5)) continue;
      num = atoi(de->d_name + 5);
      for (i = 0; i < dw_n && dw_dev[i].num != num; i++) ;
      if (i < dw_n) continue;
      snprintf(path, sizeof(path), "/dev/input/%s", de->d_name);
      if ((fd = open(path, O_RDONLY | O_NONBLOCK)) < 0) continue;
      memset(keys, 0, sizeof(keys)); memset(abs_bits, 0, sizeof(abs_bits));
      ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name);
      ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys);
      if (!strcmp(name, "gpio-keys") || TEST_BIT(KEY_ENTER, keys)) keep = 1;
      for (b = BTN_JOYSTICK; b <= BTN_THUMBR && !keep; b++) if (TEST_BIT(b, keys)) keep = 1;
      if (!keep) { close(fd); continue; }
      {
         dw_dev_t *dv = &dw_dev[dw_n++];
         memset(dv, 0, sizeof(*dv));
         dv->fd = fd; dv->num = num;
         ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs_bits)), abs_bits);
         for (i = 0; i < 4; i++)
         {
            struct input_absinfo ai;
            if (!TEST_BIT(dw_axes[i], abs_bits) || ioctl(fd, EVIOCGABS(dw_axes[i]), &ai) < 0) continue;
            dv->has_abs[i] = ai.maximum > ai.minimum;
            dv->abs0[i] = ai.value;
            dv->range[i] = ai.maximum - ai.minimum;
         }
         logf_("demo-watch: %s (%s)", path, name);
      }
   }
   closedir(d);
}

static int demo_watch(int seconds, pid_t pid)
{
   const double t0 = mono_s(), grace = t0 + 1.0, end = t0 + seconds;
   double rescan = 0;
   logf_("demo-watch: %d s, pid %d", seconds, (int)pid);
   for (;;)
   {
      struct pollfd pfd[DW_MAX];
      double now = mono_s();
      int i, r, input = 0;
      if (!proc_alive(pid)) { logf_("demo-watch: game ended by itself"); return 0; }
      if (now >= end) { stop_game(pid, "time is up"); return 2; }
      if (now >= rescan) { dw_scan(); rescan = now + 2.0; }
      log_tick();
      for (i = 0; i < dw_n; i++) { pfd[i].fd = dw_dev[i].fd; pfd[i].events = POLLIN; pfd[i].revents = 0; }
      r = poll(pfd, dw_n, 100);
      if (r <= 0) { if (!dw_n) usleep(100000); continue; }
      now = mono_s();
      for (i = dw_n - 1; i >= 0; i--)
      {
         struct input_event ev;
         ssize_t got;
         if (pfd[i].revents & (POLLERR | POLLHUP | POLLNVAL)) { dw_close(i); continue; }
         if (!(pfd[i].revents & POLLIN)) continue;
         while ((got = read(dw_dev[i].fd, &ev, sizeof(ev))) == (ssize_t)sizeof(ev))
         {
            int k;
            if (now < grace) continue;
            if (ev.type == EV_KEY && ev.value == 1 && ev.code != KEY_SLEEP)
            { logf_("demo-watch: key %d", ev.code); input = 1; }
            else if (ev.type == EV_ABS)
               for (k = 0; k < 4; k++)
                  /* > 40 % of the range: a centred 0..255 d-pad axis moves 127-128 (49.8 %) */
                  if (ev.code == dw_axes[k] && dw_dev[i].has_abs[k] &&
                      abs(ev.value - dw_dev[i].abs0[k]) * 10 > dw_dev[i].range[k] * 4)
                  { logf_("demo-watch: axis %d = %d", ev.code, ev.value); input = 1; }
         }
         if (got < 0 && errno == ENODEV) { dw_close(i); continue; }
      }
      if (input) { stop_game(pid, "button pressed"); return 1; }
   }
}

static int run_power(const char *action, int wait)
{
   pid_t p = fork();
   if (p == 0)
   {
      execl("/bin/sh", "sh", "/tmp/lumen-power.sh", action, (char *)NULL);
      _exit(127);
   }
   if (p > 0 && wait) { int st; waitpid(p, &st, 0); return WIFEXITED(st) ? WEXITSTATUS(st) : -1; }
   return p > 0 ? 0 : -1;
}
static long read_long(const char *path)
{
   char b[32] = { 0 };
   int f = open(path, O_RDONLY);
   if (f < 0) return -1;
   if (read(f, b, sizeof(b) - 1) < 0) b[0] = 0;
   close(f);
   return atol(b);
}

static const char *g_home = "/media/lumen";

/* ---- RetroArch network commands (retroarch.cfg: network_cmd_enable, port 55355) */
static void ra_cmd(const char *cmd)
{
   struct sockaddr_in a;
   int s = socket(AF_INET, SOCK_DGRAM, 0);
   if (s < 0) return;
   memset(&a, 0, sizeof(a));
   a.sin_family = AF_INET; a.sin_port = htons(55355); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
   if (sendto(s, cmd, strlen(cmd), 0, (struct sockaddr *)&a, sizeof(a)) < 0) logf_("ra_cmd %s: %s", cmd, strerror(errno));
   close(s);
}
/* discs of the running game: run.sh writes the count to /tmp/lumen-discs (it builds an
 * .m3u for a multi-disc folder) */
static int game_discs(void)
{
   FILE *g = fopen("/tmp/lumen-discs", "r");
   int c = 0;
   if (g) { if (fscanf(g, "%d", &c) != 1) c = 0; fclose(g); if (c > 0) return c; }
   {
   char line[700], content[600] = "";
   int n = 0, ln = 0;
   FILE *f = fopen("/tmp/lumen-launch", "r");
   size_t l;
   if (!f) return 1;
   while (fgets(line, sizeof(line), f))
   {
      line[strcspn(line, "\r\n")] = 0; ln++;
      if (!strncmp(line, "content=", 8)) snprintf(content, sizeof(content), "%s", line + 8);
      else if (ln == 2 && !content[0] && !strchr(line, '=')) snprintf(content, sizeof(content), "%s", line);
   }
   fclose(f);
   l = strlen(content);
   if (l < 4 || strcasecmp(content + l - 4, ".m3u")) return 1;
   if (!(f = fopen(content, "r"))) return 1;
   while (fgets(line, sizeof(line), f)) if (line[0] && line[0] != '#' && line[0] != '\r' && line[0] != '\n') n++;
   fclose(f);
   return n > 1 ? n : 1;
   }
}
/* OPEN in a game: pause it and ask (lumen --dialog), then act on the answer */
static void open_in_game(void)
{
   char cmd[700];
   int discs = game_discs(), rc;
   ra_cmd("PAUSE_TOGGLE");
   snprintf(cmd, sizeof(cmd), "SDL_VIDEODRIVER=wayland XDG_RUNTIME_DIR=/run/user/0 %s/lumen --home %s --dialog %s"
            " --log /tmp/lumen-dialog.log", g_home, g_home, discs > 1 ? "disc" : "game");
   signal(SIGCHLD, SIG_DFL);
   rc = system(cmd);
   signal(SIGCHLD, SIG_IGN);
   rc = WIFEXITED(rc) ? WEXITSTATUS(rc) : 0;
   logf_("open: discs %d, answer %d (%s)", discs, rc, rc == 1 ? "change disc" : rc == 2 ? "exit" : "continue");
   if (rc == 1)
   {
      ra_cmd("DISK_EJECT_TOGGLE"); usleep(300000);
      ra_cmd("DISK_NEXT"); usleep(300000);
      ra_cmd("DISK_EJECT_TOGGLE"); usleep(200000);
      ra_cmd("PAUSE_TOGGLE");
   }
   else if (rc == 2) { ra_cmd("PAUSE_TOGGLE"); usleep(100000); ra_cmd("QUIT"); }
   else ra_cmd("PAUSE_TOGGLE");
}

int main(int argc, char **argv)
{
   const char *log_path = NULL, *power_dir = "/data/power";
   char dev[64], prep[200], okay[200], rcount[200];
   int i, fd, acked = 0, paused = 0, demo_seconds = -1, demo_pid = 0;
   long resume0 = -1;
   time_t paused_at = 0;

   for (i = 1; i < argc; i++)
   {
      if (!strcmp(argv[i], "--log") && i + 1 < argc) log_path = argv[++i];
      else if (!strcmp(argv[i], "--power-dir") && i + 1 < argc) power_dir = argv[++i];
      else if (!strcmp(argv[i], "--demo-watch") && i + 1 < argc) demo_seconds = atoi(argv[++i]);
      else if (!strcmp(argv[i], "--pid") && i + 1 < argc) demo_pid = atoi(argv[++i]);
   }
   g_log = log_path ? fopen(log_path, "a") : NULL;
   if (!g_log) g_log = stderr;
   atexit(log_sync_atexit);
   if (demo_seconds >= 0)
   {
      if (demo_seconds < 1 || demo_pid <= 1) { logf_("demo-watch: invalid seconds/pid"); return 3; }
      return demo_watch(demo_seconds, (pid_t)demo_pid);
   }
   snprintf(prep, sizeof(prep), "%s/prepare_suspend", power_dir);
   snprintf(okay, sizeof(okay), "%s/apps_okay", power_dir);
   snprintf(rcount, sizeof(rcount), "%s/resume_count", power_dir);
   signal(SIGCHLD, SIG_IGN);   /* power.sh resume runs detached */

   fd = open_buttons(dev, sizeof(dev));
   if (fd < 0) { logf_("gpio-keys not found: %s", strerror(errno)); return 1; }
   logf_("watching %s, power handshake in %s", dev, power_dir);
   /* ready marker for boot.sh */
   printf("ready\n"); fflush(stdout);

   for (;;)
   {
      struct pollfd pfd = { fd, POLLIN, 0 };
      int r = poll(&pfd, 1, 150);
      log_tick();

      /* suspend handshake: answer once per request, after pausing the USB users */
      if (exists(prep))
      {
         if (!acked)
         {
            int f;
            logf_("power: suspend requested: pausing the session");
            log_sync(1);
            resume0 = read_long(rcount);
            signal(SIGCHLD, SIG_DFL); run_power("suspend", 1); signal(SIGCHLD, SIG_IGN);
            paused = 1; paused_at = time(NULL);
            f = open(okay, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (f >= 0) close(f);
            acked = 1;
            logf_("power: apps_okay sent (resume_count %ld)", resume0);
            log_sync(1);
         }
      }
      else if (acked) acked = 0;
      /* woke up (resume_count moved), or the suspend never happened (60 s) */
      if (paused)
      {
         long rc = read_long(rcount);
         int woke = rc >= 0 && resume0 >= 0 && rc != resume0;
         if (woke || time(NULL) - paused_at > 60)
         {
            paused = 0;
            run_power("resume", 0);
            logf_("power: %s (resume_count %ld -> %ld)", woke ? "resumed" : "no suspend after 60 s, resuming", resume0, rc);
         }
      }

      if (r > 0 && (pfd.revents & POLLIN))
      {
         struct input_event ev;
         while (read(fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev))
         {
            if (ev.type != EV_KEY || ev.value != 1) continue;
            if (ev.code == KEY_PLAYPAUSE)
            {  /* RESET: restart the game, or the dashboard */
               pid_t ra = find_proc("retroarch"), lu = find_proc("lumen");
               logf_("reset pressed: %s", ra ? "resetting the game" : "restarting the dashboard");
               if (ra) ra_cmd("RESET");
               else if (lu) { int f = open("/tmp/lumen-restart", O_WRONLY | O_CREAT, 0644); if (f >= 0) close(f); kill(lu, SIGTERM); }
            }
            else if (ev.code == KEY_EJECTCD)
            {  /* OPEN: in a game, pause and ask (change disc / exit / continue) */
               pid_t ra = find_proc("retroarch");
               logf_("open pressed%s", ra ? "" : " (dashboard: nothing)");
               if (ra && !find_proc("lumen")) open_in_game();
            }
            else if (ev.code == KEY_SLEEP) logf_("power pressed");
            else logf_("key %d pressed", ev.code);
         }
      }
   }
   return 0;
}
