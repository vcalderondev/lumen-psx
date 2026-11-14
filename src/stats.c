/* Per-profile play statistics from <home>/profiles/<profile>/playlog.tsv (written by
 * scripts/run.sh). Read once per profile into an open-addressing hash table keyed by
 * content path. Malformed lines are skipped; only the last STATS_TAIL bytes of a huge
 * file are read. */
#define _GNU_SOURCE
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "lumen.h"
#include "stats.h"
#include "i18n.h"

#define STATS_MAX     2048
#define STATS_SLOTS   4096                 /* power of two, 2x STATS_MAX */
#define STATS_TAIL    (4L << 20)           /* bytes read from the end of a huge log */
#define STATS_EPOCH_MIN 1704067200L        /* 2024-01-01: older epochs are an unsynced clock */
#define STATS_SESSION_MAX (30L * 86400)    /* longer sessions are treated as garbage */

typedef struct { char *content; game_stats_t st; } entry_t;

static entry_t s_ent[STATS_MAX];
static int     s_n;
static short   s_slot[STATS_SLOTS];        /* index + 1 into s_ent, 0 = empty */
static long    s_total;

static unsigned hash_str(const char *s)
{
   unsigned h = 2166136261u;
   while (*s) h = (h ^ (unsigned char)*s++) * 16777619u;
   return h;
}

/* slot holding content, or the empty slot where it would go */
static unsigned find_slot(const char *content)
{
   unsigned i = hash_str(content) & (STATS_SLOTS - 1);
   while (s_slot[i] && strcmp(s_ent[s_slot[i] - 1].content, content))
      i = (i + 1) & (STATS_SLOTS - 1);
   return i;
}

static void clear(void)
{
   int i;
   for (i = 0; i < s_n; i++) free(s_ent[i].content);
   memset(s_ent, 0, sizeof s_ent);
   memset(s_slot, 0, sizeof s_slot);
   s_n = 0;
   s_total = 0;
}

/* 1..10 digits terminated by '\t', value <= LONG_MAX; advances *p past the tab */
static int parse_num(char **p, long *out)
{
   char *s = *p;
   long long v = 0;
   int d = 0;
   while (*s >= '0' && *s <= '9' && d < 10) { v = v * 10 + (*s++ - '0'); d++; }
   if (!d || *s != '\t' || v > LONG_MAX) return -1;
   *out = v;
   *p = s + 1;
   return 0;
}

static void add_line(char *line)
{
   char *p = line, *content;
   long epoch, secs;
   unsigned slot;
   entry_t *e;
   if (parse_num(&p, &epoch) || parse_num(&p, &secs)) return;
   if (secs <= 0 || secs > STATS_SESSION_MAX) return;
   content = strchr(p, '\t');              /* skip the system id */
   if (!content) return;
   content++;
   content[strcspn(content, "\t\r\n")] = 0;
   if (!*content) return;
   s_total += secs;
   slot = find_slot(content);
   if (!s_slot[slot])
   {
      if (s_n >= STATS_MAX) return;        /* table full: only the total counts */
      if (!(s_ent[s_n].content = strdup(content))) return;
      s_slot[slot] = (short)(++s_n);
   }
   e = &s_ent[s_slot[slot] - 1];
   e->st.seconds += secs;
   e->st.sessions++;
   if (epoch >= STATS_EPOCH_MIN && epoch > e->st.last_epoch) e->st.last_epoch = epoch;
}

static int valid_id(const char *id)
{
   const char *c;
   if (!id || !*id) return 0;
   for (c = id; *c; c++)
      if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '_' || *c == '-')) return 0;
   return 1;
}

void stats_load(const char *home, const char *profile_id)
{
   char path[512], line[2048];
   FILE *f;
   long size;
   int skip = 0, lines = 0;
   clear();
   if (!home || !valid_id(profile_id)) return;
   snprintf(path, sizeof path, "%s/profiles/%s/playlog.tsv", home, profile_id);
   if (!(f = fopen(path, "r"))) return;
   if (!fseek(f, 0, SEEK_END) && (size = ftell(f)) > STATS_TAIL)
   {
      fseek(f, size - STATS_TAIL, SEEK_SET);
      skip = 1;                            /* first line is partial */
   }
   else rewind(f);
   while (fgets(line, sizeof line, f))
   {
      size_t len = strlen(line);
      int whole = len > 0 && line[len - 1] == '\n';
      if (!skip && (whole || feof(f))) { add_line(line); lines++; }
      skip = !whole && !feof(f);           /* overlong line: drop its remainder */
   }
   fclose(f);
   logf_("stats: profile %s, %d lines, %d games, %ld s", profile_id, lines, s_n, s_total);
}

const game_stats_t *stats_get(const char *content)
{
   unsigned slot;
   if (!content || !*content || !s_n) return NULL;
   slot = find_slot(content);
   return s_slot[slot] ? &s_ent[s_slot[slot] - 1].st : NULL;
}

long stats_total_seconds(void) { return s_total; }
int  stats_games_played(void)  { return s_n; }

const char *stats_format_duration(char *buf, unsigned n, long seconds)
{
   char hs[24], ms[8];
   long m = seconds / 60;
   if (!buf || !n) return "";
   if (m < 1)
   {
      snprintf(buf, n, "%s", tr("time.lt1"));
      return buf;
   }
   snprintf(ms, sizeof ms, "%ld", m % 60);
   if (m < 60) return trf(buf, n, "time.m", "m", ms, NULL);
   snprintf(hs, sizeof hs, "%ld", m / 60);
   return trf(buf, n, "time.hm", "h", hs, "m", ms, NULL);
}

const char *stats_format_date(char *buf, unsigned n, long epoch)
{
   char key[12], ds[4], ys[8];
   time_t t = (time_t)epoch;
   struct tm tm;
   if (!buf || !n) return "";
   buf[0] = 0;
   if (epoch <= 0 || !localtime_r(&t, &tm)) return buf;
   snprintf(key, sizeof key, "month.%d", tm.tm_mon + 1);
   snprintf(ds, sizeof ds, "%d", tm.tm_mday);
   snprintf(ys, sizeof ys, "%d", tm.tm_year + 1900);
   return trf(buf, n, "date.dmy", "d", ds, "month", tr(key), "y", ys, NULL);
}
