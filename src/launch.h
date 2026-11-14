/* Launch request v2 for scripts/run.sh (docs/settings-design.md 7.5):
 * "key=value" lines, value = rest of the line, written atomically (tmp + rename). */
#ifndef LUMEN_LAUNCH_H
#define LUMEN_LAUNCH_H

typedef struct {
   const char *core, *content, *system, *title, *profile;
   int demo, demo_seconds;
   const char *graphics, *opt_core_name;   /* NULL until step 4 */
   const char *opt_list, *ra_list;         /* "k=v, k=v" from the declaration, or NULL */
} launch_req_t;

int launch_write(const char *path, const launch_req_t *r);   /* 0 ok, -1 invalid, -2 I/O */

#endif
