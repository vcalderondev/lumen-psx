/* Launch request v2 writer (docs/settings-design.md 7.5). */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "lumen.h"
#include "launch.h"

static int one_line(const char *s) { return !s || !strpbrk(s, "\n\r"); }

static int match(const char *s, int lower_only)
{
   if (!s || !*s) return 0;
   for (; *s; s++)
   {
      char c = *s;
      if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') continue;
      if (c == '-') continue;   /* profile ids; core option keys (mupen64plus-...) */
      if (!lower_only && c >= 'A' && c <= 'Z') continue;
      return 0;
   }
   return 1;
}

/* "k=v, k=v" -> "<prefix>k=v" lines; invalid pairs are skipped and logged */
static int write_list(FILE *f, const char *prefix, const char *list)
{
   char item[200];
   const char *p = list;
   int ok = 1;
   while (p && *p && ok)
   {
      const char *e = strchr(p, ',');
      size_t n = e ? (size_t)(e - p) : strlen(p);
      char *k, *v, *eq, *end;
      if (n >= sizeof(item)) n = sizeof(item) - 1;
      memcpy(item, p, n); item[n] = 0;
      p = e ? e + 1 : NULL;
      k = item;
      while (*k == ' ' || *k == '\t') k++;
      end = k + strlen(k);
      while (end > k && (end[-1] == ' ' || end[-1] == '\t')) *--end = 0;
      if (!*k) continue;
      if (!(eq = strchr(k, '='))) { logf_("launch: skipped %s pair without '=': %s", prefix, k); continue; }
      *eq = 0; v = eq + 1;
      end = eq;
      while (end > k && (end[-1] == ' ' || end[-1] == '\t')) *--end = 0;
      while (*v == ' ' || *v == '\t') v++;
      if (!match(k, 0) || strpbrk(v, "\",=\n\r")) { logf_("launch: skipped invalid %s pair: %s", prefix, k); continue; }
      ok = fprintf(f, "%s%s=%s\n", prefix, k, v) > 0;
   }
   return ok;
}

int launch_write(const char *path, const launch_req_t *r)
{
   char tmp[300];
   FILE *f;
   int ok;
   if (!r || !r->core || !r->content || !r->system || !r->profile) return -1;
   if (!one_line(r->core) || !one_line(r->content) || !one_line(r->system) || !one_line(r->title) ||
       !one_line(r->graphics) || !one_line(r->opt_core_name))
   { logf_("launch: rejected a request with a line break in a value"); return -1; }
   if (!match(r->profile, 1)) { logf_("launch: rejected invalid profile id %s", r->profile); return -1; }
   snprintf(tmp, sizeof(tmp), "%s.tmp", path);
   if (!(f = fopen(tmp, "w"))) { logf_("launch: cannot write %s: %s", tmp, strerror(errno)); return -2; }
   ok = fprintf(f, "version=2\ncore=%s\ncontent=%s\nsystem=%s\ntitle=%s\nprofile=%s\nmode=%s\ndemo_seconds=%d\n",
                r->core, r->content, r->system, r->title ? r->title : "", r->profile,
                r->demo ? "demo" : "normal", r->demo_seconds) > 0;
   if (ok && r->resume) ok = fprintf(f, "resume=1\n") > 0;
   if (ok && r->app) ok = fprintf(f, "kind=app\n") > 0;
   if (ok && r->crt && match(r->crt, 1)) ok = fprintf(f, "crt=%s\n", r->crt) > 0;
   if (ok && r->graphics) ok = fprintf(f, "graphics=%s\n", r->graphics) > 0;
   if (ok && r->opt_core_name && r->opt_core_name[0]) ok = fprintf(f, "opt_core_name=%s\n", r->opt_core_name) > 0;
   if (ok && r->opt_list) ok = write_list(f, "opt.", r->opt_list);
   if (ok && r->ra_list) ok = write_list(f, "ra.", r->ra_list);
   if (ok) ok = fflush(f) == 0;
   if (fclose(f) != 0) ok = 0;
   if (ok) ok = rename(tmp, path) == 0;
   if (!ok) { logf_("launch: writing %s failed: %s", path, strerror(errno)); unlink(tmp); return -2; }
   return 0;
}
