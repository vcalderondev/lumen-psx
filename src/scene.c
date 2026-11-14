/* Animated per-game scenes (scene.h). Mirrors lumen-scenes/player/lumen-scene.mjs: when the
 * reference player changes, this file changes with it. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <SDL2/SDL.h>
#include "lumen.h"
#include "scene.h"

#define TAU 6.283185307179586f

/* FORMAT.md limits */
#define LIM_LAYERS    16
#define LIM_TRIS      20000
#define LIM_INST      512
#define LIM_POINTS    3000
#define LIM_FILE      (1 << 20)
#define LIM_SCENE     (3 << 20)
#define MAX_MODELS    16

/* ======================================================================== */
/* JSON (just enough for scene.json)                                         */

enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ };
typedef struct json json_t;
struct json { int type; double num; char *str; int n; json_t **kids; char **keys; };

typedef struct { const char *p, *end; int depth; } jp_t;

static void json_free(json_t *j)
{
   int i;
   if (!j) return;
   for (i = 0; i < j->n; i++) { json_free(j->kids[i]); if (j->keys) free(j->keys[i]); }
   free(j->kids); free(j->keys); free(j->str); free(j);
}

static void jws(jp_t *s) { while (s->p < s->end && (*s->p == ' ' || *s->p == '\t' || *s->p == '\n' || *s->p == '\r')) s->p++; }

static char *jstring(jp_t *s)
{
   char *out, *o;
   const char *q;
   if (s->p >= s->end || *s->p != '"') return NULL;
   q = ++s->p;
   while (q < s->end && *q != '"') { if (*q == '\\') q++; q++; }
   if (q >= s->end) return NULL;
   out = o = malloc(q - s->p + 1);
   if (!out) return NULL;
   while (s->p < q)
   {
      char c = *s->p++;
      if (c == '\\' && s->p < q)
      {
         c = *s->p++;
         switch (c)
         {
         case 'n': c = '\n'; break;
         case 't': c = '\t'; break;
         case 'r': c = '\r'; break;
         case 'b': c = '\b'; break;
         case 'f': c = '\f'; break;
         case 'u':
         {  /* \uXXXX: ASCII kept, anything else as '?' (only names use it) */
            unsigned v = 0; int k;
            for (k = 0; k < 4 && s->p < q; k++) { char h = *s->p++; v = v * 16 + (h <= '9' ? h - '0' : (h | 32) - 'a' + 10); }
            c = v < 128 ? (char)v : '?';
            break;
         }
         default: break;
         }
      }
      *o++ = c;
   }
   *o = 0;
   s->p = q + 1;
   return out;
}

static json_t *jvalue(jp_t *s)
{
   json_t *j;
   jws(s);
   if (s->p >= s->end || ++s->depth > 64) return NULL;
   j = calloc(1, sizeof *j);
   if (!j) return NULL;
   if (*s->p == '{' || *s->p == '[')
   {
      int obj = *s->p++ == '{', cap = 0;
      j->type = obj ? J_OBJ : J_ARR;
      jws(s);
      if (s->p < s->end && *s->p == (obj ? '}' : ']')) { s->p++; s->depth--; return j; }
      for (;;)
      {
         char *key = NULL;
         json_t *v;
         jws(s);
         if (obj)
         {
            if (!(key = jstring(s))) goto bad;
            jws(s);
            if (s->p >= s->end || *s->p++ != ':') { free(key); goto bad; }
         }
         if (!(v = jvalue(s))) { free(key); goto bad; }
         if (j->n == cap)
         {
            cap = cap ? cap * 2 : 8;
            j->kids = realloc(j->kids, sizeof(json_t *) * cap);
            if (obj) j->keys = realloc(j->keys, sizeof(char *) * cap);
            if (!j->kids || (obj && !j->keys)) { free(key); json_free(v); goto bad; }
         }
         j->kids[j->n] = v;
         if (obj) j->keys[j->n] = key;
         j->n++;
         jws(s);
         if (s->p < s->end && *s->p == ',') { s->p++; continue; }
         if (s->p < s->end && *s->p == (obj ? '}' : ']')) { s->p++; break; }
         goto bad;
      }
   }
   else if (*s->p == '"') { j->type = J_STR; if (!(j->str = jstring(s))) goto bad; }
   else if (!strncmp(s->p, "true", 4)) { j->type = J_BOOL; j->num = 1; s->p += 4; }
   else if (!strncmp(s->p, "false", 5)) { j->type = J_BOOL; s->p += 5; }
   else if (!strncmp(s->p, "null", 4)) { j->type = J_NULL; s->p += 4; }
   else
   {
      char *e;
      j->type = J_NUM;
      j->num = strtod(s->p, &e);
      if (e == s->p) goto bad;
      s->p = e;
   }
   s->depth--;
   return j;
bad:
   json_free(j);
   return NULL;
}

static json_t *jget(const json_t *o, const char *k)
{
   int i;
   if (!o || o->type != J_OBJ) return NULL;
   for (i = 0; i < o->n; i++) if (!strcmp(o->keys[i], k)) return o->kids[i];
   return NULL;
}
static int jnum(const json_t *j, float *out) { if (!j || j->type != J_NUM || !isfinite(j->num)) return 0; *out = (float)j->num; return 1; }
static float jnumd(const json_t *o, const char *k, float d) { float v; return jnum(jget(o, k), &v) ? v : d; }
static int jvec(const json_t *j, float *out, int n)
{
   int i;
   if (!j || j->type != J_ARR || j->n != n) return 0;
   for (i = 0; i < n; i++) if (!jnum(j->kids[i], &out[i])) return 0;
   return 1;
}
static int jcolor(const json_t *j, float out[3])
{
   const char *c;
   unsigned v; int i;
   if (!j || j->type != J_STR) return 0;
   c = j->str;
   if (c[0] != '#') return 0;
   for (i = 1; c[i]; i++) if (!((c[i] >= '0' && c[i] <= '9') || ((c[i] | 32) >= 'a' && (c[i] | 32) <= 'f'))) return 0;
   v = (unsigned)strtoul(c + 1, NULL, 16);
   if (i == 7) { out[0] = (v >> 16 & 255) / 255.0f; out[1] = (v >> 8 & 255) / 255.0f; out[2] = (v & 255) / 255.0f; return 1; }
   if (i == 4) { out[0] = (v >> 8 & 15) * 17 / 255.0f; out[1] = (v >> 4 & 15) * 17 / 255.0f; out[2] = (v & 15) * 17 / 255.0f; return 1; }
   return 0;
}

/* ======================================================================== */
/* scene data                                                                */

enum { L_GRAD, L_STARS, L_MESH, L_PART, L_GLOW };

typedef struct
{
   int type;
   /* gradient */
   int nstops; float stop_col[4][3], stop_pos[4];
   /* common */
   int count; unsigned seed; float color[3], opacity;
   /* stars */
   float area[4], size2[2], twinkle;
   /* mesh */
   int model, fog, spin_axis, has_bob, has_drift, has_wrap, ninst, has_pulse;
   float pos[3], rot[3], scale[3], spin_speed, bob_amt[3], bob_period, drift_vel[3], wrap[2], pulse_period, pulse_min;
   float (*inst)[5];
   int nkeys; float anim_period; struct { float t, pos[3], rot[3], sc[3]; } keys[32];   /* animate */
   /* particles */
   float box[6], vel[3], jitter, psize; int alpha_blend;
   /* glow */
   int npos; float (*gpos)[3]; float radius, intensity;
   /* generated points (stars, particles): count x 4 */
   float (*gen)[4];
} layer_t;

typedef struct { char path[256]; float *v; int nverts; GLuint vbo; } model_t;

struct scene
{
   char dir[512], music[600];
   float music_vol, resolution;
   float fov, cam_pos[3], cam_target[3];
   int has_orbit, has_bob; float orbit_deg, orbit_period, bob_amt[3], bob_period;
   int has_fog; float fog_col[3], fog_near, fog_far;
   float light_dir[3], light_col[3], ambient[3];
   layer_t layers[LIM_LAYERS]; int nlayers;
   model_t models[MAX_MODELS]; int nmodels;
   int triangles, points_max, points_last;
   /* GL */
   GLuint fbo, tex, depth, vao, ptbuf, fullbuf;
   int fw, fh;
};

static void layers_free(scene_t *s)
{
   int i;
   for (i = 0; i < s->nlayers; i++) { free(s->layers[i].inst); free(s->layers[i].gpos); free(s->layers[i].gen); }
   for (i = 0; i < s->nmodels; i++) free(s->models[i].v);
}

/* path relative to the scene folder, normalised like normPath(): "" parts and "." dropped,
 * ".." above the folder rejected (0) */
static int norm_path(const char *base, const char *rel, char *out, int n)
{
   char buf[512], *tok, *save = NULL;
   int len = 0;
   snprintf(buf, sizeof buf, "%s%s%s", base, base[0] ? "/" : "", rel);
   out[0] = 0;
   for (tok = strtok_r(buf, "/", &save); tok; tok = strtok_r(NULL, "/", &save))
   {
      if (!strcmp(tok, ".")) continue;
      if (!strcmp(tok, ".."))
      {
         char *sl;
         if (!len) return 0;
         sl = strrchr(out, '/');
         if (sl) *sl = 0; else out[0] = 0;
         len = (int)strlen(out);
         continue;
      }
      len += snprintf(out + len, n - len, "%s%s", len ? "/" : "", tok);
      if (len >= n) return 0;
   }
   return 1;
}

static char *read_all(const char *dir, const char *rel, long *len_out, long *total)
{
   char path[800];
   FILE *f;
   long n;
   char *b = NULL;
   snprintf(path, sizeof path, "%s/%s", dir, rel);
   if (!(f = fopen(path, "rb"))) { logf_("scene: missing file %s", path); return NULL; }
   if (fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) >= 0 && n <= LIM_FILE && fseek(f, 0, SEEK_SET) == 0
       && (b = malloc(n + 1)) && fread(b, 1, n, f) == (size_t)n)
   {
      b[n] = 0;
      if (len_out) *len_out = n;
      *total += n;
   }
   else { logf_("scene: %s unreadable or over %d bytes", path, LIM_FILE); free(b); b = NULL; }
   fclose(f);
   return b;
}

/* ---- OBJ / MTL (parseObj / parseMtl) ---- */
typedef struct { char name[64]; float kd[3], ke[3]; int has_ke; } mtl_t;

/* next line of text in place (NUL-terminated, \r dropped); NULL at the end */
static char *next_line(char **cur)
{
   char *l = *cur, *e;
   if (!l || !*l) return NULL;
   e = strchr(l, '\n');
   if (e) { *e = 0; *cur = e + 1; } else *cur = l + strlen(l);
   if ((e = strchr(l, '\r'))) *e = 0;
   while (*l == ' ' || *l == '\t') l++;
   return l;
}
/* rest of the line after the keyword, whitespace runs collapsed to one space (JS: split + join) */
static void rest_joined(const char *l, char *out, int n)
{
   int k = 0, sp = 0;
   while (*l && *l != ' ' && *l != '\t') l++;
   while (*l == ' ' || *l == '\t') l++;
   for (; *l && k < n - 1; l++)
   {
      if (*l == ' ' || *l == '\t') { sp = 1; continue; }
      if (sp && k) out[k++] = ' ';
      sp = 0;
      if (k < n - 1) out[k++] = *l;
   }
   out[k] = 0;
}

static int parse_mtl(char *text, mtl_t *m, int nm, int max)
{
   char *cur = text, *l;
   mtl_t *c = NULL;
   while ((l = next_line(&cur)))
   {
      if (!strncmp(l, "newmtl", 6) && (l[6] == ' ' || l[6] == '\t'))
      {
         if (nm >= max) continue;
         c = &m[nm++];
         memset(c, 0, sizeof *c);
         rest_joined(l, c->name, sizeof c->name);
         c->kd[0] = c->kd[1] = c->kd[2] = 0.8f;
      }
      else if (c && l[0] == 'K' && l[1] == 'd' && (l[2] == ' ' || l[2] == '\t'))
         sscanf(l + 2, "%f %f %f", &c->kd[0], &c->kd[1], &c->kd[2]);
      else if (c && l[0] == 'K' && l[1] == 'e' && (l[2] == ' ' || l[2] == '\t'))
      {
         sscanf(l + 2, "%f %f %f", &c->ke[0], &c->ke[1], &c->ke[2]);
         c->has_ke = c->ke[0] > 0 || c->ke[1] > 0 || c->ke[2] > 0;
      }
   }
   return nm;
}

typedef struct { float *p; int n, cap; } fbuf_t;
static int fb_push(fbuf_t *b, const float *v, int k)
{
   if (b->n + k > b->cap)
   {
      int nc = b->cap ? b->cap * 2 : 4096;
      float *np;
      while (nc < b->n + k) nc *= 2;
      if (!(np = realloc(b->p, sizeof(float) * nc))) return -1;
      b->p = np; b->cap = nc;
   }
   memcpy(b->p + b->n, v, sizeof(float) * k);
   b->n += k;
   return 0;
}

/* vertices: 10 floats (pos 3, face normal 3, colour 3, emissive 1) */
static int parse_obj(char *text, const mtl_t *mats, int nm, model_t *out)
{
   fbuf_t V = { 0 }, VC = { 0 }, O = { 0 };
   char *cur = text, *l;
   const mtl_t *mat = NULL;
   int ok = 1;
   while (ok && (l = next_line(&cur)))
   {
      if (l[0] == 'v' && (l[1] == ' ' || l[1] == '\t'))
      {
         float v[6] = { 0, 0, 0, -1, -1, -1 };
         int k = sscanf(l + 1, "%f %f %f %f %f %f", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]);
         if (k < 6) v[3] = -1;       /* no vertex colour */
         if (fb_push(&V, v, 3) || fb_push(&VC, v + 3, 3)) ok = 0;
      }
      else if (!strncmp(l, "usemtl", 6) && (l[6] == ' ' || l[6] == '\t'))
      {
         char name[64]; int i;
         rest_joined(l, name, sizeof name);
         mat = NULL;
         for (i = 0; i < nm; i++) if (!strcmp(mats[i].name, name)) mat = &mats[i];
      }
      else if (l[0] == 'f' && (l[1] == ' ' || l[1] == '\t'))
      {
         int idx[64], ni = 0, nv = V.n / 3, k;
         char *tok, *save = NULL;
         for (tok = strtok_r(l + 1, " \t", &save); tok && ni < 64; tok = strtok_r(NULL, " \t", &save))
         {
            int i = atoi(tok);
            idx[ni++] = i < 0 ? nv + i : i - 1;
         }
         if (ni < 3) { logf_("scene: bad face"); ok = 0; break; }
         for (k = 0; k < ni; k++) if (idx[k] < 0 || idx[k] >= nv) { logf_("scene: bad face index"); ok = 0; }
         for (k = 1; ok && k + 1 < ni; k++)
         {
            const float *a = V.p + idx[0] * 3, *b = V.p + idx[k] * 3, *c = V.p + idx[k + 1] * 3;
            float e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, e2[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
            float nn[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
            float len = sqrtf(nn[0] * nn[0] + nn[1] * nn[1] + nn[2] * nn[2]), rgb[3], emi = 0, vert[10];
            const float *pts[3];
            int q;
            if (len <= 0) len = 1;
            nn[0] /= len; nn[1] /= len; nn[2] /= len;
            if (mat && mat->has_ke) { memcpy(rgb, mat->ke, sizeof rgb); emi = 1; }
            else if (mat) memcpy(rgb, mat->kd, sizeof rgb);
            else if (VC.p[idx[0] * 3] >= 0)
               for (q = 0; q < 3; q++) rgb[q] = (VC.p[idx[0] * 3 + q] + VC.p[idx[k] * 3 + q] + VC.p[idx[k + 1] * 3 + q]) / 3;
            else rgb[0] = rgb[1] = rgb[2] = 0.8f;
            pts[0] = a; pts[1] = b; pts[2] = c;
            for (q = 0; q < 3; q++)
            {
               memcpy(vert, pts[q], 12); memcpy(vert + 3, nn, 12); memcpy(vert + 6, rgb, 12); vert[9] = emi;
               if (fb_push(&O, vert, 10)) { ok = 0; break; }
            }
         }
      }
   }
   free(V.p); free(VC.p);
   if (!ok) { free(O.p); return 0; }
   out->v = O.p;
   out->nverts = O.n / 10;
   return 1;
}

/* mtllib lines of an OBJ (before parse_obj, which modifies the text) */
static int mtllibs(const char *text, char libs[][128], int max)
{
   int n = 0;
   const char *l = text;
   while (l && *l && n < max)
   {
      const char *e = strchr(l, '\n'), *p = l;
      while (*p == ' ' || *p == '\t') p++;
      if (!strncmp(p, "mtllib", 6) && (p[6] == ' ' || p[6] == '\t'))
      {
         int k = 0;
         p += 6;
         while (*p == ' ' || *p == '\t') p++;
         while (p < (e ? e : p + strlen(p)) && k < 127) libs[n][k++] = *p++;
         while (k && (libs[n][k - 1] == ' ' || libs[n][k - 1] == '\t' || libs[n][k - 1] == '\r')) k--;
         libs[n][k] = 0;
         if (k) n++;
      }
      l = e ? e + 1 : NULL;
   }
   return n;
}

static int load_model(scene_t *s, const char *rel, long *total)
{
   char path[256], libs[8][128], dirpart[256], *text, *sl;
   mtl_t mats[64];
   int nm = 0, nl, i;
   if (!norm_path("", rel, path, sizeof path)) { logf_("scene: model outside the folder: %s", rel); return -1; }
   for (i = 0; i < s->nmodels; i++) if (!strcmp(s->models[i].path, path)) return i;
   if (s->nmodels >= MAX_MODELS) { logf_("scene: more than %d models", MAX_MODELS); return -1; }
   if (!(text = read_all(s->dir, path, NULL, total))) return -1;
   snprintf(dirpart, sizeof dirpart, "%s", path);
   if ((sl = strrchr(dirpart, '/'))) *sl = 0; else dirpart[0] = 0;
   nl = mtllibs(text, libs, 8);
   for (i = 0; i < nl; i++)
   {
      char mp[256], *mt;
      if (!norm_path(dirpart, libs[i], mp, sizeof mp)) { logf_("scene: mtllib outside the folder: %s", libs[i]); free(text); return -1; }
      if (!(mt = read_all(s->dir, mp, NULL, total))) { free(text); return -1; }
      nm = parse_mtl(mt, mats, nm, 64);
      free(mt);
   }
   i = s->nmodels;
   memset(&s->models[i], 0, sizeof s->models[i]);
   snprintf(s->models[i].path, sizeof s->models[i].path, "%s", path);
   if (!parse_obj(text, mats, nm, &s->models[i])) { free(text); logf_("scene: %s: bad OBJ", path); return -1; }
   free(text);
   s->nmodels++;
   return i;
}

/* xorshift32 (rng() in the reference) */
static unsigned xs_next(unsigned *x) { *x ^= *x << 13; *x ^= *x >> 17; *x ^= *x << 5; return *x; }

static int parse_scene(scene_t *s, const json_t *J, long *total)
{
   const json_t *cam = jget(J, "camera"), *fog = jget(J, "fog"), *light = jget(J, "light"), *music = jget(J, "music"), *layers = jget(J, "layers");
   float v; int i, points = 0;
   if (!jnum(jget(J, "format"), &v) || v != 1) { logf_("scene: format must be 1"); return -1; }
   s->resolution = clampf(jnumd(J, "resolution", 0.5f), 0.25f, 1.0f);
   s->fov = jnumd(cam, "fov", 50);
   if (!jvec(jget(cam, "position"), s->cam_pos, 3)) { s->cam_pos[0] = 0; s->cam_pos[1] = 2; s->cam_pos[2] = 10; }
   if (!jvec(jget(cam, "target"), s->cam_target, 3)) s->cam_target[0] = s->cam_target[1] = s->cam_target[2] = 0;
   if (jget(cam, "orbit")) { const json_t *o = jget(cam, "orbit"); s->has_orbit = jnum(jget(o, "degrees"), &s->orbit_deg) && jnum(jget(o, "period"), &s->orbit_period) && s->orbit_period > 0; }
   if (jget(cam, "bob")) { const json_t *o = jget(cam, "bob"); s->has_bob = jvec(jget(o, "amount"), s->bob_amt, 3) && jnum(jget(o, "period"), &s->bob_period) && s->bob_period > 0; }
   if (fog) s->has_fog = jcolor(jget(fog, "color"), s->fog_col) && jnum(jget(fog, "near"), &s->fog_near) && jnum(jget(fog, "far"), &s->fog_far) && s->fog_far > s->fog_near;
   if (!s->has_fog) { s->fog_col[0] = s->fog_col[1] = s->fog_col[2] = 0; s->fog_near = 1e6f; s->fog_far = 1e6f + 1; }
   {
      float d[3] = { -0.4f, -1, -0.3f }, l;
      jvec(jget(light, "direction"), d, 3);
      l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]); if (l <= 0) l = 1;
      s->light_dir[0] = d[0] / l; s->light_dir[1] = d[1] / l; s->light_dir[2] = d[2] / l;
      if (!jcolor(jget(light, "color"), s->light_col)) s->light_col[0] = s->light_col[1] = s->light_col[2] = 1;
      if (!jcolor(jget(light, "ambient"), s->ambient)) { s->ambient[0] = s->ambient[1] = 0.25f; s->ambient[2] = 0.3f; }
   }
   if (music && jget(music, "file") && jget(music, "file")->type == J_STR)
   {
      char rel[256];
      if (norm_path("", jget(music, "file")->str, rel, sizeof rel)) snprintf(s->music, sizeof s->music, "%s/%s", s->dir, rel);
      s->music_vol = clampf(jnumd(music, "volume", 0.8f), 0, 1);
   }
   if (!layers || layers->type != J_ARR || !layers->n || layers->n > LIM_LAYERS) { logf_("scene: 1-%d layers", LIM_LAYERS); return -1; }
   for (i = 0; i < layers->n; i++)
   {
      const json_t *Lj = layers->kids[i], *type = jget(Lj, "type"), *x;
      layer_t *L = &s->layers[s->nlayers++];
      memset(L, 0, sizeof *L);
      L->color[0] = L->color[1] = L->color[2] = 1;
      jcolor(jget(Lj, "color"), L->color);
      L->seed = (unsigned)jnumd(Lj, "seed", (float)(i + 1));
      if (!type || type->type != J_STR) { logf_("scene: layer %d: no type", i); return -1; }
      if (!strcmp(type->str, "gradient"))
      {
         const json_t *st = jget(Lj, "stops"); int k;
         L->type = L_GRAD;
         if (!st || st->type != J_ARR || st->n < 2 || st->n > 4) { logf_("scene: layer %d: 2-4 stops", i); return -1; }
         L->nstops = st->n;
         for (k = 0; k < 4; k++)
         {
            const json_t *e = st->kids[k < st->n ? k : st->n - 1];
            if (!e || e->type != J_ARR || e->n != 2 || !jcolor(e->kids[0], L->stop_col[k]) || !jnum(e->kids[1], &L->stop_pos[k])) { logf_("scene: layer %d: bad stop", i); return -1; }
         }
      }
      else if (!strcmp(type->str, "stars") || !strcmp(type->str, "particles"))
      {
         float c;
         L->type = type->str[0] == 's' ? L_STARS : L_PART;
         if (!jnum(jget(Lj, "count"), &c) || c < 1 || c != floorf(c)) { logf_("scene: layer %d: count", i); return -1; }
         L->count = (int)c;
         points += L->count;
         if (points > LIM_POINTS) break;
         if (L->type == L_STARS)
         {
            if (!jvec(jget(Lj, "area"), L->area, 4)) { L->area[0] = 0; L->area[1] = 0; L->area[2] = 1; L->area[3] = 0.6f; }
            if (!jvec(jget(Lj, "size"), L->size2, 2)) { L->size2[0] = 1; L->size2[1] = 2.5f; }
            L->twinkle = jnumd(Lj, "twinkle", 3);
            L->opacity = jnumd(Lj, "opacity", 1);
         }
         else
         {
            if (!jvec(jget(Lj, "box"), L->box, 6) || !jnum(jget(Lj, "size"), &L->psize) || L->psize <= 0) { logf_("scene: layer %d: box and size", i); return -1; }
            if (!jvec(jget(Lj, "velocity"), L->vel, 3)) { L->vel[0] = 0; L->vel[1] = 0.2f; L->vel[2] = 0; }
            L->jitter = jnumd(Lj, "jitter", 0);
            L->opacity = jnumd(Lj, "opacity", 0.6f);
            x = jget(Lj, "blend");
            L->alpha_blend = x && x->type == J_STR && !strcmp(x->str, "alpha");
         }
         {  /* generated points: four numbers each, in order */
            unsigned r = L->seed ? L->seed : 1; int k, q;
            if (!(L->gen = malloc(sizeof(float) * 4 * L->count))) return -1;
            for (k = 0; k < L->count; k++) for (q = 0; q < 4; q++) L->gen[k][q] = xs_next(&r) / 4294967296.0f;
         }
      }
      else if (!strcmp(type->str, "mesh"))
      {
         const json_t *sc = jget(Lj, "scale"), *sp = jget(Lj, "spin"), *bb = jget(Lj, "bob"), *dr = jget(Lj, "drift"), *in = jget(Lj, "instances"), *pu = jget(Lj, "pulse"), *fg = jget(Lj, "fog");
         float f;
         L->type = L_MESH;
         if (!jget(Lj, "model") || jget(Lj, "model")->type != J_STR) { logf_("scene: layer %d: model", i); return -1; }
         if ((L->model = load_model(s, jget(Lj, "model")->str, total)) < 0) return -1;
         jvec(jget(Lj, "position"), L->pos, 3);
         jvec(jget(Lj, "rotation"), L->rot, 3);
         L->scale[0] = L->scale[1] = L->scale[2] = 1;
         if (jnum(sc, &f)) L->scale[0] = L->scale[1] = L->scale[2] = f; else jvec(sc, L->scale, 3);
         L->spin_axis = -1;
         if (sp && jget(sp, "axis") && jget(sp, "axis")->type == J_STR && jnum(jget(sp, "speed"), &L->spin_speed))
         { const char *a = jget(sp, "axis")->str; L->spin_axis = !strcmp(a, "x") ? 0 : !strcmp(a, "y") ? 1 : !strcmp(a, "z") ? 2 : -1; }
         L->has_bob = bb && jvec(jget(bb, "amount"), L->bob_amt, 3) && jnum(jget(bb, "period"), &L->bob_period) && L->bob_period > 0;
         L->has_drift = dr && jvec(jget(dr, "velocity"), L->drift_vel, 3);
         L->has_wrap = L->has_drift && jvec(jget(dr, "wrap"), L->wrap, 2) && L->wrap[1] > L->wrap[0];
         L->has_pulse = pu && jnum(jget(pu, "period"), &L->pulse_period) && L->pulse_period > 0 && jnum(jget(pu, "min"), &L->pulse_min);
         L->fog = !(fg && fg->type == J_BOOL && !fg->num);
         if ((x = jget(Lj, "animate")))
         {  /* { period, keys: [{ t, position?, rotation?, scale? }] }, missing = neutral */
            const json_t *ks = jget(x, "keys");
            int k;
            if (!jnum(jget(x, "period"), &L->anim_period) || L->anim_period <= 0 || !ks || ks->type != J_ARR || ks->n < 2 || ks->n > 32)
            { logf_("scene: layer %d: bad animate", i); return -1; }
            L->nkeys = ks->n;
            for (k = 0; k < ks->n; k++)
            {
               const json_t *kj = ks->kids[k];
               if (!jnum(jget(kj, "t"), &L->keys[k].t) || L->keys[k].t < 0 || L->keys[k].t > 1 || (k && L->keys[k].t < L->keys[k - 1].t))
               { logf_("scene: layer %d: animate keys need t 0..1 ascending", i); return -1; }
               if (!jvec(jget(kj, "position"), L->keys[k].pos, 3)) L->keys[k].pos[0] = L->keys[k].pos[1] = L->keys[k].pos[2] = 0;
               if (!jvec(jget(kj, "rotation"), L->keys[k].rot, 3)) L->keys[k].rot[0] = L->keys[k].rot[1] = L->keys[k].rot[2] = 0;
               if (!jvec(jget(kj, "scale"), L->keys[k].sc, 3)) L->keys[k].sc[0] = L->keys[k].sc[1] = L->keys[k].sc[2] = 1;
            }
         }
         if (in && in->type == J_ARR)
         {
            int k;
            if (in->n > LIM_INST) { logf_("scene: layer %d: more than %d instances", i, LIM_INST); return -1; }
            L->ninst = in->n;
            if (!(L->inst = malloc(sizeof(float) * 5 * (L->ninst ? L->ninst : 1)))) return -1;
            for (k = 0; k < L->ninst; k++) if (!jvec(in->kids[k], L->inst[k], 5)) { logf_("scene: layer %d: bad instance", i); return -1; }
         }
         else
         {
            L->ninst = 1;
            if (!(L->inst = calloc(1, sizeof(float) * 5))) return -1;
            L->inst[0][4] = 1;
         }
         s->triangles += s->models[L->model].nverts / 3 * L->ninst;
      }
      else if (!strcmp(type->str, "glow"))
      {
         const json_t *ps = jget(Lj, "positions"), *pu = jget(Lj, "pulse");
         int k;
         L->type = L_GLOW;
         if (ps && ps->type == J_ARR && ps->n > 0)
         {
            L->npos = ps->n;
            if (!(L->gpos = malloc(sizeof(float) * 3 * L->npos))) return -1;
            for (k = 0; k < L->npos; k++) if (!jvec(ps->kids[k], L->gpos[k], 3)) { logf_("scene: layer %d: bad position", i); return -1; }
         }
         else
         {
            L->npos = 1;
            if (!(L->gpos = malloc(sizeof(float) * 3)) || !jvec(jget(Lj, "position"), L->gpos[0], 3)) { logf_("scene: layer %d: position(s)", i); return -1; }
         }
         if (!jnum(jget(Lj, "radius"), &L->radius) || L->radius <= 0) { logf_("scene: layer %d: radius", i); return -1; }
         L->intensity = jnumd(Lj, "intensity", 0.8f);
         L->has_pulse = pu && jnum(jget(pu, "period"), &L->pulse_period) && L->pulse_period > 0 && jnum(jget(pu, "min"), &L->pulse_min);
         points += L->npos;
      }
      else { logf_("scene: layer %d: unknown type %s", i, type->str); return -1; }
   }
   if (points > LIM_POINTS) { logf_("scene: more than %d points", LIM_POINTS); return -1; }
   if (s->triangles > LIM_TRIS) { logf_("scene: %d triangles, at most %d", s->triangles, LIM_TRIS); return -1; }
   if (*total > LIM_SCENE) { logf_("scene: %ld bytes, at most %d", *total, LIM_SCENE); return -1; }
   s->points_max = points;
   return 0;
}

static scene_t *scene_load(const char *dir)
{
   scene_t *s = calloc(1, sizeof *s);
   long total = 0, len = 0;
   char *text;
   json_t *J;
   jp_t jp;
   double t0 = now_s();
   if (!s) return NULL;
   snprintf(s->dir, sizeof s->dir, "%s", dir);
   if (!(text = read_all(dir, "scene.json", &len, &total))) { free(s); return NULL; }
   jp.p = text; jp.end = text + len; jp.depth = 0;
   J = jvalue(&jp);
   free(text);
   if (!J || J->type != J_OBJ) { logf_("scene: %s/scene.json: invalid JSON", dir); json_free(J); free(s); return NULL; }
   if (parse_scene(s, J, &total)) { json_free(J); layers_free(s); free(s); logf_("scene: %s rejected", dir); return NULL; }
   json_free(J);
   logf_("scene: loaded %s (%d layers, %d models, %d triangles, %ld bytes, %.0f ms)", dir, s->nlayers, s->nmodels, s->triangles, total, (now_s() - t0) * 1000);
   return s;
}

/* ======================================================================== */
/* loader thread                                                             */

static SDL_Thread *ld_thread;
static SDL_mutex  *ld_mx;
static SDL_cond   *ld_cv;
static char        ld_req[512], ld_done_dir[512];
static int         ld_req_new, ld_quit, ld_done_new, ld_done_failed;
static scene_t    *ld_done;

static int SDLCALL ld_main(void *ud)
{
   (void)ud;
   SDL_LockMutex(ld_mx);
   while (!ld_quit)
   {
      char dir[512];
      scene_t *s;
      while (!ld_req_new && !ld_quit) SDL_CondWait(ld_cv, ld_mx);
      if (ld_quit) break;
      snprintf(dir, sizeof dir, "%s", ld_req);
      ld_req_new = 0;
      SDL_UnlockMutex(ld_mx);
      s = dir[0] ? scene_load(dir) : NULL;
      SDL_LockMutex(ld_mx);
      if (ld_done) { layers_free(ld_done); free(ld_done); }
      ld_done = s;
      ld_done_failed = !s && dir[0];
      ld_done_new = dir[0] != 0;
      snprintf(ld_done_dir, sizeof ld_done_dir, "%s", dir);
   }
   SDL_UnlockMutex(ld_mx);
   return 0;
}

int scene_init(void)
{
   ld_mx = SDL_CreateMutex();
   ld_cv = SDL_CreateCond();
   ld_thread = ld_mx && ld_cv ? SDL_CreateThread(ld_main, "scene", NULL) : NULL;
   if (!ld_thread) { logf_("scene: loader thread failed"); return -1; }
   return 0;
}

void scene_shutdown(void)
{
   if (!ld_thread) return;
   SDL_LockMutex(ld_mx);
   ld_quit = 1;
   SDL_CondSignal(ld_cv);
   SDL_UnlockMutex(ld_mx);
   SDL_WaitThread(ld_thread, NULL);
   ld_thread = NULL;
   if (ld_done) { layers_free(ld_done); free(ld_done); ld_done = NULL; }
}

void scene_request(const char *dir)
{
   if (!ld_thread) return;
   SDL_LockMutex(ld_mx);
   snprintf(ld_req, sizeof ld_req, "%s", dir ? dir : "");
   ld_req_new = 1;
   SDL_CondSignal(ld_cv);
   SDL_UnlockMutex(ld_mx);
}

/* ======================================================================== */
/* GL                                                                        */

static GLuint p_mesh, p_grad, p_pt;
static GLint um_M, um_VP, um_R, um_eye, um_ldir, um_lcol, um_amb, um_pulse, um_fogon, um_fog, um_fogcol;
static GLint ug_C, ug_P, ug_N, up_glow;
static float g_max_pt = 64;

static const char *VS_MESH =
   "attribute vec3 aPos; attribute vec3 aNrm; attribute vec3 aCol; attribute float aEmi;\n"
   "uniform mat4 uM, uVP; uniform mat3 uR; uniform vec3 uEye, uLightDir, uLightCol, uAmb;\n"
   "uniform float uPulse, uFogOn; uniform vec2 uFog;\n"
   "varying vec3 vCol; varying float vFog;\n"
   "void main() {\n"
   "  vec4 w = uM * vec4(aPos, 1.0);\n"
   "  vec3 n = normalize(uR * aNrm);\n"
   "  vec3 lit = aCol * (uAmb + uLightCol * max(dot(n, -uLightDir), 0.0));\n"
   "  vCol = mix(lit, aCol * uPulse, aEmi);\n"
   "  float f = clamp((distance(w.xyz, uEye) - uFog.x) / (uFog.y - uFog.x), 0.0, 1.0);\n"
   "  vFog = uFogOn * f * (1.0 - 0.5 * aEmi);\n"
   "  gl_Position = uVP * w;\n"
   "}\n";
static const char *FS_MESH =
   "precision mediump float;\n"
   "uniform vec3 uFogCol; varying vec3 vCol; varying float vFog;\n"
   "void main() { gl_FragColor = vec4(mix(vCol, uFogCol, vFog), 1.0); }\n";
static const char *VS_FULL =
   "attribute vec2 aXY; varying vec2 vUV;\n"
   "void main() { vUV = vec2(0.5 + 0.5 * aXY.x, 0.5 - 0.5 * aXY.y); gl_Position = vec4(aXY, 0.0, 1.0); }\n";
static const char *FS_GRAD =
   "precision mediump float;\n"
   "uniform vec3 uC[4]; uniform vec4 uP; uniform float uN; varying vec2 vUV;\n"
   "void main() {\n"
   "  float v = vUV.y; vec3 c = uC[0];\n"
   "  if (v > uP.x) c = mix(uC[0], uC[1], clamp((v - uP.x) / max(uP.y - uP.x, 1e-4), 0.0, 1.0));\n"
   "  if (uN > 2.5 && v > uP.y) c = mix(uC[1], uC[2], clamp((v - uP.y) / max(uP.z - uP.y, 1e-4), 0.0, 1.0));\n"
   "  if (uN > 3.5 && v > uP.z) c = mix(uC[2], uC[3], clamp((v - uP.z) / max(uP.w - uP.z, 1e-4), 0.0, 1.0));\n"
   "  gl_FragColor = vec4(c, 1.0);\n"
   "}\n";
static const char *VS_PT =
   "attribute vec4 aClip; attribute float aSize; attribute vec4 aCol; varying vec4 vCol;\n"
   "void main() { vCol = aCol; gl_Position = aClip; gl_PointSize = aSize; }\n";
static const char *FS_PT =
   "precision mediump float;\n"
   "uniform float uGlow; varying vec4 vCol;\n"
   "void main() {\n"
   "  float d = length(gl_PointCoord - 0.5) * 2.0;\n"
   "  float a = uGlow > 0.5 ? pow(max(1.0 - d, 0.0), 2.0) : 1.0 - smoothstep(0.0, 1.0, d);\n"
   "  gl_FragColor = vec4(vCol.rgb * vCol.a * a, vCol.a * a);\n"
   "}\n";

static GLuint sh(GLenum type, const char *src)
{
   GLuint s = glCreateShader(type);
   GLint ok = 0;
   glShaderSource(s, 1, &src, NULL);
   glCompileShader(s);
   glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
   if (!ok) { char log[1024]; glGetShaderInfoLog(s, sizeof log, NULL, log); logf_("scene: shader: %s", log); }
   return s;
}
static GLuint prog(const char *vs, const char *fs, const char *const *attrs)
{
   GLuint p = glCreateProgram(), a = sh(GL_VERTEX_SHADER, vs), b = sh(GL_FRAGMENT_SHADER, fs);
   GLint ok = 0; int i;
   glAttachShader(p, a); glAttachShader(p, b);
   for (i = 0; attrs[i]; i++) glBindAttribLocation(p, i, attrs[i]);
   glLinkProgram(p);
   glDeleteShader(a); glDeleteShader(b);
   glGetProgramiv(p, GL_LINK_STATUS, &ok);
   if (!ok) { char log[1024]; glGetProgramInfoLog(p, sizeof log, NULL, log); logf_("scene: link: %s", log); }
   return p;
}

static void gl_once(void)
{
   static const char *const am[] = { "aPos", "aNrm", "aCol", "aEmi", NULL };
   static const char *const af[] = { "aXY", NULL };
   static const char *const ap[] = { "aClip", "aSize", "aCol", NULL };
   GLfloat range[2] = { 1, 64 };
   if (p_mesh) return;
   p_mesh = prog(VS_MESH, FS_MESH, am);
   um_M = glGetUniformLocation(p_mesh, "uM"); um_VP = glGetUniformLocation(p_mesh, "uVP"); um_R = glGetUniformLocation(p_mesh, "uR");
   um_eye = glGetUniformLocation(p_mesh, "uEye"); um_ldir = glGetUniformLocation(p_mesh, "uLightDir"); um_lcol = glGetUniformLocation(p_mesh, "uLightCol");
   um_amb = glGetUniformLocation(p_mesh, "uAmb"); um_pulse = glGetUniformLocation(p_mesh, "uPulse"); um_fogon = glGetUniformLocation(p_mesh, "uFogOn");
   um_fog = glGetUniformLocation(p_mesh, "uFog"); um_fogcol = glGetUniformLocation(p_mesh, "uFogCol");
   p_grad = prog(VS_FULL, FS_GRAD, af);
   ug_C = glGetUniformLocation(p_grad, "uC"); ug_P = glGetUniformLocation(p_grad, "uP"); ug_N = glGetUniformLocation(p_grad, "uN");
   p_pt = prog(VS_PT, FS_PT, ap);
   up_glow = glGetUniformLocation(p_pt, "uGlow");
   glGetFloatv(GL_ALIASED_POINT_SIZE_RANGE, range);
   g_max_pt = range[1] > 1 ? range[1] : 64;
   logf_("scene: max point size %.0f", g_max_pt);
}

scene_t *scene_poll(char *dir_out, int n, int *failed)
{
   scene_t *s = NULL;
   int i, isnew;
   GLint vao = 0, ab = 0;
   *failed = 0;
   if (!ld_thread) return NULL;
   SDL_LockMutex(ld_mx);
   isnew = ld_done_new;
   if (isnew)
   {
      s = ld_done; ld_done = NULL; ld_done_new = 0;
      *failed = ld_done_failed;
      snprintf(dir_out, n, "%s", ld_done_dir);
   }
   SDL_UnlockMutex(ld_mx);
   if (!s) return NULL;
   gl_once();
   glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
   glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &ab);
   glGenVertexArrays(1, &s->vao);
   for (i = 0; i < s->nmodels; i++)
   {
      model_t *m = &s->models[i];
      glGenBuffers(1, &m->vbo);
      glBindBuffer(GL_ARRAY_BUFFER, m->vbo);
      glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 10 * m->nverts, m->v, GL_STATIC_DRAW);
      free(m->v); m->v = NULL;
   }
   {
      static const float full[6] = { -1, -1, 3, -1, -1, 3 };
      glGenBuffers(1, &s->fullbuf);
      glBindBuffer(GL_ARRAY_BUFFER, s->fullbuf);
      glBufferData(GL_ARRAY_BUFFER, sizeof full, full, GL_STATIC_DRAW);
   }
   glGenBuffers(1, &s->ptbuf);
   glBindBuffer(GL_ARRAY_BUFFER, s->ptbuf);
   glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 9 * (s->points_max ? s->points_max : 1), NULL, GL_STREAM_DRAW);
   glBindBuffer(GL_ARRAY_BUFFER, ab);
   glBindVertexArray(vao);
   return s;
}

void scene_free(scene_t *s)
{
   int i;
   if (!s) return;
   for (i = 0; i < s->nmodels; i++) if (s->models[i].vbo) glDeleteBuffers(1, &s->models[i].vbo);
   if (s->ptbuf) glDeleteBuffers(1, &s->ptbuf);
   if (s->fullbuf) glDeleteBuffers(1, &s->fullbuf);
   if (s->vao) glDeleteVertexArrays(1, &s->vao);
   if (s->fbo) glDeleteFramebuffers(1, &s->fbo);
   if (s->tex) glDeleteTextures(1, &s->tex);
   if (s->depth) glDeleteRenderbuffers(1, &s->depth);
   layers_free(s);
   free(s);
}

const char *scene_music(const scene_t *s, float *volume)
{
   if (!s || !s->music[0]) return NULL;
   if (volume) *volume = s->music_vol;
   return s->music;
}
int scene_cost_points(const scene_t *s) { return s ? s->points_last : 0; }
int scene_triangles(const scene_t *s) { return s ? s->triangles : 0; }

/* ---- maths, as in the reference (column-major) ---- */
typedef struct { float m[16]; } m4;
static m4 mmul(m4 a, m4 b)
{
   m4 o; int c, r;
   for (c = 0; c < 4; c++) for (r = 0; r < 4; r++)
      o.m[c * 4 + r] = a.m[r] * b.m[c * 4] + a.m[4 + r] * b.m[c * 4 + 1] + a.m[8 + r] * b.m[c * 4 + 2] + a.m[12 + r] * b.m[c * 4 + 3];
   return o;
}
static m4 mid(void) { m4 o; memset(&o, 0, sizeof o); o.m[0] = o.m[5] = o.m[10] = o.m[15] = 1; return o; }
static m4 mtr(float x, float y, float z) { m4 o = mid(); o.m[12] = x; o.m[13] = y; o.m[14] = z; return o; }
static m4 msc(float x, float y, float z) { m4 o = mid(); o.m[0] = x; o.m[5] = y; o.m[10] = z; return o; }
static m4 mrx(float a) { m4 o = mid(); float c = cosf(a), s = sinf(a); o.m[5] = c; o.m[6] = s; o.m[9] = -s; o.m[10] = c; return o; }
static m4 mry(float a) { m4 o = mid(); float c = cosf(a), s = sinf(a); o.m[0] = c; o.m[2] = -s; o.m[8] = s; o.m[10] = c; return o; }
static m4 mrz(float a) { m4 o = mid(); float c = cosf(a), s = sinf(a); o.m[0] = c; o.m[1] = s; o.m[4] = -s; o.m[5] = c; return o; }
#define D2R(d) ((d) * 3.14159265358979f / 180.0f)
static m4 mrot(const float d[3]) { return mmul(mry(D2R(d[1])), mmul(mrx(D2R(d[0])), mrz(D2R(d[2])))); }
static void mxf(const m4 *m, const float v[4], float o[4])
{
   int r;
   for (r = 0; r < 4; r++) o[r] = m->m[r] * v[0] + m->m[4 + r] * v[1] + m->m[8 + r] * v[2] + m->m[12 + r] * v[3];
}
static float wrapf(float x, float lo, float hi) { float s = hi - lo; return s > 0 ? lo + fmodf(fmodf(x - lo, s) + s, s) : x; }
static float ssf(float e0, float e1, float x) { float t = (x - e0) / (e1 - e0); t = t < 0 ? 0 : t > 1 ? 1 : t; return t * t * (3 - 2 * t); }
static float pulse_at(int has, float period, float mn, float t, float phase)
{
   return has ? mn + (1 - mn) * (0.5f + 0.5f * sinf(TAU * (t / period + phase))) : 1;
}

static void fbo_ensure(scene_t *s, int w, int h)
{
   if (s->fbo && s->fw == w && s->fh == h) return;
   if (!s->fbo) { glGenFramebuffers(1, &s->fbo); glGenTextures(1, &s->tex); glGenRenderbuffers(1, &s->depth); }
   s->fw = w; s->fh = h;
   glBindTexture(GL_TEXTURE_2D, s->tex);
   glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
   glBindRenderbuffer(GL_RENDERBUFFER, s->depth);
   glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, w, h);
   glBindFramebuffer(GL_FRAMEBUFFER, s->fbo);
   glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s->tex, 0);
   glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, s->depth);
   if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) logf_("scene: FBO incomplete (%dx%d)", w, h);
   logf_("scene: FBO %dx%d", w, h);
}

static void attr(GLuint loc, int n, int stride, int off)
{
   glEnableVertexAttribArray(loc);
   glVertexAttribPointer(loc, n, GL_FLOAT, GL_FALSE, stride, (void *)(size_t)off);
}

static float g_pts[LIM_POINTS * 9];

static void draw_points(scene_t *s, int np, int glow, int flat, int alpha_blend)
{
   if (!np) return;
   glUseProgram(p_pt);
   if (flat) glDisable(GL_DEPTH_TEST); else glEnable(GL_DEPTH_TEST);
   glDepthMask(GL_FALSE);
   glEnable(GL_BLEND);
   if (alpha_blend) glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA); else glBlendFunc(GL_ONE, GL_ONE);
   glUniform1f(up_glow, glow ? 1.0f : 0.0f);
   glBindBuffer(GL_ARRAY_BUFFER, s->ptbuf);
   glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(float) * 9 * np, g_pts);
   attr(0, 4, 36, 0); attr(1, 1, 36, 16); attr(2, 4, 36, 20);
   glDrawArrays(GL_POINTS, 0, np);
   glDisableVertexAttribArray(0); glDisableVertexAttribArray(1); glDisableVertexAttribArray(2);
   glDepthMask(GL_TRUE);
   s->points_last += np;
}

GLuint scene_render(scene_t *s, double td, int out_w, int out_h, int *fw, int *fh)
{
   float t = (float)td, FW, FH, eye[3], fov, proj, fog[2];
   m4 VP;
   GLint vao = 0, ab = 0;
   int i, w = (int)(out_w * s->resolution + 0.5f), h = (int)(out_h * s->resolution + 0.5f);
   if (w < 16) w = 16;
   if (h < 9) h = 9;
   glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
   glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &ab);
   fbo_ensure(s, w, h);
   glBindFramebuffer(GL_FRAMEBUFFER, s->fbo);
   glBindVertexArray(s->vao);
   FW = (float)w; FH = (float)h;
   glViewport(0, 0, w, h);
   glClearColor(s->fog_col[0], s->fog_col[1], s->fog_col[2], 1);
   glDepthMask(GL_TRUE);
   glClearDepthf(1.0f);
   glDepthFunc(GL_LESS);
   glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
   s->points_last = 0;

   {  /* camera (cameraAt) */
      float tgt[3], d[3], z[3], x[3], y[3], l;
      m4 P, V;
      memcpy(tgt, s->cam_target, sizeof tgt);
      memcpy(eye, s->cam_pos, sizeof eye);
      if (s->has_orbit)
      {
         float a = D2R(s->orbit_deg) * sinf(TAU * t / s->orbit_period), c = cosf(a), sn = sinf(a);
         d[0] = eye[0] - tgt[0]; d[2] = eye[2] - tgt[2];
         eye[0] = tgt[0] + d[0] * c + d[2] * sn;
         eye[2] = tgt[2] - d[0] * sn + d[2] * c;
      }
      if (s->has_bob) { float k = sinf(TAU * t / s->bob_period); for (i = 0; i < 3; i++) eye[i] += s->bob_amt[i] * k; }
      fov = D2R(s->fov);
      {  /* perspective, near 0.1 far 500 */
         float f = 1 / tanf(fov / 2), n = 0.1f, fa = 500, asp = FW / FH;
         memset(&P, 0, sizeof P);
         P.m[0] = f / asp; P.m[5] = f; P.m[10] = (fa + n) / (n - fa); P.m[11] = -1; P.m[14] = 2 * fa * n / (n - fa);
      }
      z[0] = eye[0] - tgt[0]; z[1] = eye[1] - tgt[1]; z[2] = eye[2] - tgt[2];
      l = sqrtf(z[0] * z[0] + z[1] * z[1] + z[2] * z[2]); if (l <= 0) l = 1; z[0] /= l; z[1] /= l; z[2] /= l;
      x[0] = z[2]; x[1] = 0; x[2] = -z[0];                 /* cross((0,1,0), z) */
      l = sqrtf(x[0] * x[0] + x[2] * x[2]); if (l <= 0) l = 1; x[0] /= l; x[2] /= l;
      y[0] = z[1] * x[2] - z[2] * x[1]; y[1] = z[2] * x[0] - z[0] * x[2]; y[2] = z[0] * x[1] - z[1] * x[0];
      V = mid();
      V.m[0] = x[0]; V.m[1] = y[0]; V.m[2] = z[0];
      V.m[4] = x[1]; V.m[5] = y[1]; V.m[6] = z[1];
      V.m[8] = x[2]; V.m[9] = y[2]; V.m[10] = z[2];
      V.m[12] = -(x[0] * eye[0] + x[1] * eye[1] + x[2] * eye[2]);
      V.m[13] = -(y[0] * eye[0] + y[1] * eye[1] + y[2] * eye[2]);
      V.m[14] = -(z[0] * eye[0] + z[1] * eye[1] + z[2] * eye[2]);
      VP = mmul(P, V);
   }
   proj = FH / 2 / tanf(fov / 2);
   fog[0] = s->fog_near; fog[1] = s->fog_far;
#define FOG_OF(w) (s->has_fog ? clampf((sqrtf(((w)[0] - eye[0]) * ((w)[0] - eye[0]) + ((w)[1] - eye[1]) * ((w)[1] - eye[1]) + ((w)[2] - eye[2]) * ((w)[2] - eye[2])) - fog[0]) / (fog[1] - fog[0]), 0, 1) : 0)

   for (i = 0; i < s->nlayers; i++)
   {
      layer_t *L = &s->layers[i];
      int np = 0, k;
      if (L->type == L_GRAD)
      {
         float cs[12], ps[4];
         glUseProgram(p_grad); glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND);
         for (k = 0; k < 4; k++) { memcpy(cs + k * 3, L->stop_col[k], 12); ps[k] = L->stop_pos[k]; }
         glUniform3fv(ug_C, 4, cs); glUniform4fv(ug_P, 1, ps); glUniform1f(ug_N, (float)L->nstops);
         glBindBuffer(GL_ARRAY_BUFFER, s->fullbuf);
         attr(0, 2, 0, 0);
         glDrawArrays(GL_TRIANGLES, 0, 3);
         glDisableVertexAttribArray(0);
      }
      else if (L->type == L_MESH)
      {
         model_t *m = &s->models[L->model];
         float pos[3], rot[3], scl[3], fogon = L->fog ? 1.0f : 0.0f;
         m4 R, base;
         glUseProgram(p_mesh); glEnable(GL_DEPTH_TEST); glDepthMask(GL_TRUE); glDisable(GL_BLEND);
         glUniformMatrix4fv(um_VP, 1, GL_FALSE, VP.m);
         glUniform3fv(um_eye, 1, eye); glUniform3fv(um_ldir, 1, s->light_dir); glUniform3fv(um_lcol, 1, s->light_col); glUniform3fv(um_amb, 1, s->ambient);
         glUniform2fv(um_fog, 1, fog); glUniform3fv(um_fogcol, 1, s->fog_col); glUniform1f(um_fogon, fogon);
         glUniform1f(um_pulse, pulse_at(L->has_pulse, L->pulse_period, L->pulse_min, t, 0));
         glBindBuffer(GL_ARRAY_BUFFER, m->vbo);
         attr(0, 3, 40, 0); attr(1, 3, 40, 12); attr(2, 3, 40, 24); attr(3, 1, 40, 36);
         /* meshMatrices: pose (animPose) + bob, spin */
         memcpy(pos, L->pos, sizeof pos);
         memcpy(rot, L->rot, sizeof rot);
         memcpy(scl, L->scale, sizeof scl);
         if (L->nkeys)
         {
            int n = L->nkeys, ia = n - 1, j;
            float u = fmodf(fmodf(t / L->anim_period, 1) + 1, 1), span, d, f, e;
            for (j = 0; j < n; j++) if (L->keys[j].t <= u) ia = j;
            {
               int a = u < L->keys[0].t ? n - 1 : ia, b = u < L->keys[0].t ? 0 : (ia + 1) % n;
               span = L->keys[b].t - L->keys[a].t; d = u - L->keys[a].t;
               if (span <= 0) span += 1;
               if (d < 0) d += 1;
               f = span > 0 ? (d / span < 1 ? d / span : 1) : 0;
               e = f * f * (3 - 2 * f);
               for (j = 0; j < 3; j++)
               {
                  pos[j] += L->keys[a].pos[j] + (L->keys[b].pos[j] - L->keys[a].pos[j]) * e;
                  rot[j] += L->keys[a].rot[j] + (L->keys[b].rot[j] - L->keys[a].rot[j]) * e;
                  scl[j] *= L->keys[a].sc[j] + (L->keys[b].sc[j] - L->keys[a].sc[j]) * e;
               }
            }
         }
         if (L->has_bob) { float kb = sinf(TAU * t / L->bob_period); for (k = 0; k < 3; k++) pos[k] += L->bob_amt[k] * kb; }
         if (L->spin_axis >= 0) rot[L->spin_axis] += L->spin_speed * t;
         R = mrot(rot);
         base = mmul(mtr(pos[0], pos[1], pos[2]), mmul(R, msc(scl[0], scl[1], scl[2])));
         for (k = 0; k < L->ninst; k++)
         {
            const float *in = L->inst[k];
            float p[3] = { in[0], in[1], in[2] }, r3[9];
            m4 I, M, RR;
            int q;
            if (L->has_drift)
            {
               for (q = 0; q < 3; q++) p[q] += L->drift_vel[q] * t;
               if (L->has_wrap) p[0] = wrapf(p[0], L->wrap[0], L->wrap[1]);
            }
            I = mmul(mtr(p[0], p[1], p[2]), mmul(mry(D2R(in[3])), msc(in[4], in[4], in[4])));
            M = mmul(base, I);
            RR = mmul(R, mry(D2R(in[3])));
            r3[0] = RR.m[0]; r3[1] = RR.m[1]; r3[2] = RR.m[2]; r3[3] = RR.m[4]; r3[4] = RR.m[5]; r3[5] = RR.m[6]; r3[6] = RR.m[8]; r3[7] = RR.m[9]; r3[8] = RR.m[10];
            glUniformMatrix4fv(um_M, 1, GL_FALSE, M.m);
            glUniformMatrix3fv(um_R, 1, GL_FALSE, r3);
            glDrawArrays(GL_TRIANGLES, 0, m->nverts);
         }
         for (k = 0; k < 4; k++) glDisableVertexAttribArray(k);
      }
      else if (L->type == L_STARS)
      {
         float x0 = L->area[0], y0 = L->area[1], x1 = L->area[2], y1 = L->area[3], kk = FH / 1080.0f;
         for (k = 0; k < L->count; k++)
         {
            const float *g = L->gen[k];
            float *o = g_pts + np * 9, sz = (L->size2[0] + g[2] * (L->size2[1] - L->size2[0])) * kk;
            o[0] = (x0 + g[0] * (x1 - x0)) * 2 - 1; o[1] = 1 - 2 * (y0 + g[1] * (y1 - y0)); o[2] = 0; o[3] = 1;
            o[4] = sz > 1 ? sz : 1;
            o[5] = L->color[0]; o[6] = L->color[1]; o[7] = L->color[2];
            o[8] = (0.55f + 0.45f * sinf(TAU * (t / L->twinkle + g[3]))) * L->opacity;
            np++;
         }
         draw_points(s, np, 0, 1, 0);
      }
      else if (L->type == L_PART)
      {
         const float *b = L->box;
         for (k = 0; k < L->count; k++)
         {
            const float *g = L->gen[k];
            float w[4], cl[4], e, al, sz;
            w[0] = wrapf(b[0] + g[0] * (b[3] - b[0]) + L->vel[0] * t, b[0], b[3]);
            w[1] = wrapf(b[1] + g[1] * (b[4] - b[1]) + L->vel[1] * t, b[1], b[4]);
            w[2] = wrapf(b[2] + g[2] * (b[5] - b[2]) + L->vel[2] * t, b[2], b[5]);
            w[0] += L->jitter * sinf(TAU * (t / 6 + g[3]));
            w[2] += L->jitter * cosf(TAU * (t / 7 + g[3]));
            w[3] = 1;
            e = b[4] > b[1] ? (w[1] - b[1]) / (b[4] - b[1]) : 0.5f;
            al = L->opacity * ssf(0, 0.15f, e) * (1 - ssf(0.85f, 1, e)) * (1 - FOG_OF(w));
            mxf(&VP, w, cl);
            if (cl[3] <= 0.1f) continue;
            sz = L->psize * proj / cl[3];
            sz = sz < 1 ? 1 : sz > g_max_pt ? g_max_pt : sz;
            {
               float *o = g_pts + np * 9;
               o[0] = cl[0]; o[1] = cl[1]; o[2] = cl[2]; o[3] = cl[3]; o[4] = sz;
               o[5] = L->color[0]; o[6] = L->color[1]; o[7] = L->color[2]; o[8] = al;
               np++;
            }
         }
         draw_points(s, np, 0, 0, L->alpha_blend);
      }
      else if (L->type == L_GLOW)
      {
         for (k = 0; k < L->npos; k++)
         {
            float w[4] = { L->gpos[k][0], L->gpos[k][1], L->gpos[k][2], 1 }, cl[4], sz;
            mxf(&VP, w, cl);
            if (cl[3] <= 0.1f) continue;
            sz = 2 * L->radius * proj / cl[3];
            if (sz > g_max_pt) sz = g_max_pt;
            {
               float *o = g_pts + np * 9;
               o[0] = cl[0]; o[1] = cl[1]; o[2] = cl[2]; o[3] = cl[3]; o[4] = sz;
               o[5] = L->color[0]; o[6] = L->color[1]; o[7] = L->color[2];
               o[8] = L->intensity * pulse_at(L->has_pulse, L->pulse_period, L->pulse_min, t, k * 0.37f) * (1 - 0.7f * FOG_OF(w));
               np++;
            }
         }
         draw_points(s, np, 1, 0, 0);
      }
   }
#undef FOG_OF
   glDisable(GL_DEPTH_TEST);
   glDepthMask(GL_TRUE);
   glDepthFunc(GL_LEQUAL);
   glEnable(GL_BLEND);
   glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
   glBindBuffer(GL_ARRAY_BUFFER, ab);
   glBindVertexArray(vao);
   *fw = w; *fh = h;
   return s->tex;
}
