/* Per-profile play statistics. scripts/run.sh appends one line per finished normal
 * (non-demo) session to <home>/profiles/<profile>/playlog.tsv:
 *    <start epoch>\t<seconds>\t<system id>\t<content path>
 * (epoch from `date +%s`: on a console without synced time it is from 2018, so "last
 * played" is only shown when it is >= 2024-01-01). Lumen only reads the file. */
#ifndef LUMEN_STATS_H
#define LUMEN_STATS_H

typedef struct {
   long seconds;           /* total play time of this content */
   long last_epoch;        /* start of the most recent session, 0 = unknown/unsynced */
   int  sessions;
} game_stats_t;

void stats_load(const char *home, const char *profile_id);   /* (re)read; call again when the profile changes */
const game_stats_t *stats_get(const char *content);          /* NULL = never played by this profile */
long stats_total_seconds(void);                              /* all games, this profile */
int  stats_games_played(void);                               /* distinct contents */
/* "2 h 15 min" / "45 min" / "< 1 min" in the active language (keys time.hm, time.m, time.lt1) */
const char *stats_format_duration(char *buf, unsigned n, long seconds);
/* "14 nov 2026" style date in the active language (keys month.1 .. month.12, date.dmy) */
const char *stats_format_date(char *buf, unsigned n, long epoch);

#endif
