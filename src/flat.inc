/* Lumen — the flat launch (lumen-design, guidelines/06 "Launch: flat"). No 3D: a field of
 * the game's colour wipes over the dashboard, the system's machine is a flat illustration
 * that drops in and floats over a line of shadow, its medium goes into it (through its lid
 * or door where it has one), then its screen lights up and grows into the frame, or its
 * lamp lights and it hops. Every layer is one textured quad.
 *
 * The illustrations are exported by lumen-design (tools/export-launch.sh) into
 * <home>/assets/launch/<key>/: launch.ini and up to six layers, each a 720x540 picture of
 * the same 480x360 canvas (x 0..480, y -60..300, logical px), placed at (400, 150):
 *   body    the machine             under   a lid while it stands open (behind the medium)
 *   medium  what goes in            over    the lid shut / the door (over the medium)
 *   lit     lamps that come on      tick    the three strokes of the click
 * Where a layer shows the game's art it has a hole, and art.png is the hole's shape. At
 * Play the cover, reduced to five flat tones of its own palette (a photographic cover
 * clashes with a flat drawing), is painted through that shape into the layer, once: the
 * launch itself draws no cover. Layers and cover are decoded on worker threads once the
 * selection settles.
 *
 * Included by main.c (uses draw_2d, draw_shape, gl_scissor, surface_to_tex, label_t). */

#include "image.h"

enum { FL_BODY, FL_UNDER, FL_MEDIUM, FL_OVER, FL_LIT, FL_TICK, FL_N };
enum { FM_NONE, FM_BEHIND, FM_OVER, FM_CLIP, FM_FRONT, FM_COIN };
static const char *const FL_NAME[FL_N] = { "body", "under", "medium", "over", "lit", "tick" };
#define FLAT_X0 400.0f      /* where the canvas's (0, 0) sits on the 1280x720 layout */
#define FLAT_Y0 150.0f
#define FLAT_CY (-60.0f)    /* the canvas: x 0..480, y -60..300 */
#define FLAT_CW 480.0f
#define FLAT_CH 360.0f
#define FLAT_SEAT 1.32f     /* the medium seats: the click */
#define FLAT_FADE 2.28f     /* the fade to black starts */
#define FLAT_END  2.70f

typedef struct { float sx, sy, tx, ty; } fxf;   /* p' = s * p + t (no rotation anywhere) */
static fxf fx_id(void) { fxf r = { 1, 1, 0, 0 }; return r; }
static fxf fx_mul(fxf a, fxf b) { fxf r = { a.sx * b.sx, a.sy * b.sy, a.sx * b.tx + a.tx, a.sy * b.ty + a.ty }; return r; }   /* b first */
static fxf fx_move(float x, float y) { fxf r = { 1, 1, x, y }; return r; }
static fxf fx_about(float ox, float oy, float sx, float sy) { fxf r = { sx, sy, ox - sx * ox, oy - sy * oy }; return r; }

typedef struct {
   char key[32];
   int ok;                       /* the layers are on the GPU */
   GLuint tex[FL_N];
   int medium, door;             /* FM_*; door: 0 none, 1 lid (hinged at the back), 2 flap */
   int has_clip_y, has_screen, art_layer;   /* art_layer: -1 none, FL_MEDIUM or FL_BODY */
   float clip_y, clip_rect[4], origin[2], door_o[2], tick_o[2], screen[4], art[4], shadow;
   SDL_Surface *base, *mask;     /* CPU copies: the layer with the hole, and the hole's shape */
   /* the game's art, painted */
   char art_path[512];
   SDL_Surface *art_s;           /* NULL: no cover file (five bands of pal instead) */
   float pal[5][3];              /* its five tones, dark to light */
   char painted[512]; int painted_ok;   /* the cover now in the layer's hole */
   label_t l_kicker, l_title;
} flat_t;
static flat_t g_flat;

/* ---- workers: one job for a system's layers, one for a game's cover ------------------- */
typedef struct { char key[32]; SDL_Surface *s[FL_N], *mask; SDL_atomic_t done; } flat_ljob_t;
typedef struct { char path[512]; SDL_Surface *s; float pal[5][3]; SDL_atomic_t done; } flat_ajob_t;
static flat_ljob_t *g_flat_ljob;
static flat_ajob_t *g_flat_ajob;

static SDL_Surface *flat_png(const char *path)
{  /* ABGR8888, premultiplied: a straight-alpha edge filters to a dark fringe when the
    * machine grows into the frame */
   SDL_Surface *f = IMG_Load(path), *s;
   int x, y;
   if (!f) return NULL;
   s = SDL_ConvertSurfaceFormat(f, SDL_PIXELFORMAT_ABGR8888, 0);
   SDL_FreeSurface(f);
   if (!s) return NULL;
   for (y = 0; y < s->h; y++)
   {
      unsigned char *p = (unsigned char *)s->pixels + y * s->pitch;
      for (x = 0; x < s->w; x++, p += 4)
         if (p[3] != 255) { p[0] = (unsigned char)(p[0] * p[3] / 255); p[1] = (unsigned char)(p[1] * p[3] / 255); p[2] = (unsigned char)(p[2] * p[3] / 255); }
   }
   return s;
}

static void flat_layers_decode(flat_ljob_t *j)
{
   int i;
   for (i = 0; i < FL_N; i++)
   {
      char path[512];
      snprintf(path, sizeof(path), "%s/assets/launch/%s/%s.png", g_home, j->key, FL_NAME[i]);
      j->s[i] = file_exists(path) ? flat_png(path) : NULL;
   }
   {
      char path[512];
      snprintf(path, sizeof(path), "%s/assets/launch/%s/art.png", g_home, j->key);
      j->mask = file_exists(path) ? flat_png(path) : NULL;
   }
}
static int flat_layers_thread(void *p) { flat_ljob_t *j = p; flat_layers_decode(j); SDL_AtomicSet(&j->done, 1); return 0; }

/* The cover as a flat drawing: scaled down, smoothed, then every pixel takes the tone of
 * its luminance band: five bands of equal population, each the mean colour of its pixels. */
static void flat_paint(SDL_Surface *s, float pal[5][3])
{
   const int w = s->w, h = s->h, n = w * h;
   unsigned char *px = s->pixels, *tmp = malloc((size_t)n * 4);
   long hist[256] = { 0 }, acc = 0;
   double sum[5][3] = { { 0 } }; long cnt[5] = { 0 };
   int cut[4], x, y, i, k = 0, pass;
   if (!tmp) return;
   for (pass = 0; pass < 2; pass++)
   {  /* two 3x3 box blurs: the grain and the dither of a photograph must not survive */
      memcpy(tmp, px, (size_t)n * 4);
      for (y = 0; y < h; y++)
         for (x = 0; x < w; x++)
         {
            int r = 0, g = 0, b = 0, c = 0, dx, dy;
            for (dy = -1; dy <= 1; dy++)
               for (dx = -1; dx <= 1; dx++)
               {
                  const int xx = x + dx, yy = y + dy;
                  const unsigned char *q;
                  if (xx < 0 || yy < 0 || xx >= w || yy >= h) continue;
                  q = tmp + (yy * s->pitch) + xx * 4; r += q[0]; g += q[1]; b += q[2]; c++;
               }
            px[y * s->pitch + x * 4] = (unsigned char)(r / c); px[y * s->pitch + x * 4 + 1] = (unsigned char)(g / c); px[y * s->pitch + x * 4 + 2] = (unsigned char)(b / c);
         }
   }
   free(tmp);
#define FLAT_LUM(p) (((p)[0] * 54 + (p)[1] * 183 + (p)[2] * 19) >> 8)
   for (y = 0; y < h; y++) for (x = 0; x < w; x++) hist[FLAT_LUM(px + y * s->pitch + x * 4)]++;
   for (i = 0; i < 256 && k < 4; i++) { acc += hist[i]; while (k < 4 && acc >= (long)n * (k + 1) / 5) cut[k++] = i; }
   while (k < 4) cut[k++] = 255;
   for (y = 0; y < h; y++)
      for (x = 0; x < w; x++)
      {
         const unsigned char *p = px + y * s->pitch + x * 4; const int l = FLAT_LUM(p);
         int b = 0; while (b < 4 && l > cut[b]) b++;
         sum[b][0] += p[0]; sum[b][1] += p[1]; sum[b][2] += p[2]; cnt[b]++;
      }
   for (i = 0; i < 5; i++)
   {
      int j = i;   /* an empty band (a cover of few tones) borrows its neighbour's */
      while (j > 0 && !cnt[j]) j--;
      while (j < 4 && !cnt[j]) j++;
      for (k = 0; k < 3; k++) pal[i][k] = cnt[j] ? (float)(sum[j][k] / cnt[j] / 255.0) : 0.3f;
   }
   for (y = 0; y < h; y++)
      for (x = 0; x < w; x++)
      {
         unsigned char *p = px + y * s->pitch + x * 4; const int l = FLAT_LUM(p);
         int b = 0; while (b < 4 && l > cut[b]) b++;
         p[0] = (unsigned char)(pal[b][0] * 255.0f + 0.5f); p[1] = (unsigned char)(pal[b][1] * 255.0f + 0.5f); p[2] = (unsigned char)(pal[b][2] * 255.0f + 0.5f); p[3] = 255;
      }
#undef FLAT_LUM
}

static void flat_art_decode(flat_ajob_t *j)
{
   j->s = image_load_scaled(j->path, 192, 0, NULL, NULL);
   if (j->s) flat_paint(j->s, j->pal);
}
static int flat_art_thread(void *p) { flat_ajob_t *j = p; flat_art_decode(j); SDL_AtomicSet(&j->done, 1); return 0; }

/* ---- the declaration -------------------------------------------------------------------- */
static int flat_floats(const char *v, float *out, int n)
{
   int i = 0;
   while (i < n && *v) { char *e; out[i] = strtof(v, &e); if (e == v) break; i++; v = e; while (*v == ',' || *v == ' ') v++; }
   return i;
}

static int flat_read_ini(flat_t *f, const char *key)
{
   char path[512], line[320];
   FILE *fp;
   snprintf(path, sizeof(path), "%s/assets/launch/%s/launch.ini", g_home, key);
   if (!(fp = fopen(path, "r"))) return 0;
   f->medium = FM_NONE; f->door = 0; f->has_clip_y = f->has_screen = 0; f->art_layer = -1; f->shadow = 300;
   while (fgets(line, sizeof(line), fp))
   {
      char k[32], v[256];
      if (sscanf(line, " %31[a-z_] = %255[^\r\n]", k, v) != 2) continue;
      if (!strcmp(k, "medium"))
         f->medium = !strcmp(v, "behind") ? FM_BEHIND : !strcmp(v, "over") ? FM_OVER : !strcmp(v, "clip") ? FM_CLIP :
                     !strcmp(v, "front") ? FM_FRONT : !strcmp(v, "coin") ? FM_COIN : FM_NONE;
      else if (!strcmp(k, "clip_y")) { f->clip_y = (float)atof(v); f->has_clip_y = 1; }
      else if (!strcmp(k, "clip_rect")) flat_floats(v, f->clip_rect, 4);
      else if (!strcmp(k, "origin")) flat_floats(v, f->origin, 2);
      else if (!strcmp(k, "door")) f->door = !strcmp(v, "lid") ? 1 : !strcmp(v, "flap") ? 2 : 0;
      else if (!strcmp(k, "door_origin")) flat_floats(v, f->door_o, 2);
      else if (!strcmp(k, "tick_origin")) flat_floats(v, f->tick_o, 2);
      else if (!strcmp(k, "screen")) f->has_screen = flat_floats(v, f->screen, 4) == 4;
      else if (!strcmp(k, "shadow")) f->shadow = (float)atof(v);
      else if (!strcmp(k, "art"))
      {
         f->art_layer = !strncmp(v, "medium,", 7) ? FL_MEDIUM : !strncmp(v, "body,", 5) ? FL_BODY : -1;
         if (f->art_layer >= 0 && flat_floats(strchr(v, ',') + 1, f->art, 4) != 4) f->art_layer = -1;
      }
   }
   fclose(fp);
   return 1;
}

static void flat_layers_upload(flat_t *f, flat_ljob_t *j)
{
   int i;
   if (f->base) { SDL_FreeSurface(f->base); f->base = NULL; }
   if (f->mask) { SDL_FreeSurface(f->mask); f->mask = NULL; }
   f->painted[0] = 0; f->painted_ok = 0;
   for (i = 0; i < FL_N; i++)
   {
      if (f->tex[i]) { glDeleteTextures(1, &f->tex[i]); f->tex[i] = 0; }
      if (!j->s[i]) continue;
      f->tex[i] = surface_to_tex(j->s[i], 1, NULL);
      if (i == f->art_layer && j->mask && j->mask->w == j->s[i]->w && j->mask->h == j->s[i]->h) { f->base = j->s[i]; f->mask = j->mask; j->mask = NULL; }
      else SDL_FreeSurface(j->s[i]);
      j->s[i] = NULL;
   }
   if (j->mask) { SDL_FreeSurface(j->mask); j->mask = NULL; }
   f->ok = f->tex[FL_BODY] != 0;
   if (!f->ok) logf_("flat launch: no body layer for '%s'", f->key);
}

static void flat_art_take(flat_t *f, flat_ajob_t *j)
{
   if (f->art_s) { SDL_FreeSurface(f->art_s); f->art_s = NULL; }
   if (j->s) { f->art_s = j->s; j->s = NULL; memcpy(f->pal, j->pal, sizeof(f->pal)); }
}

/* The game's art into the hole of its layer: cover-fit in the art's box, through the
 * hole's shape (art.png), under whatever the layer draws over it. One upload, at Play. */
static void flat_paint_layer(flat_t *f)
{
   const SDL_Surface *b = f->base, *m = f->mask, *a = f->art_s;
   const float k = b ? b->w / FLAT_CW : 1.0f;
   SDL_Surface *o;
   int x0, y0, x1, y1, x, y;
   float aw, ah, ox, oy;
   if (!b || !m || f->art_layer < 0) return;
   /* ABGR8888 by its masks: the console's SDL has no SDL_CreateRGBSurfaceWithFormat */
   if (!(o = SDL_CreateRGBSurface(0, b->w, b->h, 32, 0x000000ff, 0x0000ff00, 0x00ff0000, 0xff000000))) return;
   for (y = 0; y < b->h; y++) memcpy((unsigned char *)o->pixels + y * o->pitch, (const unsigned char *)b->pixels + y * b->pitch, (size_t)b->w * 4);
   x0 = (int)floorf(f->art[0] * k); y0 = (int)floorf((f->art[1] - FLAT_CY) * k);
   x1 = (int)ceilf((f->art[0] + f->art[2]) * k); y1 = (int)ceilf((f->art[1] + f->art[3] - FLAT_CY) * k);
   if (x0 < 0) x0 = 0;
   if (y0 < 0) y0 = 0;
   if (x1 > b->w) x1 = b->w;
   if (y1 > b->h) y1 = b->h;
   /* cover-fit: the art fills the box, its overflow centred */
   aw = (float)(x1 - x0); ah = (float)(y1 - y0);
   if (a) { if (aw / ah > (float)a->w / a->h) ah = aw * a->h / a->w; else aw = ah * a->w / a->h; }
   ox = x0 + ((x1 - x0) - aw) * 0.5f; oy = y0 + ((y1 - y0) - ah) * 0.5f;
   for (y = y0; y < y1; y++)
      for (x = x0; x < x1; x++)
      {
         const unsigned char mk = ((const unsigned char *)m->pixels + y * m->pitch)[x * 4 + 3];
         unsigned char *p = (unsigned char *)o->pixels + y * o->pitch + x * 4;
         float c[3], w;
         int q;
         if (!mk || p[3] == 255) continue;
         if (a)
         {
            int ax = (int)((x + 0.5f - ox) / aw * a->w), ay = (int)((y + 0.5f - oy) / ah * a->h);
            const unsigned char *s;
            ax = ax < 0 ? 0 : (ax >= a->w ? a->w - 1 : ax); ay = ay < 0 ? 0 : (ay >= a->h ? a->h - 1 : ay);
            s = (const unsigned char *)a->pixels + ay * a->pitch + ax * 4;
            c[0] = s[0]; c[1] = s[1]; c[2] = s[2];
         }
         else
         {  /* no cover: the five tones in bands, the lightest at the top */
            int band = 4 - (int)((y - y0) * 5 / (y1 - y0 > 0 ? y1 - y0 : 1));
            band = band < 0 ? 0 : (band > 4 ? 4 : band);
            c[0] = f->pal[band][0] * 255.0f; c[1] = f->pal[band][1] * 255.0f; c[2] = f->pal[band][2] * 255.0f;
         }
         w = (1.0f - p[3] / 255.0f) * (mk / 255.0f);   /* premultiplied "under" */
         for (q = 0; q < 3; q++) p[q] = (unsigned char)(p[q] + c[q] * w + 0.5f);
         p[3] = (unsigned char)(p[3] + 255.0f * w + 0.5f);
      }
   if (f->tex[f->art_layer]) glDeleteTextures(1, &f->tex[f->art_layer]);
   f->tex[f->art_layer] = surface_to_tex(o, 1, NULL);
   SDL_FreeSurface(o);
}

/* results of the workers: called once per frame */
static void flat_poll(void)
{
   if (g_flat_ljob && SDL_AtomicGet(&g_flat_ljob->done))
   {
      double ps = now_s();
      if (!strcmp(g_flat_ljob->key, g_flat.key)) flat_layers_upload(&g_flat, g_flat_ljob);
      else { int i; for (i = 0; i < FL_N; i++) if (g_flat_ljob->s[i]) SDL_FreeSurface(g_flat_ljob->s[i]); if (g_flat_ljob->mask) SDL_FreeSurface(g_flat_ljob->mask); }
      free(g_flat_ljob); g_flat_ljob = NULL;
      perf_stall("flat layers upload", ps);
   }
   if (g_flat_ajob && SDL_AtomicGet(&g_flat_ajob->done))
   {
      if (!strcmp(g_flat_ajob->path, g_flat.art_path)) flat_art_take(&g_flat, g_flat_ajob);
      else if (g_flat_ajob->s) SDL_FreeSurface(g_flat_ajob->s);
      free(g_flat_ajob); g_flat_ajob = NULL;
   }
}

/* The selection has settled on g of system pl: its illustration and its painted cover are
 * wanted. Nothing blocks here unless `now` (Play was pressed before the workers finished). */
static void flat_want(const platform_t *pl, const game_t *g, int now)
{
   flat_t *f = &g_flat;
   if (strcmp(f->key, pl->flat))
   {
      if (g_flat_ljob && !now) return;   /* one job at a time: asked again next frame */
      snprintf(f->key, sizeof(f->key), "%s", pl->flat);
      f->ok = 0;
      if (flat_read_ini(f, f->key))
      {
         flat_ljob_t *j = calloc(1, sizeof(*j));
         snprintf(j->key, sizeof(j->key), "%s", f->key);
         if (now) { flat_layers_decode(j); flat_layers_upload(f, j); free(j); }
         else if (!g_flat_ljob) { g_flat_ljob = j; SDL_DetachThread(SDL_CreateThread(flat_layers_thread, "flat-layers", j)); }
         else free(j);
      }
      else logf_("flat launch: no <home>/assets/launch/%s/launch.ini", f->key);
   }
   else if (now && !f->ok && g_flat_ljob)
   {  /* Play while the layers are still being decoded: wait for that worker */
      while (!SDL_AtomicGet(&g_flat_ljob->done)) SDL_Delay(1);
      flat_poll();
   }
   if (g && strcmp(f->art_path, g->cover))
   {
      if (g_flat_ajob && !now) return;
      snprintf(f->art_path, sizeof(f->art_path), "%s", g->cover);
      if (f->art_s) { SDL_FreeSurface(f->art_s); f->art_s = NULL; }
      if (g->has_cover && g->cover[0])
      {
         flat_ajob_t *j = calloc(1, sizeof(*j));
         snprintf(j->path, sizeof(j->path), "%s", g->cover);
         if (now) { flat_art_decode(j); flat_art_take(f, j); free(j); }
         else if (!g_flat_ajob) { g_flat_ajob = j; SDL_DetachThread(SDL_CreateThread(flat_art_thread, "flat-art", j)); }
         else free(j);
      }
   }
   else if (now && g_flat_ajob && !strcmp(g_flat_ajob->path, f->art_path))
   {  /* Play while this cover is still being painted: wait for that worker */
      while (!SDL_AtomicGet(&g_flat_ajob->done)) SDL_Delay(1);
      flat_poll();
   }
}

/* Play: everything must be there now. 0 when the system has no illustration (the caller
 * falls back to its 3D / cover animation). */
static int flat_begin(const platform_t *pl, const game_t *g)
{
   flat_t *f = &g_flat;
   int i, k;
   char up[64];
   flat_want(pl, g, 1);
   if (!f->ok) return 0;
   if (!f->art_s)
   {  /* no cover file (a placeholder card): five tones between the card's two colours */
      for (i = 0; i < 5; i++)
         for (k = 0; k < 3; k++) f->pal[i][k] = clampf(lerpf(g->col[k] * 0.45f, g->acc[k] * 0.6f + 0.4f, i / 4.0f), 0, 1);
   }
   if (!f->painted_ok || strcmp(f->painted, g->cover))
   {
      double ps = now_s();
      flat_paint_layer(f);
      snprintf(f->painted, sizeof(f->painted), "%s", g->cover); f->painted_ok = 1;
      perf_stall("flat art paint", ps);
   }
   for (i = 0; pl->name[i] && i < (int)sizeof(up) - 1; i++) up[i] = (char)toupper((unsigned char)pl->name[i]);
   up[i] = 0;
   label_set(&f->l_kicker, g_font_caps, up);
   label_set(&f->l_title, g_font_title, g->title);
   return 1;
}

/* ---- drawing ---------------------------------------------------------------------------- */
typedef struct { int n; float r[2][4]; } flat_clip_t;   /* 0: none; else the union of n rectangles (x, y, w, h) */

static float flat_ramp(float t, float a, float b) { return clampf((t - a) / (b - a), 0, 1); }
static float flat_smooth(float x) { return x * x * (3 - 2 * x); }

/* a rectangle of the canvas through a transform, normalised (a flipped lid has sy < 0) */
static void flat_rect(fxf m, float x, float y, float w, float h, float out[4])
{
   float x0 = m.sx * x + m.tx, y0 = m.sy * y + m.ty, x1 = m.sx * (x + w) + m.tx, y1 = m.sy * (y + h) + m.ty;
   out[0] = x0 < x1 ? x0 : x1; out[1] = y0 < y1 ? y0 : y1; out[2] = fabsf(x1 - x0); out[3] = fabsf(y1 - y0);
}

/* one layer: the whole canvas as a quad (premultiplied texture), alpha a */
static void flat_layer(GLuint tex, fxf m, float a, const flat_clip_t *clip)
{
   const float c[4] = { a, a, a, a };
   const float x = m.tx, y = m.sy * FLAT_CY + m.ty, w = m.sx * FLAT_CW, h = m.sy * FLAT_CH;
   int i;
   if (!tex || a <= 0.003f) return;
   if (!clip || !clip->n) { draw_2d(tex, x, y, w, h, c, 0); return; }
   glEnable(GL_SCISSOR_TEST);
   for (i = 0; i < clip->n; i++)
   {
      gl_scissor(clip->r[i][0], clip->r[i][1], clip->r[i][2], clip->r[i][3]);
      draw_2d(tex, x, y, w, h, c, 0);
   }
   glDisable(GL_SCISSOR_TEST);
}

/* The launch at pt seconds after Play. Called with g_prog_2d bound and blending on; leaves
 * both as it found them. Opaque from 0.4 s on: the caller draws nothing under it. */
static void flat_draw(float pt, const game_t *g, const platform_t *pl)
{
   flat_t *f = &g_flat;
   const float PI2 = 6.2831853f;
   fxf body, med = fx_id(), door = fx_id();
   flat_clip_t clip = { 0 };
   float field[3], mx, text_a, med_a = 0.0f, lit, door_sy = 1.0f, bob, u;
   int k, i;
   (void)pl;

   /* the field: a deep tone of the game's accent, growing as a circle from the focused tile */
   mx = g->acc[0] > g->acc[1] ? g->acc[0] : g->acc[1]; if (g->acc[2] > mx) mx = g->acc[2]; if (mx < 0.05f) mx = 0.05f;
   for (k = 0; k < 3; k++) field[k] = g->acc[k] / mx * 0.36f * 0.86f + (k == 2 ? 0.06f : k == 1 ? 0.03f : 0.025f);
   {
      const float e = flat_ramp(pt, 0.05f, 0.42f), r = 1500.0f * (1.0f - (1.0f - e) * (1.0f - e) * (1.0f - e));
      const float fc[4] = { field[0], field[1], field[2], 1.0f };
      if (e >= 1.0f) { glDisable(GL_BLEND); draw_2d(0, 0, 0, W, H, fc, 0); glEnable(GL_BLEND); }   /* opaque: no blending for the full screen */
      else if (r > 1.0f) draw_shape(196.0f - r, 318.0f - r, 2 * r, 2 * r, r, 0.0f, fc);
   }
   text_a = flat_ramp(pt, 0.40f, 0.60f);
   if (f->has_screen) text_a *= 1.0f - flat_ramp(pt, 1.98f, 2.12f);   /* the growing machine takes the whole frame */

   /* the machine's own motion: drop with a squash, float, the click, the end */
   {
      fxf drop, end = fx_id(), sq = fx_id();
      if (pt < 0.62f) { u = flat_ramp(pt, 0.30f, 0.62f); drop = fx_move(0, -620.0f * (1.0f - u * u)); }
      else if (pt < 0.70f) { u = flat_ramp(pt, 0.62f, 0.70f); drop = fx_mul(fx_move(0, 16.0f * u), fx_about(240, 150, lerpf(1, 1.05f, u), lerpf(1, 0.93f, u))); }
      else if (pt < 0.78f) { u = flat_smooth(flat_ramp(pt, 0.70f, 0.78f)); drop = fx_mul(fx_move(0, lerpf(16.0f, -10.0f, u)), fx_about(240, 150, lerpf(1.05f, 0.98f, u), lerpf(0.93f, 1.03f, u))); }
      else { u = flat_smooth(flat_ramp(pt, 0.78f, 0.88f)); drop = fx_mul(fx_move(0, lerpf(-10.0f, 0, u)), fx_about(240, 150, lerpf(0.98f, 1, u), lerpf(1.03f, 1, u))); }
      bob = pt > 0.88f ? 0.5f - 0.5f * cosf((pt - 0.88f) * PI2 / 3.0f) : 0.0f;
      if (pt > FLAT_SEAT)
      {
         if (pt < FLAT_SEAT + 0.06f) { u = flat_ramp(pt, FLAT_SEAT, FLAT_SEAT + 0.06f); sq = fx_about(240, 300, lerpf(1, 1.045f, u), lerpf(1, 0.945f, u)); }
         else if (pt < FLAT_SEAT + 0.14f) { u = flat_smooth(flat_ramp(pt, FLAT_SEAT + 0.06f, FLAT_SEAT + 0.14f)); sq = fx_about(240, 300, lerpf(1.045f, 0.99f, u), lerpf(0.945f, 1.015f, u)); }
         else { u = flat_smooth(flat_ramp(pt, FLAT_SEAT + 0.14f, FLAT_SEAT + 0.20f)); sq = fx_about(240, 300, lerpf(0.99f, 1, u), lerpf(1.015f, 1, u)); }
      }
      if (f->has_screen)
      {  /* the lit screen grows into the frame (its centre to the centre of the screen) */
         const float cx = f->screen[0] + f->screen[2] * 0.5f, cy = f->screen[1] + f->screen[3] * 0.5f;
         const float zs = 1.12f * (W / f->screen[2] > H / f->screen[3] ? W / f->screen[2] : H / f->screen[3]);
         u = flat_ramp(pt, 1.98f, 2.46f); u = u * u * u;
         end = fx_mul(fx_move((W * 0.5f - FLAT_X0 - cx) * u, (H * 0.5f - FLAT_Y0 - cy) * u), fx_about(cx, cy, lerpf(1, zs, u), lerpf(1, zs, u)));
      }
      else if (pt > 1.90f)
      {  /* no screen to enter: it hops */
         if (pt < 2.02f) { u = flat_ramp(pt, 1.90f, 2.02f); end = fx_move(0, -22.0f * (1.0f - (1.0f - u) * (1.0f - u))); }
         else if (pt < 2.14f) { u = flat_ramp(pt, 2.02f, 2.14f); end = fx_mul(fx_move(0, -22.0f * (1.0f - u * u)), fx_about(240, 300, lerpf(1, 1.03f, u), lerpf(1, 0.96f, u))); }
         else { u = flat_smooth(flat_ramp(pt, 2.14f, 2.22f)); end = fx_about(240, 300, lerpf(1.03f, 1, u), lerpf(0.96f, 1, u)); }
      }
      body = fx_mul(fx_move(FLAT_X0, FLAT_Y0), fx_mul(drop, fx_mul(end, fx_mul(fx_move(0, -12.0f * bob), sq))));
   }

   /* the line of shadow breathes with the float; system and title under it */
   if (text_a > 0.003f)
   {
      const float sw = f->shadow * (1.0f - 0.16f * bob), sc[4] = { 0, 0, 0, 0.30f * (1.0f - 0.3f * bob) * text_a };
      const float kc[4] = { 1, 1, 1, 0.62f * text_a }, tc[4] = { 0.98f, 0.98f, 0.99f, text_a };
      draw_shape(W * 0.5f - sw * 0.5f, 474.0f, sw, 4.0f, 2.0f, 0.0f, sc);
      draw_2d(f->l_kicker.tex, floorf((W - f->l_kicker.w) * 0.5f), 508.0f, f->l_kicker.w, f->l_kicker.h, kc, 0);
      draw_2d(f->l_title.tex, floorf((W - f->l_title.w) * 0.5f), 530.0f, f->l_title.w, f->l_title.h, tc, 0);
   }

   /* the door: a lid stands up behind the medium (seen from the front it folds to a line and
    * flips); a flap swings up out of the way */
   if (f->door)
   {
      const float open = flat_ramp(pt, 0.80f, 0.94f), shut = flat_ramp(pt, FLAT_SEAT + 0.04f, FLAT_SEAT + 0.18f), o = flat_smooth(open) * (1.0f - flat_smooth(shut));
      door_sy = f->door == 1 ? lerpf(1.0f, -0.9f, o) : lerpf(1.0f, 0.07f, o);
      door = fx_about(f->door_o[0], f->door_o[1], 1.0f, door_sy);
   }

   /* the medium: from where it comes to where it ends up */
   if (f->medium != FM_NONE)
   {
      med_a = flat_ramp(pt, 0.94f, 1.00f);
      u = flat_ramp(pt, 1.00f, FLAT_SEAT); u = u * u * (1.6f - 0.6f * u);   /* accelerates, then seats */
      if (f->medium == FM_FRONT) med = fx_mul(fx_move(0, 130.0f * (1.0f - flat_smooth(flat_ramp(pt, 1.00f, FLAT_SEAT)))), fx_about(240, 180, lerpf(1.35f, 1, flat_smooth(flat_ramp(pt, 1.00f, FLAT_SEAT))), lerpf(1.35f, 1, flat_smooth(flat_ramp(pt, 1.00f, FLAT_SEAT)))));
      else if (f->medium == FM_COIN)
      {  /* to the slot, then edge first into it */
         const float a = flat_smooth(flat_ramp(pt, 1.00f, FLAT_SEAT - 0.08f)), e = flat_ramp(pt, FLAT_SEAT - 0.08f, FLAT_SEAT);
         med = fx_mul(fx_move(64.0f * (1.0f - a), -140.0f * (1.0f - a) + 8.0f * flat_ramp(pt, FLAT_SEAT, FLAT_SEAT + 0.04f)),
                      fx_about(f->origin[0], f->origin[1], lerpf(1.5f, 1, a) * lerpf(1, 0.14f, e), lerpf(1.5f, 1, a)));
         med_a *= 1.0f - flat_ramp(pt, FLAT_SEAT, FLAT_SEAT + 0.04f);
      }
      else med = fx_move(0, -170.0f * (1.0f - u));
      if (f->medium == FM_CLIP && f->has_clip_y)
      {  /* a slot seen from above: nothing of the medium shows below its edge */
         const float y = body.sy * f->clip_y + body.ty;
         clip.n = 1; clip.r[0][0] = 0; clip.r[0][1] = 0; clip.r[0][2] = W; clip.r[0][3] = y > 0 ? y : 0;
      }
      else if (f->medium == FM_FRONT)
      {  /* a front door: inside its opening, or in front of the machine below it */
         const float y = body.sy * f->clip_rect[3] + body.ty;
         clip.n = 2;
         flat_rect(body, f->clip_rect[0], f->clip_rect[1], f->clip_rect[2] - f->clip_rect[0], f->clip_rect[3] - f->clip_rect[1], clip.r[0]);
         clip.r[1][0] = 0; clip.r[1][1] = y; clip.r[1][2] = W; clip.r[1][3] = H - y > 0 ? H - y : 0;
      }
   }
   lit = pt < 1.56f ? 0.0f : pt < 1.60f ? 0.7f : pt < 1.66f ? 0.2f : 1.0f;

   glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);   /* the layers are premultiplied */
   if (f->medium == FM_BEHIND)
   {
      flat_layer(f->tex[FL_MEDIUM], fx_mul(body, med), med_a, NULL);
   }
   flat_layer(f->tex[FL_BODY], body, 1.0f, NULL);
   if (f->door == 1 && door_sy < 0.0f) flat_layer(f->tex[FL_UNDER], fx_mul(body, door), 1.0f, NULL);
   if (f->medium != FM_NONE && f->medium != FM_BEHIND)
   {
      flat_layer(f->tex[FL_MEDIUM], fx_mul(body, med), med_a, &clip);
   }
   if (f->door && (f->door == 2 || door_sy >= 0.0f)) flat_layer(f->tex[FL_OVER], fx_mul(body, door), 1.0f, NULL);
   if (f->has_screen && lit > 0.0f)
   {  /* the lit screen: the cover's five tones in flat bands, the lightest at the top */
      float r[4];
      flat_rect(body, f->screen[0], f->screen[1], f->screen[2], f->screen[3], r);
      for (i = 0; i < 5; i++)
      {
         const float c[4] = { f->pal[i][0] * lit, f->pal[i][1] * lit, f->pal[i][2] * lit, lit };
         draw_2d(0, r[0], r[1], r[2], r[3] * (5 - i) / 5.0f, c, 0);
      }
   }
   flat_layer(f->tex[FL_LIT], body, lit, NULL);
   if (pt > FLAT_SEAT - 0.02f && pt < FLAT_SEAT + 0.22f)
   {  /* the three strokes of the click */
      const float in = flat_ramp(pt, FLAT_SEAT - 0.02f, FLAT_SEAT + 0.02f), out = flat_ramp(pt, FLAT_SEAT + 0.02f, FLAT_SEAT + 0.22f);
      const float s = lerpf(0.7f, 1.0f, in) + 0.25f * out;
      flat_layer(f->tex[FL_TICK], fx_mul(body, fx_about(f->tick_o[0], f->tick_o[1], s, s)), in * (1.0f - out), NULL);
   }
   glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}
