/*
 * Lumen — 3D game frontend for the PlayStation Classic.
 * SDL2 2.0.4 + OpenGL ES 3.0 on Weston/Wayland, Cortex-A35, 1 GB RAM.
 *
 * Prototype scope:
 *   - scans /media/games/<n>/Game.ini (PSC/BleemSync layout) for PlayStation games
 *   - 3D jewel-case carousel with spring physics, idle float, lighting,
 *     floor reflections, ambient colour from the selected cover
 *   - platform pills (only platforms that have games)
 *   - "play": the case flips to show its back, zooms, fades to black, then the
 *     frontend exits with code 10 and writes the launch request to
 *     /tmp/lumen-launch (core path + content path) for scripts/run.sh
 *   - Select+Start (or Escape) exits with code 0 (back to the Sony menu)
 *
 * Usage: lumen [--root /media] [--home /media/lumen] [--log file]
 */
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>
#include <ctype.h>
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include "gl.h"

#define W 1280
#define H 720
#define MAX_GAMES 2048
#define MAX_PLATFORMS 16
#define TEX_WINDOW 6       /* covers kept on the GPU around the selection */
#define VISIBLE 5          /* boxes drawn on each side */
#define BG_OPACITY 0.30f   /* per-game background image opacity */

/* ------------------------------------------------------------------------ */
/* logging                                                                   */
static FILE *g_log;
static void logf_(const char *fmt, ...)
{
   va_list ap;
   if (!g_log) g_log = stderr;
   va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap);
   fputc('\n', g_log); fflush(g_log);
}

static double now_s(void) { return SDL_GetPerformanceCounter() / (double)SDL_GetPerformanceFrequency(); }

static long rss_kb(void)
{
   char line[256]; long kb = -1;
   FILE *f = fopen("/proc/self/status", "r");
   if (!f) return -1;
   while (fgets(line, sizeof(line), f))
      if (!strncmp(line, "VmRSS:", 6)) { kb = strtol(line + 6, NULL, 10); break; }
   fclose(f);
   return kb;
}

/* ------------------------------------------------------------------------ */
/* data                                                                      */
typedef struct {
   char title[128], publisher[96], year[16], players[8];
   char content[512], cover[512], bg[512];
   int platform;
   /* runtime */
   GLuint tex, back_tex, spine_tex;
   int tex_state;              /* 0 none, 2 decoding, 1 loaded, -1 placeholder */
   int has_cover;
   float col[3];               /* average cover colour */
   spring x, z, ry, sc, al;
} game_t;

typedef struct {
   char name[32], core[128];
   int first, count;
   GLuint label;
   int label_w, label_h;
} platform_t;

static game_t     g_games[MAX_GAMES];
static int        g_ngames;
static platform_t g_plat[MAX_PLATFORMS];
static int        g_nplat;

static char g_root[256]     = "/media";
static char g_home[256]     = "/media/lumen";
static char g_core_dir[256] = "/media/lumen/cores";
static char g_state[256]    = "/media/lumen/state.txt";

static void trim(char *s)
{
   char *e;
   while (*s && isspace((unsigned char)*s)) memmove(s, s + 1, strlen(s));
   e = s + strlen(s);
   while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
}

static int file_exists(const char *p) { struct stat st; return stat(p, &st) == 0; }

static void ini_get(const char *ini, const char *key, char *out, size_t n)
{
   char pat[64]; const char *p, *e; size_t len;
   out[0] = 0;
   snprintf(pat, sizeof(pat), "\n%s=", key);
   p = strstr(ini, pat);
   if (!p) return;
   p += strlen(pat);
   e = strpbrk(p, "\r\n");
   len = e ? (size_t)(e - p) : strlen(p);
   if (len >= n) len = n - 1;
   memcpy(out, p, len); out[len] = 0;
   trim(out);
}

static int cmp_title(const void *a, const void *b)
{
   const game_t *x = a, *y = b;
   if (x->platform != y->platform) return x->platform - y->platform;
   return strcasecmp(x->title, y->title);
}

static void scan_psx(void)
{
   char dir[512], path[640], buf[4096], discs[512];
   DIR *d; struct dirent *de; int pi = g_nplat;
   snprintf(dir, sizeof(dir), "%s/games", g_root);
   if (!(d = opendir(dir))) { logf_("no games dir %s", dir); return; }
   snprintf(g_plat[pi].name, sizeof(g_plat[pi].name), "PlayStation");
   snprintf(g_plat[pi].core, sizeof(g_plat[pi].core), "pcsx_rearmed_libretro.so");
   while ((de = readdir(d)) && g_ngames < MAX_GAMES)
   {
      FILE *f; size_t n; game_t *g; char *c;
      if (de->d_name[0] == '.') continue;
      snprintf(path, sizeof(path), "%s/%s/Game.ini", dir, de->d_name);
      if (!(f = fopen(path, "rb"))) continue;
      buf[0] = '\n';
      n = fread(buf + 1, 1, sizeof(buf) - 2, f); fclose(f);
      buf[n + 1] = 0;
      g = &g_games[g_ngames];
      memset(g, 0, sizeof(*g));
      ini_get(buf, "Title", g->title, sizeof(g->title));
      ini_get(buf, "Publisher", g->publisher, sizeof(g->publisher));
      ini_get(buf, "Year", g->year, sizeof(g->year));
      ini_get(buf, "Players", g->players, sizeof(g->players));
      ini_get(buf, "Discs", discs, sizeof(discs));
      if ((c = strchr(discs, ','))) *c = 0;
      trim(discs);
      if (!discs[0]) continue;
      snprintf(g->content, sizeof(g->content), "%s/%s/%s.cue", dir, de->d_name, discs);
      if (!file_exists(g->content))
      {
         snprintf(g->content, sizeof(g->content), "%s/%s/%s.pbp", dir, de->d_name, discs);
         if (!file_exists(g->content)) continue;
      }
      snprintf(g->cover, sizeof(g->cover), "%s/%s/%s.png", dir, de->d_name, discs);
      g->has_cover = file_exists(g->cover);
      {
         static const char *const bgn[] = { "background.jpg", "background.png", "bg.jpg", "bg.png", NULL };
         int b;
         for (b = 0; bgn[b]; b++)
         {
            snprintf(g->bg, sizeof(g->bg), "%s/%s/%s", dir, de->d_name, bgn[b]);
            if (file_exists(g->bg)) break;
            g->bg[0] = 0;
         }
      }
      if (!g->title[0]) snprintf(g->title, sizeof(g->title), "%s", discs);
      g->platform = pi;
      g_ngames++;
   }
   closedir(d);
   g_nplat++;
}

static void build_platform_index(void)
{
   int i, p, kept = 0;
   qsort(g_games, g_ngames, sizeof(game_t), cmp_title);
   for (p = 0; p < g_nplat; p++) { g_plat[p].first = -1; g_plat[p].count = 0; }
   for (i = 0; i < g_ngames; i++)
   {
      platform_t *pl = &g_plat[g_games[i].platform];
      if (pl->first < 0) pl->first = i;
      pl->count++;
   }
   /* drop empty platforms */
   for (p = 0; p < g_nplat; p++)
      if (g_plat[p].count > 0) g_plat[kept++] = g_plat[p];
   g_nplat = kept;
}

/* ------------------------------------------------------------------------ */
/* GL resources                                                              */
static GLuint g_prog_box, g_prog_2d, g_vbo_quad, g_vao_quad;
static GLint  u_mvp, u_model, u_tex, u_tint, u_alpha, u_mode, u_light, u_cam;
static GLint  u2_mvp, u2_tex, u2_color, u2_mode;
static GLuint g_white;
static TTF_Font *g_font_title, *g_font_sub, *g_font_small, *g_font_cover;

static const char *VS_BOX =
   "#version 300 es\n"
   "layout(location=0) in vec3 pos; layout(location=1) in vec2 uv; layout(location=2) in vec3 nrm;\n"
   "uniform mat4 mvp, model;\n"
   "out vec2 v_uv; out vec3 v_n; out vec3 v_w;\n"
   "void main(){ v_uv=uv; v_n=mat3(model)*nrm; v_w=(model*vec4(pos,1.0)).xyz; gl_Position=mvp*vec4(pos,1.0); }\n";

/* modes: 0 lit texture (insert, back card), 1 lit flat colour (tray plastic),
 *        2 floor reflection, 3 black spine with printed title, 4 clear glass lid */
static const char *FS_BOX =
   "#version 300 es\n"
   "precision mediump float;\n"
   "in vec2 v_uv; in vec3 v_n; in vec3 v_w;\n"
   "uniform sampler2D tex; uniform vec3 tint; uniform float alpha; uniform int mode;\n"
   "uniform vec3 light; uniform vec3 cam;\n"
   "out vec4 frag;\n"
   "void main(){\n"
   "  vec3 n = normalize(v_n);\n"
   "  vec3 l = normalize(light);\n"
   "  vec3 v = normalize(cam - v_w);\n"
   "  float ndl = max(dot(n, l), 0.0);\n"
   "  if (mode==2) { vec3 c = texture(tex, v_uv).rgb * tint; float f = smoothstep(0.62, 1.0, v_uv.y);\n"
   "                 frag = vec4(c, alpha * f * f * 0.20); return; }\n"
   "  if (mode==4) {\n"
   "    float ndv = clamp(abs(dot(n, v)), 0.0, 1.0);\n"
   "    float fres = 0.04 + 0.96 * pow(1.0 - ndv, 4.0);\n"
   "    vec3 r = reflect(-v, n);\n"
   "    float band = smoothstep(0.45, 0.95, r.y) * 0.45;\n"
   "    float d = v_uv.x - (1.0 - v_uv.y) * 0.55 - 0.18 + r.x * 0.9;\n"
   "    float streak = exp(-d * d * 60.0) * 0.30 + exp(-(d - 0.16) * (d - 0.16) * 260.0) * 0.12;\n"
   "    float spec = pow(max(dot(reflect(-l, n), v), 0.0), 70.0) * 0.7;\n"
   "    vec2 e = min(v_uv, 1.0 - v_uv);\n"
   "    float rim = 1.0 - smoothstep(0.0, 0.016, min(e.x, e.y));\n"
   "    float hinge = 1.0 - step(0.088, v_uv.x);\n"
   "    float ridges = smoothstep(0.009, 0.0, abs(v_uv.x - 0.020)) + smoothstep(0.009, 0.0, abs(v_uv.x - 0.072));\n"
   "    float a = 0.035 + fres * 0.45 + band * 0.35 + streak + spec + rim * 0.40 + ridges * 0.40 + hinge * 0.08;\n"
   "    frag = vec4(vec3(0.93, 0.96, 1.0), clamp(a, 0.0, 0.92) * alpha); return; }\n"
   "  vec3 base;\n"
   "  if (mode==1) base = tint;\n"
   "  else if (mode==3) { float t = texture(tex, vec2(1.0 - v_uv.y, v_uv.x)).a; base = mix(vec3(0.030, 0.030, 0.036), vec3(0.90), t); }\n"
   "  else base = texture(tex, v_uv).rgb * tint;\n"
   "  float diff = 0.50 + 0.50 * ndl;\n"
   "  float spec = pow(max(dot(reflect(-l, n), v), 0.0), (mode==0) ? 18.0 : 40.0) * ((mode==0) ? 0.10 : 0.35);\n"
   "  frag = vec4(base * diff + vec3(spec), alpha);\n"
   "}\n";

static const char *VS_2D =
   "#version 300 es\n"
   "layout(location=0) in vec3 pos; layout(location=1) in vec2 uv;\n"
   "uniform mat4 mvp; out vec2 v_uv;\n"
   "void main(){ v_uv=uv; gl_Position=mvp*vec4(pos,1.0); }\n";

static const char *FS_2D =
   "#version 300 es\n"
   "precision mediump float;\n"
   "in vec2 v_uv; uniform sampler2D tex; uniform vec4 color; uniform int mode; out vec4 frag;\n"
   "void main(){\n"
   "  vec4 t = texture(tex, v_uv);\n"
   "  if (mode==1) { vec2 d = abs(v_uv - 0.5) * 2.0; float r = length(max(d - vec2(0.82, 0.0), 0.0)) ;\n"
   "                 float a = 1.0 - smoothstep(0.96, 1.0, max(d.y, r)); frag = vec4(color.rgb, color.a * a); return; }\n"
   "  if (mode==2) { float d = length((v_uv - 0.5) * vec2(1.6, 1.0)); frag = vec4(0.0,0.0,0.0, color.a * smoothstep(0.35, 0.95, d)); return; }\n"
   "  if (mode==3) { float d = length((v_uv - 0.5) * 2.0); frag = vec4(color.rgb, color.a * pow(clamp(1.0 - d, 0.0, 1.0), 2.2)); return; }\n"
   "  if (mode==4) { float dv = length((v_uv - 0.5) * vec2(1.6, 1.0)); float av = 0.55 * smoothstep(0.35, 0.95, dv);\n"
   "                 vec2 q = (v_uv - vec2(0.5, 0.4167)) / vec2(0.328, 0.4167); float as = color.a * pow(clamp(1.0 - length(q), 0.0, 1.0), 2.2);\n"
   "                 float a = as + av * (1.0 - as); frag = vec4(color.rgb * as / max(a, 0.001), a); return; }\n"
   "  frag = t * color;\n"
   "}\n";

static GLuint compile(GLenum type, const char *src)
{
   GLint ok; char info[1024];
   GLuint s = glCreateShader(type);
   glShaderSource(s, 1, &src, NULL);
   glCompileShader(s);
   glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
   if (!ok) { glGetShaderInfoLog(s, sizeof(info), NULL, info); logf_("shader error: %s", info); }
   return s;
}

static GLuint link_prog(const char *vs, const char *fs)
{
   GLint ok; char info[1024];
   GLuint p = glCreateProgram();
   glAttachShader(p, compile(GL_VERTEX_SHADER, vs));
   glAttachShader(p, compile(GL_FRAGMENT_SHADER, fs));
   glLinkProgram(p);
   glGetProgramiv(p, GL_LINK_STATUS, &ok);
   if (!ok) { glGetProgramInfoLog(p, sizeof(info), NULL, info); logf_("link error: %s", info); }
   return p;
}

static void gl_setup(void)
{
   /* unit quad in XY, centred, facing +Z: pos(3) uv(2) normal(3) */
   static const float q[] = {
      -0.5f, -0.5f, 0, 0, 1, 0, 0, 1,
       0.5f, -0.5f, 0, 1, 1, 0, 0, 1,
       0.5f,  0.5f, 0, 1, 0, 0, 0, 1,
      -0.5f, -0.5f, 0, 0, 1, 0, 0, 1,
       0.5f,  0.5f, 0, 1, 0, 0, 0, 1,
      -0.5f,  0.5f, 0, 0, 0, 0, 0, 1,
   };
   static const unsigned char white[4] = { 255, 255, 255, 255 };
   g_prog_box = link_prog(VS_BOX, FS_BOX);
   u_mvp = glGetUniformLocation(g_prog_box, "mvp");
   u_model = glGetUniformLocation(g_prog_box, "model");
   u_tex = glGetUniformLocation(g_prog_box, "tex");
   u_tint = glGetUniformLocation(g_prog_box, "tint");
   u_alpha = glGetUniformLocation(g_prog_box, "alpha");
   u_mode = glGetUniformLocation(g_prog_box, "mode");
   u_light = glGetUniformLocation(g_prog_box, "light");
   u_cam = glGetUniformLocation(g_prog_box, "cam");
   g_prog_2d = link_prog(VS_2D, FS_2D);
   u2_mvp = glGetUniformLocation(g_prog_2d, "mvp");
   u2_tex = glGetUniformLocation(g_prog_2d, "tex");
   u2_color = glGetUniformLocation(g_prog_2d, "color");
   u2_mode = glGetUniformLocation(g_prog_2d, "mode");

   glGenVertexArrays(1, &g_vao_quad);
   glBindVertexArray(g_vao_quad);
   glGenBuffers(1, &g_vbo_quad);
   glBindBuffer(GL_ARRAY_BUFFER, g_vbo_quad);
   glBufferData(GL_ARRAY_BUFFER, sizeof(q), q, GL_STATIC_DRAW);
   glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 32, (void *)0);
   glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 32, (void *)12);
   glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, 32, (void *)20);
   glEnableVertexAttribArray(0);
   glEnableVertexAttribArray(1);
   glEnableVertexAttribArray(2);

   glGenTextures(1, &g_white);
   glBindTexture(GL_TEXTURE_2D, g_white);
   glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
}

static void surface_avg(SDL_Surface *c, float *avg)
{
   double r = 0, g = 0, b = 0; long n = 0; int x, y;
   for (y = 0; y < c->h; y += 4)
      for (x = 0; x < c->w; x += 4)
      {
         const unsigned char *p = (const unsigned char *)c->pixels + y * c->pitch + x * 4;
         r += p[0]; g += p[1]; b += p[2]; n++;
      }
   if (n) { avg[0] = r / n / 255.0; avg[1] = g / n / 255.0; avg[2] = b / n / 255.0; }
}

/* Upload an SDL surface as RGBA (ABGR8888 == R,G,B,A bytes on little endian). */
static GLuint surface_to_tex(SDL_Surface *s, int mip, float *avg)
{
   GLuint t; SDL_Surface *c = SDL_ConvertSurfaceFormat(s, SDL_PIXELFORMAT_ABGR8888, 0);
   if (!c) return 0;
   if (avg)
   {
      double r = 0, g = 0, b = 0; long n = 0; int x, y;
      for (y = 0; y < c->h; y += 4)
         for (x = 0; x < c->w; x += 4)
         {
            const unsigned char *p = (const unsigned char *)c->pixels + y * c->pitch + x * 4;
            r += p[0]; g += p[1]; b += p[2]; n++;
         }
      if (n) { avg[0] = r / n / 255.0; avg[1] = g / n / 255.0; avg[2] = b / n / 255.0; }
   }
   glGenTextures(1, &t);
   glBindTexture(GL_TEXTURE_2D, t);
   glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
   glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, c->w, c->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, c->pixels);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mip ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
   if (mip) glGenerateMipmap(GL_TEXTURE_2D);
   SDL_FreeSurface(c);
   return t;
}

static GLuint text_tex(TTF_Font *f, const char *s, int *w, int *h)
{
   SDL_Color white = { 255, 255, 255, 255 };
   SDL_Surface *surf; GLuint t;
   if (!f || !s || !*s) { *w = *h = 0; return 0; }
   surf = TTF_RenderUTF8_Blended(f, s, white);
   if (!surf) { *w = *h = 0; return 0; }
   *w = surf->w; *h = surf->h;
   t = surface_to_tex(surf, 0, NULL);
   SDL_FreeSurface(surf);
   return t;
}

static unsigned hash_str(const char *s) { unsigned h = 2166136261u; while (*s) h = (h ^ (unsigned char)*s++) * 16777619u; return h; }

/* Back of the case / placeholder front: coloured card with wrapped text. */
static GLuint card_tex(const game_t *g, int back)
{
   SDL_Surface *card = SDL_CreateRGBSurface(0, 256, 256, 32, 0x000000ff, 0x0000ff00, 0x00ff0000, 0xff000000);
   SDL_Color white = { 255, 255, 255, 255 };
   SDL_Rect r; GLuint t; char line[160]; int y = 18;
   Uint8 cr = (Uint8)(g->col[0] * 255), cg = (Uint8)(g->col[1] * 255), cb = (Uint8)(g->col[2] * 255);
   if (!card) return 0;
   if (back) { cr = cr / 3 + 12; cg = cg / 3 + 12; cb = cb / 3 + 18; }
   SDL_FillRect(card, NULL, SDL_MapRGBA(card->format, cr, cg, cb, 255));
#define LINE(font, txt) do { SDL_Surface *ts = TTF_RenderUTF8_Blended_Wrapped(font, txt, white, 224); \
      if (ts) { r.x = 16; r.y = y; r.w = ts->w; r.h = ts->h; SDL_BlitSurface(ts, NULL, card, &r); y += ts->h + 6; SDL_FreeSurface(ts); } } while (0)
   if (g_font_cover)
   {
      if (!back) y = 150;
      LINE(g_font_cover, g->title);
      if (back)
      {
         snprintf(line, sizeof(line), "%s%s%s", g->year, g->year[0] && g->publisher[0] ? "  ·  " : "", g->publisher);
         if (line[0]) LINE(g_font_small, line);
         if (g->players[0]) { snprintf(line, sizeof(line), "%s jugador%s", g->players, strcmp(g->players, "1") ? "es" : ""); LINE(g_font_small, line); }
         y = 220; LINE(g_font_small, "Memory card 1");
      }
   }
#undef LINE
   t = surface_to_tex(card, 1, NULL);
   SDL_FreeSurface(card);
   return t;
}

/* Spine print: the title in white on a transparent strip (sampled rotated). */
static GLuint spine_tex(const game_t *g)
{
   SDL_Color white = { 255, 255, 255, 255 };
   SDL_Surface *strip, *ts;
   SDL_Rect r; GLuint t;
   if (!g_font_small) return 0;
   strip = SDL_CreateRGBSurface(0, 512, 48, 32, 0x000000ff, 0x0000ff00, 0x00ff0000, 0xff000000);
   if (!strip) return 0;
   SDL_FillRect(strip, NULL, SDL_MapRGBA(strip->format, 0, 0, 0, 0));
   ts = TTF_RenderUTF8_Blended(g_font_small, g->title, white);
   if (ts)
   {
      SDL_SetSurfaceBlendMode(ts, SDL_BLENDMODE_NONE);
      r.w = ts->w > 470 ? 470 : ts->w; r.h = ts->h > 46 ? 46 : ts->h;
      r.x = 24; r.y = (48 - r.h) / 2;
      SDL_BlitSurface(ts, NULL, strip, &r);
      SDL_FreeSurface(ts);
   }
   t = surface_to_tex(strip, 1, NULL);
   SDL_FreeSurface(strip);
   return t;
}

static void game_placeholder(game_t *g)
{
   unsigned h = hash_str(g->title);
   g->col[0] = 0.25f + (h & 255) / 512.0f;
   g->col[1] = 0.20f + ((h >> 8) & 255) / 640.0f;
   g->col[2] = 0.30f + ((h >> 16) & 255) / 512.0f;
   g->tex = card_tex(g, 0);
   g->tex_state = -1;
   g->spine_tex = spine_tex(g);
}

static void game_finish_cover(game_t *g, SDL_Surface *s, const float avg[3])
{
   if (s)
   {
      g->tex = surface_to_tex(s, 1, NULL);
      memcpy(g->col, avg, sizeof(g->col));
      g->tex_state = 1;
      g->spine_tex = spine_tex(g);
   }
   if (!g->tex) game_placeholder(g);
}

static void game_free_tex(game_t *g)
{
   if (g->tex) glDeleteTextures(1, &g->tex);
   if (g->back_tex) glDeleteTextures(1, &g->back_tex);
   if (g->spine_tex) glDeleteTextures(1, &g->spine_tex);
   g->tex = g->back_tex = g->spine_tex = 0;
   g->tex_state = 0;
}

/* ------------------------------------------------------------------------ */
/* drawing                                                                   */
static mat4 g_proj, g_view, g_vp, g_ortho;

static void draw_quad_box(mat4 model, GLuint tex, const float tint[3], float alpha, int mode)
{
   mat4 mvp = m4_mul(g_vp, model);
   glUniformMatrix4fv(u_mvp, 1, GL_FALSE, mvp.m);
   glUniformMatrix4fv(u_model, 1, GL_FALSE, model.m);
   glUniform3fv(u_tint, 1, tint);
   glUniform1f(u_alpha, alpha);
   glUniform1i(u_mode, mode);
   glBindTexture(GL_TEXTURE_2D, tex ? tex : g_white);
   glDrawArrays(GL_TRIANGLES, 0, 6);
}

/* PS1 jewel case, height 1.0: 142 x 125 x 10 mm -> width 1.136, depth 0.08.
 * Square paper insert on the right, hinge strip on the left, black tray, glass lid. */
#define CASE_W  1.136f
#define CASE_D  0.080f
#define HINGE   0.105f
#define INSET_Z 0.012f

static void case_part(mat4 base, float x, float y, float z, float ry, float rx, float sx, float sy,
                      GLuint tex, const float tint[3], float alpha, int mode)
{
   mat4 m = m4_mul(base, m4_mul(m4_translate(x, y, z), m4_mul(m4_rot_y(ry), m4_mul(m4_rot_x(rx), m4_scale(sx, sy, 1)))));
   draw_quad_box(m, tex, tint, alpha, mode);
}

static void draw_case(const game_t *g, mat4 base, float alpha, float flip)
{
   static const float one[3] = { 1, 1, 1 };
   static const float tray[3] = { 0.045f, 0.045f, 0.052f };
   static const float edge[3] = { 0.16f, 0.17f, 0.19f };
   const float hw = CASE_W * 0.5f, hd = CASE_D * 0.5f, PI = (float)M_PI;
   const float ins_l = -hw + HINGE, ins_r = hw - 0.018f, ins_w = ins_r - ins_l;
   base = m4_mul(base, m4_rot_y(flip * PI));

   /* opaque: back card, tray edges, spine, insert, hinge strip */
   if (g->back_tex) case_part(base, 0, 0, -hd, PI, 0, CASE_W, 1.0f, g->back_tex, one, alpha, 0);
   else             case_part(base, 0, 0, -hd, PI, 0, CASE_W, 1.0f, 0, tray, alpha, 1);
   case_part(base, hw, 0, 0, PI / 2, 0, CASE_D, 1.0f, 0, edge, alpha, 1);
   case_part(base, 0, 0.5f, 0, 0, -PI / 2, CASE_W, CASE_D, 0, edge, alpha, 1);
   case_part(base, 0, -0.5f, 0, 0, PI / 2, CASE_W, CASE_D, 0, edge, alpha, 1);
   case_part(base, -hw, 0, 0, -PI / 2, 0, CASE_D, 1.0f, g->spine_tex, one, alpha, 3);
   case_part(base, (ins_l + ins_r) * 0.5f, 0, hd - INSET_Z, 0, 0, ins_w, 0.972f, g->tex, one, alpha, 0);
   case_part(base, -hw + HINGE * 0.5f, 0, hd - INSET_Z - 0.002f, 0, 0, HINGE, 1.0f, 0, tray, alpha, 1);

   /* transparent: glass lid (no depth writes so the insert stays visible) */
   glDepthMask(GL_FALSE);
   case_part(base, 0, 0, hd, 0, 0, CASE_W, 1.0f, 0, one, alpha, 4);
   glDepthMask(GL_TRUE);
}

/* Mirror the insert about the floor plane under the case (y = -0.5*scale). */
static void draw_reflection(const game_t *g, mat4 base, float alpha)
{
   static const float one[3] = { 1, 1, 1 };
   const float hw = CASE_W * 0.5f, ins_l = -hw + HINGE, ins_r = hw - 0.018f;
   float floor_y = -0.5f * g->sc.x - 0.01f;
   mat4 m = m4_mul(m4_translate(0, 2.0f * floor_y, 0), m4_mul(m4_scale(1, -1, 1), base));
   m = m4_mul(m, m4_mul(m4_translate((ins_l + ins_r) * 0.5f, 0, CASE_D * 0.5f), m4_scale(ins_r - ins_l, 0.972f, 1)));
   draw_quad_box(m, g->tex, one, alpha, 2);
}

static void draw_2d(GLuint tex, float x, float y, float w, float h, const float rgba[4], int mode)
{
   mat4 m = m4_mul(g_ortho, m4_mul(m4_translate(x + w * 0.5f, y + h * 0.5f, 0), m4_scale(w, -h, 1)));
   glUniformMatrix4fv(u2_mvp, 1, GL_FALSE, m.m);
   glUniform4fv(u2_color, 1, rgba);
   glUniform1i(u2_mode, mode);
   glBindTexture(GL_TEXTURE_2D, tex ? tex : g_white);
   glDrawArrays(GL_TRIANGLES, 0, 6);
}

/* ------------------------------------------------------------------------ */
typedef struct { GLuint tex; int w, h; char key[200]; } label_t;
static void label_set(label_t *l, TTF_Font *f, const char *s)
{
   if (!strcmp(l->key, s) && (l->tex || !*s)) return;
   if (l->tex) glDeleteTextures(1, &l->tex);
   snprintf(l->key, sizeof(l->key), "%s", s);
   l->tex = text_tex(f, s, &l->w, &l->h);
}

static TTF_Font *open_font(const char **paths, int size)
{
   int i;
   for (i = 0; paths[i]; i++)
   {
      TTF_Font *f = TTF_OpenFont(paths[i], size);
      if (f) return f;
   }
   logf_("no font found for size %d", size);
   return NULL;
}

/* ------------------------------------------------------------------------ */
/* covers: decoded on a worker thread, uploaded on the main thread           */
static SDL_mutex   *cv_mx;
static SDL_cond    *cv_cv;
static int          cv_req = -1, cv_busy, cv_done = -1, cv_quit;
static char         cv_path[512];
static SDL_Surface *cv_surf;
static float        cv_avg[3];

static int cover_worker(void *unused)
{
   (void)unused;
   SDL_LockMutex(cv_mx);
   while (!cv_quit)
   {
      int idx; char path[512]; SDL_Surface *s, *c = NULL; float avg[3] = { 0.3f, 0.3f, 0.35f };
      while (cv_req < 0 && !cv_quit) SDL_CondWait(cv_cv, cv_mx);
      if (cv_quit) break;
      idx = cv_req; cv_req = -1;
      snprintf(path, sizeof(path), "%s", cv_path);
      SDL_UnlockMutex(cv_mx);
      s = IMG_Load(path);
      if (s) { c = SDL_ConvertSurfaceFormat(s, SDL_PIXELFORMAT_ABGR8888, 0); SDL_FreeSurface(s); }
      if (c) surface_avg(c, avg);
      SDL_LockMutex(cv_mx);
      cv_surf = c; cv_done = idx;
      memcpy(cv_avg, avg, sizeof(avg));
   }
   SDL_UnlockMutex(cv_mx);
   return 0;
}

static void cover_request(int idx, const char *path)
{
   SDL_LockMutex(cv_mx);
   snprintf(cv_path, sizeof(cv_path), "%s", path);
   cv_req = idx; cv_busy = 1;
   SDL_CondSignal(cv_cv);
   SDL_UnlockMutex(cv_mx);
}

/* ------------------------------------------------------------------------ */
/* per-game background: decoded on a worker thread, crossfaded on the GPU    */
static SDL_mutex   *bg_mx;
static SDL_cond    *bg_cv;
static char         bg_req[512];      /* path requested (guarded by bg_mx) */
static int          bg_req_new;
static SDL_Surface *bg_done;          /* decoded surface ready to upload   */
static char         bg_done_path[512];
static int          bg_quit;

static int bg_worker(void *unused)
{
   (void)unused;
   SDL_LockMutex(bg_mx);
   while (!bg_quit)
   {
      char path[512]; SDL_Surface *s, *c = NULL;
      while (!bg_req_new && !bg_quit) SDL_CondWait(bg_cv, bg_mx);
      if (bg_quit) break;
      snprintf(path, sizeof(path), "%s", bg_req);
      bg_req_new = 0;
      SDL_UnlockMutex(bg_mx);
      s = path[0] ? IMG_Load(path) : NULL;
      if (s) { c = SDL_ConvertSurfaceFormat(s, SDL_PIXELFORMAT_ABGR8888, 0); SDL_FreeSurface(s); }
      SDL_LockMutex(bg_mx);
      if (bg_done) SDL_FreeSurface(bg_done);
      bg_done = c;
      snprintf(bg_done_path, sizeof(bg_done_path), "%s", path);
      if (!c && path[0]) logf_("background failed: %s", path);
   }
   SDL_UnlockMutex(bg_mx);
   return 0;
}

static void bg_request(const char *path)
{
   SDL_LockMutex(bg_mx);
   snprintf(bg_req, sizeof(bg_req), "%s", path);
   bg_req_new = 1;
   SDL_CondSignal(bg_cv);
   SDL_UnlockMutex(bg_mx);
}

/* ------------------------------------------------------------------------ */
/* input: game controllers + raw joysticks mapped to logical actions         */
enum { IN_LEFT, IN_RIGHT, IN_UP, IN_DOWN, IN_L1, IN_R1, IN_PLAY, IN_START, IN_SELECT, IN_COUNT };
static int           in_down[IN_COUNT];     /* current state                  */
static int           in_pressed[IN_COUNT];  /* edge this frame                */
static SDL_JoystickID in_ctrl_ids[8];       /* joysticks opened as controllers */
static int           in_nctrl;

static void in_set(int b, int down)
{
   if (b < 0) return;
   if (down && !in_down[b]) in_pressed[b] = 1;
   in_down[b] = down;
}

static int in_is_ctrl(SDL_JoystickID id)
{
   int i;
   for (i = 0; i < in_nctrl; i++) if (in_ctrl_ids[i] == id) return 1;
   return 0;
}

static void in_open(int index)
{
   char guid[64];
   SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(index), guid, sizeof(guid));
   if (SDL_IsGameController(index))
   {
      SDL_GameController *c = SDL_GameControllerOpen(index);
      if (c && in_nctrl < 8)
      {
         in_ctrl_ids[in_nctrl++] = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(c));
         logf_("controller %d: %s (guid %s)", index, SDL_GameControllerName(c), guid);
         return;
      }
   }
   if (SDL_JoystickOpen(index))
      logf_("joystick %d (raw): %s (guid %s)", index, SDL_JoystickNameForIndex(index), guid);
   else
      logf_("joystick %d failed: %s", index, SDL_GetError());
}

/* Raw button layout of the PlayStation Classic pad (and most PS-style pads):
 * 0 triangle, 1 circle, 2 cross, 3 square, 4 L2, 5 R2, 6 L1, 7 R1, 8 select, 9 start */
static int in_raw_button(int b)
{
   switch (b)
   {
   case 1: case 2: return IN_PLAY;
   case 6: return IN_L1;
   case 7: return IN_R1;
   case 8: return IN_SELECT;
   case 9: return IN_START;
   }
   return -1;
}

static void in_axis(int axis, int value)
{
   int neg = value < -16000, pos = value > 16000;
   if (axis == 0) { in_set(IN_LEFT, neg); in_set(IN_RIGHT, pos); }
   if (axis == 1) { in_set(IN_UP, neg); in_set(IN_DOWN, pos); }
}

static void in_event(const SDL_Event *e)
{
   switch (e->type)
   {
   case SDL_JOYDEVICEADDED:
      if (!SDL_IsGameController(e->jdevice.which)) in_open(e->jdevice.which);
      break;
   case SDL_CONTROLLERDEVICEADDED:
      in_open(e->cdevice.which);
      break;
   case SDL_CONTROLLERBUTTONDOWN: case SDL_CONTROLLERBUTTONUP:
   {
      int d = e->type == SDL_CONTROLLERBUTTONDOWN, b = -1;
      switch (e->cbutton.button)
      {
      case SDL_CONTROLLER_BUTTON_DPAD_LEFT: b = IN_LEFT; break;
      case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: b = IN_RIGHT; break;
      case SDL_CONTROLLER_BUTTON_DPAD_UP: b = IN_UP; break;
      case SDL_CONTROLLER_BUTTON_DPAD_DOWN: b = IN_DOWN; break;
      case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: b = IN_L1; break;
      case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: b = IN_R1; break;
      case SDL_CONTROLLER_BUTTON_A: case SDL_CONTROLLER_BUTTON_B: b = IN_PLAY; break;
      case SDL_CONTROLLER_BUTTON_START: b = IN_START; break;
      case SDL_CONTROLLER_BUTTON_BACK: b = IN_SELECT; break;
      }
      in_set(b, d);
      break;
   }
   case SDL_CONTROLLERAXISMOTION:
      if (e->caxis.axis == SDL_CONTROLLER_AXIS_LEFTX) in_axis(0, e->caxis.value);
      if (e->caxis.axis == SDL_CONTROLLER_AXIS_LEFTY) in_axis(1, e->caxis.value);
      break;
   case SDL_JOYBUTTONDOWN: case SDL_JOYBUTTONUP:
      if (!in_is_ctrl(e->jbutton.which))
         in_set(in_raw_button(e->jbutton.button), e->type == SDL_JOYBUTTONDOWN);
      break;
   case SDL_JOYAXISMOTION:
      if (!in_is_ctrl(e->jaxis.which)) in_axis(e->jaxis.axis, e->jaxis.value);
      break;
   case SDL_JOYHATMOTION:
      if (!in_is_ctrl(e->jhat.which))
      {
         in_set(IN_LEFT, (e->jhat.value & SDL_HAT_LEFT) != 0);
         in_set(IN_RIGHT, (e->jhat.value & SDL_HAT_RIGHT) != 0);
         in_set(IN_UP, (e->jhat.value & SDL_HAT_UP) != 0);
         in_set(IN_DOWN, (e->jhat.value & SDL_HAT_DOWN) != 0);
      }
      break;
   case SDL_KEYDOWN: case SDL_KEYUP:
   {
      int d = e->type == SDL_KEYDOWN, b = -1;
      if (e->key.repeat) break;
      if (d) logf_("key: sym 0x%x scancode %d (%s)", (unsigned)e->key.keysym.sym,
                   (int)e->key.keysym.scancode, SDL_GetKeyName(e->key.keysym.sym));
      switch (e->key.keysym.sym)
      {
      case SDLK_LEFT: b = IN_LEFT; break;
      case SDLK_RIGHT: b = IN_RIGHT; break;
      case SDLK_UP: case SDLK_PAGEUP: b = IN_L1; break;
      case SDLK_DOWN: case SDLK_PAGEDOWN: b = IN_R1; break;
      case SDLK_RETURN: b = IN_PLAY; break;
      case SDLK_ESCAPE: b = IN_SELECT; in_set(IN_START, d); break;
      }
      in_set(b, d);
      break;
   }
   }
}

/* ------------------------------------------------------------------------ */
/* help bar: button icons ($home/icons/<name>.png, optional) + labels        */
typedef struct { const char *icons[3]; const char *fallback; const char *text; } hint_t;
static const hint_t g_hints[] = {
   { { "cross", "circle", NULL }, "\xE2\x9C\x95 / \xE2\x97\x8B", "Jugar" },
   { { "l1", "r1", NULL },        "L1 / R1",                     "Plataforma" },
   { { "select", "start", NULL }, "Select + Start",              "Salir" },
};
#define NHINTS ((int)(sizeof(g_hints) / sizeof(g_hints[0])))
typedef struct { GLuint icon[3]; int iw[3], ih[3]; GLuint fb, txt; int fbw, fbh, tw, th; } hint_gl_t;
static hint_gl_t g_hint_gl[NHINTS];

static void hints_load(const char *home, TTF_Font *font)
{
   int i, k;
   for (i = 0; i < NHINTS; i++)
   {
      hint_gl_t *h = &g_hint_gl[i];
      int have = 1;
      for (k = 0; k < 3 && g_hints[i].icons[k]; k++)
      {
         char path[400]; SDL_Surface *sf;
         snprintf(path, sizeof(path), "%s/icons/%s.png", home, g_hints[i].icons[k]);
         sf = IMG_Load(path);
         if (sf) { h->iw[k] = sf->w; h->ih[k] = sf->h; h->icon[k] = surface_to_tex(sf, 0, NULL); SDL_FreeSurface(sf); }
         if (!h->icon[k]) have = 0;
      }
      if (!have)
         h->fb = text_tex(font, g_hints[i].fallback, &h->fbw, &h->fbh);
      h->txt = text_tex(font, g_hints[i].text, &h->tw, &h->th);
   }
}

/* draw the hints centred on y; returns nothing, icons scaled to icon_h */
static void hints_draw(float y, float alpha)
{
   const float icon_h = 28.0f, gap_icon = 6.0f, gap_txt = 10.0f, gap_hint = 42.0f;
   const float ic[4] = { 1, 1, 1, alpha }, tc[4] = { 0.62f, 0.61f, 0.72f, alpha };
   float total = 0, x;
   int i, k;
   for (i = 0; i < NHINTS; i++)
   {
      hint_gl_t *h = &g_hint_gl[i];
      if (h->fb) total += h->fbw;
      else for (k = 0; k < 3 && h->icon[k]; k++) total += icon_h * h->iw[k] / (float)h->ih[k] + gap_icon;
      total += gap_txt + h->tw + (i < NHINTS - 1 ? gap_hint : 0);
   }
   x = (W - total) * 0.5f;
   for (i = 0; i < NHINTS; i++)
   {
      hint_gl_t *h = &g_hint_gl[i];
      if (h->fb) { draw_2d(h->fb, x, y - h->fbh * 0.5f, h->fbw, h->fbh, tc, 0); x += h->fbw; }
      else for (k = 0; k < 3 && h->icon[k]; k++)
      {
         float w = icon_h * h->iw[k] / (float)h->ih[k];
         draw_2d(h->icon[k], x, y - icon_h * 0.5f, w, icon_h, ic, 0);
         x += w + gap_icon;
      }
      x += gap_txt;
      draw_2d(h->txt, x, y - h->th * 0.5f, h->tw, h->th, tc, 0);
      x += h->tw + gap_hint;
   }
}

typedef struct { GLuint tex; int w, h; } bgtex_t;
static bgtex_t bg_cur, bg_next;
static float   bg_mix = 1.0f;          /* 0 -> showing cur, 1 -> next done */
static char    bg_shown[512];          /* path of the background we want   */

static void bg_poll_upload(void)
{
   SDL_Surface *s = NULL; char path[512] = "";
   SDL_LockMutex(bg_mx);
   s = bg_done; bg_done = NULL;
   snprintf(path, sizeof(path), "%s", bg_done_path);
   SDL_UnlockMutex(bg_mx);
   if (!s) return;
   if (strcmp(path, bg_shown)) { SDL_FreeSurface(s); return; } /* stale */
   if (bg_cur.tex && bg_mix >= 1.0f) { glDeleteTextures(1, &bg_cur.tex); bg_cur.tex = 0; }
   if (bg_next.tex) { if (bg_cur.tex) glDeleteTextures(1, &bg_cur.tex); bg_cur = bg_next; }
   bg_next.tex = surface_to_tex(s, 0, NULL);
   bg_next.w = s->w; bg_next.h = s->h;
   SDL_FreeSurface(s);
   bg_mix = 0.0f;
}

/* draw a background texture "cover"-fitted with a slow Ken Burns zoom */
static void bg_draw(const bgtex_t *b, float alpha, double t)
{
   float sx, sy, sc, w, h;
   const float c[4] = { 1, 1, 1, alpha };
   if (!b->tex || alpha <= 0.001f) return;
   sx = (float)W / b->w; sy = (float)H / b->h;
   sc = (sx > sy ? sx : sy) * (1.05f + 0.03f * sinf((float)t * 0.07f));
   w = b->w * sc; h = b->h * sc;
   draw_2d(b->tex, (W - w) * 0.5f + 12.0f * sinf((float)t * 0.05f), (H - h) * 0.5f, w, h, c, 0);
}

int main(int argc, char **argv)
{
   char font_b[300], font_r[300], padb[300];
   const char *fonts_bold[] = { font_b, "/usr/share/fonts/ttf/LiberationSans-Bold.ttf", NULL };
   const char *fonts_reg[]  = { font_r, "/usr/share/fonts/ttf/LiberationSans-Regular.ttf", NULL };
   const char *log_path = NULL;
   SDL_Window *win; SDL_GLContext ctx;
   SDL_GameController *pad = NULL;
   double t0 = now_s(), last, fps_t, info_t = 0, play_t = -1;
   int i, sel_plat = 0, sel = 0, running = 1, exit_code = 0, frames = 0;
   int held_dir = 0; double held_next = 0, sel_t = 0;
   int start_down = 0, select_down = 0;
   spring bg[3], info_y, info_a, plat_x;
   label_t l_title = { 0 }, l_sub = { 0 }, l_count = { 0 }, l_brand = { 0 }, l_start = { 0 };
   float fade = 1.0f; /* 1 = black, fades in at start */

   for (i = 1; i < argc; i++)
   {
      if (!strcmp(argv[i], "--root") && i + 1 < argc) snprintf(g_root, sizeof(g_root), "%s", argv[++i]);
      else if (!strcmp(argv[i], "--log") && i + 1 < argc) log_path = argv[++i];
      else if (!strcmp(argv[i], "--home") && i + 1 < argc) snprintf(g_home, sizeof(g_home), "%s", argv[++i]);
   }
   snprintf(g_core_dir, sizeof(g_core_dir), "%s/cores", g_home);
   snprintf(g_state, sizeof(g_state), "%s/state.txt", g_home);
   snprintf(font_b, sizeof(font_b), "%s/fonts/bold.ttf", g_home);
   snprintf(font_r, sizeof(font_r), "%s/fonts/regular.ttf", g_home);
   snprintf(padb, sizeof(padb), "%s/gamecontrollerdb.txt", g_home);
   if (log_path) g_log = fopen(log_path, "w");
   logf_("lumen start");

   if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK) != 0)
   { logf_("SDL_Init: %s", SDL_GetError()); return 1; }
   logf_("controller mappings: %d from %s", SDL_GameControllerAddMappingsFromFile(padb), padb);
   SDL_JoystickEventState(SDL_ENABLE);
   SDL_GameControllerEventState(SDL_ENABLE);
   logf_("joysticks at start: %d", SDL_NumJoysticks());
   for (i = 0; i < SDL_NumJoysticks(); i++) in_open(i);
   IMG_Init(IMG_INIT_PNG | IMG_INIT_JPG);
   TTF_Init();

   SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
   SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
   SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
   SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
   SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 16);
   SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
   SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 4);
   win = SDL_CreateWindow("lumen", 0, 0, W, H, SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN_DESKTOP);
   ctx = win ? SDL_GL_CreateContext(win) : NULL;
   if (!ctx)
   {
      logf_("MSAA 4x unavailable (%s), retrying without", SDL_GetError());
      if (win) SDL_DestroyWindow(win);
      SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 0);
      SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 0);
      win = SDL_CreateWindow("lumen", 0, 0, W, H, SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN_DESKTOP);
      if (!win) { logf_("CreateWindow: %s", SDL_GetError()); return 1; }
      ctx = SDL_GL_CreateContext(win);
      if (!ctx) { logf_("GL context: %s", SDL_GetError()); return 1; }
   }
   {
      GLint samples = 0;
      glGetIntegerv(GL_SAMPLES, &samples);
      logf_("MSAA samples: %d", samples);
   }
   SDL_GL_SetSwapInterval(1);
   SDL_ShowCursor(SDL_DISABLE);
   logf_("GL %s | %s", glGetString(GL_VERSION), glGetString(GL_RENDERER));
   gl_setup();
   bg_mx = SDL_CreateMutex();
   bg_cv = SDL_CreateCond();
   SDL_CreateThread(bg_worker, "bg", NULL);
   cv_mx = SDL_CreateMutex();
   cv_cv = SDL_CreateCond();
   SDL_CreateThread(cover_worker, "covers", NULL);

   g_font_title = open_font(fonts_bold, 34);
   g_font_sub   = open_font(fonts_reg, 20);
   g_font_small = open_font(fonts_reg, 16);
   g_font_cover = open_font(fonts_bold, 22);

   scan_psx();
   build_platform_index();
   logf_("games: %d, platforms: %d, scan %.0f ms", g_ngames, g_nplat, (now_s() - t0) * 1000);
   for (i = 0; i < g_nplat; i++)
   {
      char s[64];
      snprintf(s, sizeof(s), "%s", g_plat[i].name);
      g_plat[i].label = text_tex(g_font_small, s, &g_plat[i].label_w, &g_plat[i].label_h);
   }

   /* resume last selection */
   {
      FILE *f = fopen(g_state, "r");
      if (f) { if (fscanf(f, "%d %d", &sel_plat, &sel) != 2) sel_plat = sel = 0; fclose(f); }
      if (sel_plat < 0 || sel_plat >= g_nplat) sel_plat = 0;
      if (g_nplat && (sel < 0 || sel >= g_plat[sel_plat].count)) sel = 0;
   }

   g_proj = m4_perspective(38.0f * (float)M_PI / 180.0f, (float)W / H, 0.1f, 50.0f);
   g_view = m4_translate(0, 0.22f, -4.7f);
   g_vp = m4_mul(g_proj, g_view);
   g_ortho = m4_ortho(0, W, H, 0);

   memset(bg, 0, sizeof(bg));
   bg[0].x = 0.05f; bg[1].x = 0.05f; bg[2].x = 0.08f;
   memset(&info_y, 0, sizeof(info_y)); memset(&info_a, 0, sizeof(info_a)); memset(&plat_x, 0, sizeof(plat_x));
   label_set(&l_brand, g_font_small, "lumen");
   hints_load(g_home, g_font_small);
   label_set(&l_start, g_font_sub, "Iniciando\xE2\x80\xA6");

   /* first frame positions: start off to the right so they fly in */
   for (i = 0; i < g_ngames; i++)
   {
      game_t *g = &g_games[i];
      g->x.x = 6.0f + (i % 8) * 0.6f; g->z.x = -3.0f; g->ry.x = -1.2f; g->sc.x = 1.0f; g->al.x = 0.0f;
   }

   last = fps_t = now_s();
   logf_("startup %.0f ms, rss %ld kB", (last - t0) * 1000, rss_kb());

   while (running)
   {
      SDL_Event e; double t = now_s(); float dt = (float)(t - last);
      platform_t *pl = g_nplat ? &g_plat[sel_plat] : NULL;
      int n = pl ? pl->count : 0, move = 0, plat_move = 0, play = 0;
      if (dt > 0.05f) dt = 0.05f;
      last = t;

      memset(in_pressed, 0, sizeof(in_pressed));
      while (SDL_PollEvent(&e))
      {
         if (e.type == SDL_QUIT) running = 0;
         else in_event(&e);
      }
      /* logical actions -> UI */
      if (in_down[IN_SELECT] && in_down[IN_START]) running = 0;
      else
      {
         if (in_pressed[IN_PLAY] || (in_pressed[IN_START] && !in_down[IN_SELECT])) play = 1;
         if (in_pressed[IN_L1] || in_pressed[IN_UP]) plat_move = -1;
         if (in_pressed[IN_R1] || in_pressed[IN_DOWN]) plat_move = 1;
         if (in_pressed[IN_LEFT]) { move = -1; held_next = t + 0.32; }
         if (in_pressed[IN_RIGHT]) { move = 1; held_next = t + 0.32; }
      }
      held_dir = in_down[IN_LEFT] ? -1 : (in_down[IN_RIGHT] ? 1 : 0);
      (void)start_down; (void)select_down; (void)pad;
      if (play_t >= 0) { move = plat_move = play = 0; }
      if (!move && held_dir && t >= held_next) { move = held_dir; held_next = t + 0.085; }

      if (n && move) { sel = (sel + move + n) % n; info_y.x = 18.0f; info_a.x = 0.0f; sel_t = t; }
      if (g_nplat > 1 && plat_move)
      {
         int np = (sel_plat + plat_move + g_nplat) % g_nplat, k;
         /* new platform boxes fly in from the side of travel */
         for (k = 0; k < g_plat[np].count; k++)
         {
            game_t *g = &g_games[g_plat[np].first + k];
            g->x.x = plat_move * (5.0f + k * 0.4f); g->x.v = 0; g->z.x = -2.5f; g->ry.x = -plat_move * 1.2f; g->al.x = 0;
         }
         sel_plat = np; sel = 0; pl = &g_plat[sel_plat]; n = pl->count; sel_t = t;
         info_y.x = 18.0f; info_a.x = 0.0f; plat_x.v = plat_move * 900.0f;
      }
      if (n && play && play_t < 0) { play_t = t; logf_("play: %s", g_games[pl->first + sel].content); }

      /* background: ask the worker once the selection has settled */
      if (n && t - sel_t > 0.25)
      {
         const char *want = g_games[pl->first + sel].bg;
         if (strcmp(want, bg_shown))
         {
            snprintf(bg_shown, sizeof(bg_shown), "%s", want);
            if (want[0]) bg_request(want);
            else if (bg_next.tex) { if (bg_cur.tex) glDeleteTextures(1, &bg_cur.tex); bg_cur = bg_next; bg_next.tex = 0; bg_mix = 0.0f; }
         }
      }
      bg_poll_upload();
      bg_mix = bg_mix + dt * 1.4f > 1.0f ? 1.0f : bg_mix + dt * 1.4f;

      /* cover streaming: decoded on the worker, nearest to the selection first */
      {
         int done = -1, k;
         SDL_Surface *cs = NULL; float avg[3];
         SDL_LockMutex(cv_mx);
         if (cv_done >= 0) { done = cv_done; cs = cv_surf; memcpy(avg, cv_avg, sizeof(avg)); cv_done = -1; cv_surf = NULL; cv_busy = 0; }
         SDL_UnlockMutex(cv_mx);
         if (done >= 0)
         {
            game_finish_cover(&g_games[done], cs, avg);
            if (cs) SDL_FreeSurface(cs);
         }
         if (n)
         {
            int placed = 0;
            for (k = 0; k <= TEX_WINDOW && !placed; k++)
            {
               int cand[2], c;
               cand[0] = (sel + k) % n; cand[1] = (sel - k + n) % n;
               for (c = 0; c < 2 && !placed; c++)
               {
                  int gi = pl->first + cand[c];
                  game_t *g = &g_games[gi];
                  if (g->tex_state) continue;
                  if (g->has_cover) { if (!cv_busy) { g->tex_state = 2; cover_request(gi, g->cover); } placed = 1; }
                  else { game_placeholder(g); placed = 1; }
               }
            }
            for (k = 0; k < g_ngames; k++)
            {
               game_t *g = &g_games[k];
               int d = 9999;
               if (g->platform == g_games[pl->first].platform)
               {
                  int o = abs((k - pl->first) - sel);
                  d = o < n - o ? o : n - o;
               }
               if ((g->tex_state == 1 || g->tex_state == -1) && d > TEX_WINDOW + 3) game_free_tex(g);
            }
            /* back card only for the settled (or launching) selection */
            {
               game_t *cg = &g_games[pl->first + sel];
               if ((cg->tex_state == 1 || cg->tex_state == -1) && !cg->back_tex && (t - sel_t > 0.2 || play_t >= 0))
                  cg->back_tex = card_tex(cg, 1);
            }
         }
      }

      /* animation targets */
      {
         float flip = 0.0f, zoom = 0.0f;
         if (play_t >= 0)
         {
            float pt = (float)(t - play_t);
            flip = clampf(pt / 0.75f, 0, 1); flip = flip * flip * (3 - 2 * flip);
            zoom = clampf((pt - 0.35f) / 0.8f, 0, 1); zoom = zoom * zoom * (3 - 2 * zoom);
            fade = clampf((pt - 1.15f) / 0.45f, 0, 1);
            if (pt > 1.75f) { exit_code = 10; running = 0; }
         }
         else fade = fade > 0 ? fade - dt * 1.6f : 0;

         for (i = 0; i < n; i++)
         {
            game_t *g = &g_games[pl->first + i];
            int o = i - sel, a;
            float tx, tz, tr, ts, ta;
            if (o > n / 2) o -= n;
            if (o < -n / 2) o += n;
            a = abs(o);
            tx = o == 0 ? 0.0f : (o > 0 ? 1 : -1) * (1.08f + (a - 1) * 0.50f);
            tz = o == 0 ? 0.55f + zoom * 1.4f : -0.35f - a * 0.22f;
            tr = o == 0 ? 0.0f : -(o > 0 ? 1 : -1) * 1.0f;
            ts = o == 0 ? 1.10f : 0.92f;
            ta = a > VISIBLE ? 0.0f : 1.0f - a * 0.14f;
            spring_step(&g->x, tx, 140, 15, dt);
            spring_step(&g->z, tz, 140, 15, dt);
            spring_step(&g->ry, tr, 120, 14, dt);
            spring_step(&g->sc, ts, 160, 16, dt);
            spring_step(&g->al, ta, 90, 14, dt);
         }

         /* ambient colour from the selected cover */
         if (n)
         {
            game_t *cg = &g_games[pl->first + sel];
            float base[3] = { 0.050f, 0.050f, 0.078f };
            for (i = 0; i < 3; i++) spring_step(&bg[i], base[i] + cg->col[i] * 0.16f, 6, 5, dt);
         }
         spring_step(&info_y, 0, 170, 17, dt);
         spring_step(&info_a, 1, 60, 12, dt);
         spring_step(&plat_x, 0, 120, 16, dt);

         /* ---- render ---- */
         glViewport(0, 0, W, H);
         glClearColor(bg[0].x, bg[1].x, bg[2].x, 1);
         glClearDepthf(1.0f);
         glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
         glEnable(GL_BLEND);
         glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

         glUseProgram(g_prog_2d);
         glUniform1i(u2_tex, 0);
         {
            float ba = BG_OPACITY * (1.0f - fade), e = bg_mix * bg_mix * (3 - 2 * bg_mix);
            bg_draw(&bg_cur, ba * (1.0f - e), t);
            bg_draw(&bg_next, ba * e, t);
         }
         {
            const float vig[4] = { 0, 0, 0, 0.55f };
            float spot[4] = { 0.55f + bg[0].x, 0.55f + bg[1].x, 0.62f + bg[2].x, 0.10f * (1.0f - fade) };
            (void)vig;
            draw_2d(0, 0, 0, W, H, spot, 4);
         }

         glUseProgram(g_prog_box);
         glUniform1i(u_tex, 0);
         {
            const float light[3] = { -0.5f, 0.7f, 1.0f }, cam[3] = { 0.0f, -0.22f, 4.7f };
            glUniform3fv(u_light, 1, light);
            glUniform3fv(u_cam, 1, cam);
         }
         /* reflections first (no depth), then cases back-to-front with depth */
         glDisable(GL_DEPTH_TEST);
         for (i = VISIBLE; i >= 0 && n; i--)
         {
            int s;
            for (s = -1; s <= 1; s += 2)
            {
               int idx, o = i * s;
               game_t *g;
               mat4 base;
               if (i == 0 && s == 1) continue;
               idx = ((sel + o) % n + n) % n;
               g = &g_games[pl->first + idx];
               if (g->al.x < 0.01f || !g->tex) continue;
               base = m4_mul(m4_translate(g->x.x, 0, g->z.x), m4_mul(m4_rot_y(g->ry.x), m4_scale(g->sc.x, g->sc.x, g->sc.x)));
               draw_reflection(g, base, g->al.x * (1.0f - fade));
            }
         }
         glEnable(GL_DEPTH_TEST);
         glDepthFunc(GL_LEQUAL);
         for (i = VISIBLE; i >= 0 && n; i--)
         {
            int s;
            for (s = -1; s <= 1; s += 2)
            {
               int idx, o = i * s;
               game_t *g;
               mat4 base;
               float bob = 0.0f, sway = 0.0f, fl = 0.0f;
               if (i == 0 && s == 1) continue;
               idx = ((sel + o) % n + n) % n;
               g = &g_games[pl->first + idx];
               if (g->al.x < 0.01f || !g->tex) continue;
               if (o == 0)
               {
                  bob = sinf((float)t * 1.9f) * 0.035f;
                  sway = -0.17f + sinf((float)t * 1.9f + 0.6f) * 0.06f;
                  if (play_t >= 0) { fl = flip; sway *= 1.0f - flip; }
               }
               base = m4_mul(m4_translate(g->x.x, bob, g->z.x),
                      m4_mul(m4_rot_y(g->ry.x + sway), m4_scale(g->sc.x, g->sc.x, g->sc.x)));
               draw_case(g, base, g->al.x, fl);
            }
         }
         glDisable(GL_DEPTH_TEST);

         /* ---- 2D overlay ---- */
         glUseProgram(g_prog_2d);
         {
            const float txt[4] = { 0.93f, 0.92f, 0.97f, 1 - fade };
            const float dim[4] = { 0.55f, 0.54f, 0.64f, 1 - fade };
            const float pill_on[4] = { 0.93f, 0.93f, 0.99f, 1 - fade };
            const float pill_off[4] = { 0.25f, 0.25f, 0.32f, 0.6f * (1 - fade) };
            const float txt_dark[4] = { 0.15f, 0.13f, 0.36f, 1 - fade };
            float px, total = 0;
            char s[256];
            game_t *cg = n ? &g_games[pl->first + sel] : NULL;

            draw_2d(l_brand.tex, 36, 26, l_brand.w, l_brand.h, txt, 0);
            if (pl) { snprintf(s, sizeof(s), "%d juegos  \xC2\xB7  %s", pl->count, pl->name); label_set(&l_count, g_font_small, s); }
            draw_2d(l_count.tex, W - 36 - l_count.w, 26, l_count.w, l_count.h, dim, 0);

            for (i = 0; i < g_nplat; i++) total += g_plat[i].label_w + 36 + 10;
            px = (W - total) * 0.5f + plat_x.x * 0.02f;
            for (i = 0; i < g_nplat; i++)
            {
               float pw = g_plat[i].label_w + 36, ph = 32;
               draw_2d(0, px, 64, pw, ph, i == sel_plat ? pill_on : pill_off, 1);
               draw_2d(g_plat[i].label, px + 18, 64 + (ph - g_plat[i].label_h) * 0.5f,
                       g_plat[i].label_w, g_plat[i].label_h, i == sel_plat ? txt_dark : txt, 0);
               px += pw + 10;
            }

            if (cg)
            {
               float a = info_a.x * (1 - fade);
               float c1[4] = { 0.93f, 0.92f, 0.97f, a }, c2[4] = { 0.55f, 0.54f, 0.64f, a };
               label_set(&l_title, g_font_title, cg->title);
               snprintf(s, sizeof(s), "%s%s%s", cg->year, cg->year[0] && cg->publisher[0] ? "  \xC2\xB7  " : "", cg->publisher);
               label_set(&l_sub, g_font_sub, s);
               draw_2d(l_title.tex, (W - l_title.w) * 0.5f, 588 + info_y.x, l_title.w, l_title.h, c1, 0);
               draw_2d(l_sub.tex, (W - l_sub.w) * 0.5f, 630 + info_y.x * 0.7f, l_sub.w, l_sub.h, c2, 0);
            }
            else
            {
               label_set(&l_title, g_font_title, "Agrega juegos a /games");
               draw_2d(l_title.tex, (W - l_title.w) * 0.5f, 330, l_title.w, l_title.h, txt, 0);
            }
            {
               const float hc[4] = { 0.43f, 0.42f, 0.52f, 1 - fade };
               (void)hc;
               hints_draw(686, 1 - fade);
            }
            if (fade > 0.001f)
            {
               const float black[4] = { 0, 0, 0, fade };
               draw_2d(0, 0, 0, W, H, black, 0);
               if (play_t >= 0)
               {
                  const float st[4] = { 0.93f, 0.92f, 0.97f, fade };
                  draw_2d(l_start.tex, (W - l_start.w) * 0.5f, (H - l_start.h) * 0.5f, l_start.w, l_start.h, st, 0);
               }
            }
         }
      }

      SDL_GL_SwapWindow(win);
      frames++;
      if (t - fps_t >= 5.0)
      {
         logf_("fps %.1f, rss %ld kB", frames / (t - fps_t), rss_kb());
         frames = 0; fps_t = t;
      }
      (void)info_t;
   }

   /* persist selection; hand the launch request to the launcher script */
   {
      FILE *f = fopen(g_state, "w");
      if (f) { fprintf(f, "%d %d\n", sel_plat, sel); fclose(f); }
   }
   if (exit_code == 10 && g_nplat)
   {
      game_t *g = &g_games[g_plat[sel_plat].first + sel];
      FILE *f = fopen("/tmp/lumen-launch", "w");
      if (f) { fprintf(f, "%s/%s\n%s\n", g_core_dir, g_plat[sel_plat].core, g->content); fclose(f); }
      logf_("launch request: %s", g->content);
   }
   SDL_LockMutex(bg_mx); bg_quit = 1; SDL_CondSignal(bg_cv); SDL_UnlockMutex(bg_mx);
   SDL_LockMutex(cv_mx); cv_quit = 1; SDL_CondSignal(cv_cv); SDL_UnlockMutex(cv_mx);
   logf_("exit %d", exit_code);
   SDL_GL_DeleteContext(ctx);
   SDL_DestroyWindow(win);
   TTF_Quit(); IMG_Quit(); SDL_Quit();
   return exit_code;
}
