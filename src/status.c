/* Top-right status: clock (only when the time looks synced) and network, polled every 2 s. */
#define _GNU_SOURCE
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "lumen.h"
#include "status.h"

static status_t s_cache;
static double   s_last;
static int      s_valid;

/* first line of a small sysfs file, trailing newline stripped; "" on failure */
static void read_line(const char *path, char *buf, size_t n)
{
   FILE *f = fopen(path, "r");
   buf[0] = 0;
   if (!f) return;
   if (fgets(buf, (int)n, f)) buf[strcspn(buf, "\r\n")] = 0;
   else buf[0] = 0;
   fclose(f);
}

static int net_up(void)
{
   DIR *d = opendir("/sys/class/net");
   struct dirent *e;
   char path[320], state[32], carrier[8];
   int up = 0;
   if (!d) return 0;
   while (!up && (e = readdir(d)))
   {
      if (e->d_name[0] == '.' || !strcmp(e->d_name, "lo")) continue;
      snprintf(path, sizeof path, "/sys/class/net/%s/operstate", e->d_name);
      read_line(path, state, sizeof state);
      if (!strcmp(state, "up")) up = 1;
      else if (!strcmp(state, "unknown"))
      {
         snprintf(path, sizeof path, "/sys/class/net/%s/carrier", e->d_name);
         read_line(path, carrier, sizeof carrier);   /* read fails (EINVAL) when the link is down */
         up = !strcmp(carrier, "1");
      }
   }
   closedir(d);
   return up;
}

static void refresh(void)
{
   time_t now = time(NULL);
   struct tm tm;
   memset(&s_cache, 0, sizeof s_cache);
   if (localtime_r(&now, &tm) && tm.tm_year + 1900 >= 2024)
   {
      s_cache.clock_ok = 1;
      strftime(s_cache.clock, sizeof s_cache.clock, "%H:%M", &tm);
   }
   s_cache.net_up = net_up();
}

void status_update(double t, status_t *out)
{
   if (!s_valid || t - s_last >= 2.0 || t < s_last)
   {
      refresh();
      s_last = t;
      s_valid = 1;
   }
   if (out) *out = s_cache;
}
