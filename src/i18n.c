/* Translation layer (docs/settings-design.md section 5).
 * Each loaded language is one heap block: the entry index followed by the file text,
 * parsed in place and sorted for bsearch. Loading happens only at start and on a
 * language change, never per frame. */
#include <ctype.h>
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lumen.h"
#include "i18n.h"

typedef struct { const char *k, *v; } entry_t;
typedef struct { char code[16]; void *block; entry_t *e; int n; } lang_t;
typedef struct { char code[16]; char name[48]; char main_profile[64]; } lang_info_t;

static lang_info_t s_list[I18N_MAX_LANGS];
static int         s_nlist;
static lang_t      s_fb;            /* "en", always kept loaded as fallback */
static lang_t      s_act;           /* active language when it is not "en"  */
static lang_t     *s_cur;           /* &s_act, &s_fb or NULL                */
static unsigned    s_gen;
static char        s_dir[300];

static int cmp_entry(const void *a, const void *b)
{
   return strcmp(((const entry_t *)a)->k, ((const entry_t *)b)->k);
}

static char *strip(char *s)
{
   char *e;
   while (*s && isspace((unsigned char)*s)) s++;
   e = s + strlen(s);
   while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
   return s;
}

static void lang_free(lang_t *L)
{
   free(L->block);
   memset(L, 0, sizeof(*L));
}

/* read <dir>/<code>.lang into one block; 0 ok, -1 error */
static int lang_load(lang_t *L, const char *code)
{
   char path[340], *text, *p;
   FILE *f; long sz; size_t max = 1, i; entry_t *e; int n = 0;
   memset(L, 0, sizeof(*L));
   snprintf(path, sizeof(path), "%s/%s.lang", s_dir, code);
   if (!(f = fopen(path, "rb"))) return -1;
   fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
   if (sz <= 0 || sz > (1 << 20)) { fclose(f); logf_("i18n: bad size %ld for %s", sz, path); return -1; }
   /* upper bound of entries = number of lines; count them in a first pass */
   text = malloc((size_t)sz + 1);
   if (!text) { fclose(f); return -1; }
   if (fread(text, 1, (size_t)sz, f) != (size_t)sz) { free(text); fclose(f); return -1; }
   fclose(f);
   text[sz] = 0;
   for (i = 0; i < (size_t)sz; i++) if (text[i] == '\n') max++;
   L->block = malloc(max * sizeof(entry_t) + (size_t)sz + 1);
   if (!L->block) { free(text); return -1; }
   e = L->block;
   p = (char *)(e + max);
   memcpy(p, text, (size_t)sz + 1);
   free(text);
   if (!strncmp(p, "\xEF\xBB\xBF", 3)) p += 3;
   while (*p)
   {
      char *line = p, *nl = strchr(p, '\n'), *k, *v, *eq, *r, *w;
      if (nl) { *nl = 0; p = nl + 1; } else p += strlen(p);
      k = strip(line);
      if (!*k || *k == '#' || !(eq = strchr(k, '='))) continue;
      *eq = 0;
      k = strip(k); v = strip(eq + 1);
      if (!*k) continue;
      for (r = w = v; *r; r++)   /* "\n" -> line break, "\\" -> backslash */
      {
         if (r[0] == '\\' && r[1] == 'n') { *w++ = '\n'; r++; }
         else if (r[0] == '\\' && r[1] == '\\') { *w++ = '\\'; r++; }
         else *w++ = *r;
      }
      *w = 0;
      e[n].k = k; e[n].v = v; n++;
   }
   qsort(e, n, sizeof(entry_t), cmp_entry);
   L->e = e; L->n = n;
   snprintf(L->code, sizeof(L->code), "%s", code);
   return 0;
}

static const char *lang_find(const lang_t *L, const char *key)
{
   entry_t k, *r;
   if (!L || !L->n) return NULL;
   k.k = key; k.v = NULL;
   r = bsearch(&k, L->e, L->n, sizeof(entry_t), cmp_entry);
   return r ? r->v : NULL;
}

static int cmp_info(const void *a, const void *b)
{
   return strcmp(((const lang_info_t *)a)->code, ((const lang_info_t *)b)->code);
}

int i18n_init(const char *home)
{
   DIR *d; struct dirent *de;
   s_nlist = 0; s_cur = NULL;
   lang_free(&s_act); lang_free(&s_fb);
   snprintf(s_dir, sizeof(s_dir), "%s/lang", home);
   if (!(d = opendir(s_dir)))
   {
      logf_("i18n: warning: %s missing, showing translation keys", s_dir);
      return 0;
   }
   while ((de = readdir(d)) && s_nlist < I18N_MAX_LANGS)
   {
      size_t l = strlen(de->d_name);
      lang_info_t *li = &s_list[s_nlist];
      lang_t tmp; const char *nm;
      if (de->d_name[0] == '.' || l < 6 || l - 5 >= sizeof(li->code) || strcmp(de->d_name + l - 5, ".lang")) continue;
      memcpy(li->code, de->d_name, l - 5); li->code[l - 5] = 0;
      if (lang_load(&tmp, li->code)) { logf_("i18n: cannot read %s/%s", s_dir, de->d_name); continue; }
      nm = lang_find(&tmp, "lang.name");
      snprintf(li->name, sizeof(li->name), "%s", nm ? nm : li->code);
      nm = lang_find(&tmp, "profile.main");
      snprintf(li->main_profile, sizeof(li->main_profile), "%s", nm ? nm : "");
      lang_free(&tmp);
      s_nlist++;
   }
   closedir(d);
   qsort(s_list, s_nlist, sizeof(lang_info_t), cmp_info);
   if (lang_load(&s_fb, "en")) logf_("i18n: warning: no en.lang fallback in %s", s_dir);
   else s_cur = &s_fb;
   logf_("i18n: %d languages in %s", s_nlist, s_dir);
   return s_nlist;
}

int i18n_set_language(const char *code)
{
   int i, known = 0;
   lang_t tmp;
   if (!code || !*code) return -1;
   if (s_cur && !strcmp(s_cur->code, code)) return 0;
   for (i = 0; i < s_nlist; i++) if (!strcmp(s_list[i].code, code)) known = 1;
   if (!known)
   {
      if (s_nlist) logf_("i18n: unknown language '%s', keeping '%s'", code, i18n_language());
      return -1;
   }
   if (!strcmp(code, "en") && s_fb.n)
   {
      lang_free(&s_act);
      s_cur = &s_fb;
   }
   else
   {
      if (lang_load(&tmp, code)) { logf_("i18n: cannot load '%s'", code); return -1; }
      lang_free(&s_act);
      s_act = tmp;
      s_cur = &s_act;
   }
   s_gen++;
   logf_("i18n: language %s (%d strings)", code, s_cur->n);
   return 0;
}

const char *i18n_language(void) { return s_cur ? s_cur->code : ""; }
int         i18n_count(void) { return s_nlist; }
const char *i18n_code(int i) { return i >= 0 && i < s_nlist ? s_list[i].code : ""; }
const char *i18n_name(int i) { return i >= 0 && i < s_nlist ? s_list[i].name : ""; }
const char *i18n_main_profile_name(int i) { return i >= 0 && i < s_nlist ? s_list[i].main_profile : ""; }
unsigned    i18n_generation(void) { return s_gen; }

const char *tr(const char *key)
{
   const char *v = lang_find(s_cur, key);
   if (!v && s_cur != &s_fb) v = lang_find(&s_fb, key);
   return v ? v : key;
}

/* value of placeholder <name,len> among the NULL-terminated name/value pairs, or NULL */
static const char *arg_find(const char *name, size_t len, va_list ap)
{
   va_list cp; const char *k, *v, *r = NULL;
   va_copy(cp, ap);
   while ((k = va_arg(cp, const char *)))
   {
      v = va_arg(cp, const char *);
      if (strlen(k) == len && !memcmp(k, name, len)) { r = v ? v : ""; break; }
   }
   va_end(cp);
   return r;
}

const char *trf(char *buf, size_t n, const char *key, ...)
{
   const char *s = tr(key);
   size_t o = 0;
   va_list ap;
   if (!buf || !n) return "";
   va_start(ap, key);
   while (*s && o + 1 < n)
   {
      const char *close = s[0] == '{' ? strchr(s + 1, '}') : NULL;
      const char *val = close && close - s <= 32 ? arg_find(s + 1, (size_t)(close - s - 1), ap) : NULL;
      if (val)
      {
         while (*val && o + 1 < n) buf[o++] = *val++;
         s = close + 1;
      }
      else buf[o++] = *s++;
   }
   va_end(ap);
   /* never cut a UTF-8 sequence in half */
   if (o > 0 && o + 1 >= n)
   {
      size_t j = o - 1, need;
      unsigned char c;
      while (j > 0 && ((unsigned char)buf[j] & 0xC0) == 0x80) j--;
      c = (unsigned char)buf[j];
      need = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
      if (o - j < need) o = j;
   }
   buf[o] = 0;
   return buf;
}
