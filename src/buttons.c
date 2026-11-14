/*
 * lumen-buttons — console buttons for a Lumen session (PlayStation Classic).
 *
 * The console's front buttons are a gpio-keys input device:
 *   POWER = KEY_SLEEP (142, wake source), RESET = KEY_PLAYPAUSE (164), OPEN = KEY_EJECTCD (161)
 *
 * POWER is handled by the system power manager, which suspends the console
 * after a handshake: it creates <power>/prepare_suspend and waits for the app
 * in front to answer with <power>/apps_okay. This daemon answers it (after a
 * sync), so POWER suspends/resumes the console in the menu and in games.
 * RESET quits a running RetroArch, which returns the session to Lumen.
 * OPEN is logged (reserved for disc swapping).
 *
 * Usage: lumen-buttons [--log file] [--power-dir /data/power]
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
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static FILE *g_log;
static void logf_(const char *fmt, ...)
{
   va_list ap; time_t now = time(NULL); char ts[32];
   strftime(ts, sizeof(ts), "%H:%M:%S", localtime(&now));
   fprintf(g_log, "%s ", ts);
   va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap);
   fputc('\n', g_log); fflush(g_log);
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

int main(int argc, char **argv)
{
   const char *log_path = NULL, *power_dir = "/data/power";
   char dev[64], prep[200], okay[200];
   int i, fd, acked = 0;

   for (i = 1; i < argc; i++)
   {
      if (!strcmp(argv[i], "--log") && i + 1 < argc) log_path = argv[++i];
      else if (!strcmp(argv[i], "--power-dir") && i + 1 < argc) power_dir = argv[++i];
   }
   g_log = log_path ? fopen(log_path, "a") : NULL;
   if (!g_log) g_log = stderr;
   snprintf(prep, sizeof(prep), "%s/prepare_suspend", power_dir);
   snprintf(okay, sizeof(okay), "%s/apps_okay", power_dir);

   fd = open_buttons(dev, sizeof(dev));
   if (fd < 0) { logf_("gpio-keys not found: %s", strerror(errno)); return 1; }
   logf_("watching %s, power handshake in %s", dev, power_dir);
   /* ready marker for boot.sh */
   printf("ready\n"); fflush(stdout);

   for (;;)
   {
      struct pollfd pfd = { fd, POLLIN, 0 };
      int r = poll(&pfd, 1, 150);

      /* power handshake: answer once per suspend request */
      if (exists(prep))
      {
         if (!acked)
         {
            int f;
            sync();
            f = open(okay, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (f >= 0) close(f);
            acked = 1;
            logf_("power: suspend requested -> apps_okay");
         }
      }
      else if (acked) { acked = 0; logf_("power: resumed"); }

      if (r > 0 && (pfd.revents & POLLIN))
      {
         struct input_event ev;
         while (read(fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev))
         {
            if (ev.type != EV_KEY || ev.value != 1) continue;
            if (ev.code == KEY_PLAYPAUSE)
            {
               pid_t ra = find_proc("retroarch");
               logf_("reset pressed%s", ra ? ": quitting RetroArch" : "");
               if (ra) kill(ra, SIGTERM);
            }
            else if (ev.code == KEY_EJECTCD) logf_("open pressed");
            else if (ev.code == KEY_SLEEP) logf_("power pressed");
            else logf_("key %d pressed", ev.code);
         }
      }
   }
   return 0;
}
