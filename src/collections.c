/* Collections (favourites, folders) and resume marks, see collections.h. */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include "lumen.h"
#include "collections.h"
#include "settings.h"

#define COLL_MAX_ENTRIES 1024
#define RESUME_MAX       256

typedef struct { int folder; char content[512]; } entry_t;   /* folder -1 = favourite */

static entry_t  s_e[COLL_MAX_ENTRIES];
static int      s_ne;
static char     s_folder[COLL_MAX_FOLDERS][COLL_NAME_MAX + 4];
static int      s_nf;
static char     s_path[400], s_resume_path[400];
static unsigned s_gen = 1;
static char     s_resume[RESUME_MAX][512];
static int      s_nr;

static int find_entry(int folder, const char *content)
{
   int i;
   for (i = 0; i < s_ne; i++) if (s_e[i].folder == folder && !strcmp(s_e[i].content, content)) return i;
   return -1;
}

int coll_folder_find(const char *name)
{
   int f;
   for (f = 0; f < s_nf; f++) if (!strcasecmp(s_folder[f], name)) return f;
   return -1;
}

static int folder_add(const char *name)
{
   int f = coll_folder_find(name);
   if (f >= 0) return f;
   if (s_nf >= COLL_MAX_FOLDERS) return -1;
   snprintf(s_folder[s_nf], sizeof(s_folder[0]), "%s", name);
   return s_nf++;
}

static void entry_add(int folder, const char *content)
{
   if (s_ne >= COLL_MAX_ENTRIES || !content[0] || find_entry(folder, content) >= 0) return;
   s_e[s_ne].folder = folder;
   snprintf(s_e[s_ne].content, sizeof(s_e[0].content), "%s", content);
   s_ne++;
}

static void entry_del(int i)
{
   if (i < 0 || i >= s_ne) return;
   memmove(&s_e[i], &s_e[i + 1], (size_t)(s_ne - i - 1) * sizeof(entry_t));
   s_ne--;
}

/* drop folders left without games, renumbering the entries */
static void prune_folders(void)
{
   int f = 0, i;
   while (f < s_nf)
   {
      int used = 0;
      for (i = 0; i < s_ne; i++) if (s_e[i].folder == f) { used = 1; break; }
      if (used) { f++; continue; }
      memmove(s_folder[f], s_folder[f + 1], (size_t)(s_nf - f - 1) * sizeof(s_folder[0]));
      s_nf--;
      for (i = 0; i < s_ne; i++) if (s_e[i].folder > f) s_e[i].folder--;
   }
}

static int save(void)
{
   char tmp[420];
   FILE *f;
   int i, ok;
   if (!s_path[0]) return -1;
   {  /* the profile folder exists for every profile but main on a fresh stick */
      char dir[400], *sl;
      snprintf(dir, sizeof(dir), "%s", s_path);
      if ((sl = strrchr(dir, '/'))) { *sl = 0; mkdir(dir, 0755); }
   }
   snprintf(tmp, sizeof(tmp), "%s.tmp", s_path);
   if (!(f = fopen(tmp, "w"))) { logf_("collections: cannot write %s: %s", tmp, strerror(errno)); return -1; }
   ok = 1;
   for (i = 0; i < s_ne && ok; i++)
   {
      if (s_e[i].folder < 0) ok = fprintf(f, "fav\t%s\n", s_e[i].content) > 0;
      else ok = fprintf(f, "folder\t%s\t%s\n", s_folder[s_e[i].folder], s_e[i].content) > 0;
   }
   if (ok) ok = fflush(f) == 0 && fsync(fileno(f)) == 0;
   if (fclose(f) != 0) ok = 0;
   if (ok) ok = rename(tmp, s_path) == 0;
   if (!ok) { logf_("collections: save failed: %s", strerror(errno)); unlink(tmp); return -1; }
   {
      char dir[400], *sl;
      snprintf(dir, sizeof(dir), "%s", s_path);
      if ((sl = strrchr(dir, '/'))) { *sl = 0; settings_fsync_dir(dir); }
   }
   return 0;
}

static void changed(void) { s_gen++; save(); }

static void resume_load(void)
{
   char line[700];
   FILE *f;
   s_nr = 0;
   if (!(f = fopen(s_resume_path, "r"))) return;
   while (fgets(line, sizeof(line), f) && s_nr < RESUME_MAX)
   {
      char *tab;
      line[strcspn(line, "\r\n")] = 0;
      if ((tab = strchr(line, '\t'))) *tab = 0;
      if (!line[0]) continue;
      snprintf(s_resume[s_nr++], sizeof(s_resume[0]), "%s", line);
   }
   fclose(f);
}

void coll_load(const char *home, const char *profile_id)
{
   char line[700];
   FILE *f;
   s_ne = s_nf = 0;
   snprintf(s_path, sizeof(s_path), "%s/profiles/%s/collections.tsv", home, profile_id);
   snprintf(s_resume_path, sizeof(s_resume_path), "%s/profiles/%s/resume.tsv", home, profile_id);
   s_gen++;
   resume_load();
   if (!(f = fopen(s_path, "r"))) return;
   while (fgets(line, sizeof(line), f))
   {
      char *a, *b;
      line[strcspn(line, "\r\n")] = 0;
      if (!(a = strchr(line, '\t'))) continue;
      *a++ = 0;
      if (!strcmp(line, "fav")) entry_add(-1, a);
      else if (!strcmp(line, "folder") && (b = strchr(a, '\t')))
      {
         int fi;
         *b++ = 0;
         if (!a[0] || (fi = folder_add(a)) < 0) continue;
         entry_add(fi, b);
      }
   }
   fclose(f);
   logf_("collections: %d entries, %d folders, %d resume marks (%s)", s_ne, s_nf, s_nr, profile_id);
}

int coll_is_fav(const char *content) { return find_entry(-1, content) >= 0; }

void coll_toggle_fav(const char *content)
{
   int i = find_entry(-1, content);
   if (i >= 0) entry_del(i); else entry_add(-1, content);
   changed();
}

int         coll_nfolders(void) { return s_nf; }
const char *coll_folder_name(int f) { return f >= 0 && f < s_nf ? s_folder[f] : ""; }
int         coll_in_folder(int f, const char *content) { return find_entry(f, content) >= 0; }

void coll_toggle_folder(int f, const char *content)
{
   int i;
   if (f < 0 || f >= s_nf) return;
   i = find_entry(f, content);
   if (i >= 0) { entry_del(i); prune_folders(); } else entry_add(f, content);
   changed();
}

int coll_create_folder(const char *name, const char *content)
{
   int f;
   if (!name || !name[0]) return -3;
   if (coll_folder_find(name) >= 0) return -2;
   if (s_nf >= COLL_MAX_FOLDERS || s_ne >= COLL_MAX_ENTRIES) return -1;
   f = folder_add(name);
   entry_add(f, content);
   changed();
   return 0;
}

int coll_fav_count(void) { int i, n = 0; for (i = 0; i < s_ne; i++) n += s_e[i].folder < 0; return n; }
const char *coll_fav_at(int k)
{
   int i;
   for (i = 0; i < s_ne; i++) if (s_e[i].folder < 0 && k-- == 0) return s_e[i].content;
   return NULL;
}
int coll_folder_count(int f) { int i, n = 0; for (i = 0; i < s_ne; i++) n += s_e[i].folder == f; return n; }
const char *coll_folder_at(int f, int k)
{
   int i;
   for (i = 0; i < s_ne; i++) if (s_e[i].folder == f && k-- == 0) return s_e[i].content;
   return NULL;
}
unsigned coll_generation(void) { return s_gen; }

int resume_has(const char *content)
{
   int i;
   for (i = 0; i < s_nr; i++) if (!strcmp(s_resume[i], content)) return 1;
   return 0;
}

void resume_forget(const char *content)
{
   char tmp[420], line[700];
   FILE *in, *out;
   int i;
   for (i = 0; i < s_nr; i++)
      if (!strcmp(s_resume[i], content))
      {
         memmove(s_resume[i], s_resume[i + 1], (size_t)(s_nr - i - 1) * sizeof(s_resume[0]));
         s_nr--; i--;
      }
   /* rewrite the file without that content's lines (keeps the others' epochs) */
   snprintf(tmp, sizeof(tmp), "%s.tmp", s_resume_path);
   if (!(in = fopen(s_resume_path, "r"))) return;
   if (!(out = fopen(tmp, "w"))) { fclose(in); return; }
   while (fgets(line, sizeof(line), in))
   {
      size_t n = strlen(content);
      if (!strncmp(line, content, n) && (line[n] == '\t' || line[n] == '\n' || line[n] == '\r' || !line[n])) continue;
      fputs(line, out);
   }
   fclose(in);
   fflush(out); fsync(fileno(out));
   fclose(out);
   if (rename(tmp, s_resume_path) != 0) unlink(tmp);
   logf_("resume: forgot %s", content);
}
