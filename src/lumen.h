/* Lumen — data model and render services shared by the frontend modules.
 * main.c owns every definition; the other units only use what is declared here. */
#ifndef LUMEN_H
#define LUMEN_H
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include "gl.h"

#define W 1280
#define H 720
#define MAX_GAMES 2048
#define MAX_PLATFORMS 16

enum { KIND_DISC, KIND_CART, KIND_ARCADE };
enum { ANIM_OPEN_CASE_DISC, ANIM_INSERT_CART, ANIM_INSERT_COIN };

typedef struct {
   char title[128], publisher[96], year[16], players[8];
   char content[512], cover[512], bg[512], disc[512];
   int platform;
   /* runtime */
   GLuint tex, back_tex, spine_tex;
   int tex_state;              /* 0 none, 2 decoding, 1 loaded, -1 placeholder */
   int has_cover;
   float col[3];               /* average cover colour */
   spring x, z, ry, sc, al;
} game_t;

/* System declaration: <home>/systems/<id>.ini (see docs/systems.md). */
typedef struct {
   char id[32], name[48], logo[300], core[128];
   char layout[8], paths[512], exts[128], art[16], media_art[16], sound[300];
   int order, show_name, kind, launch, idle_float;
   float aspect, w, h, color[3];
   /* [graphics]: per-core options and RetroArch overrides, "k=v, k=v" lists */
   char gfx_core[48], gfx_enh[320], gfx_orig[320], gfx_ra_enh[160], gfx_ra_orig[160];
   int has_gfx;
   /* [demo]: attract-mode demos allowed, per-system length (0 = settings demo_seconds) */
   int demo_allowed, demo_seconds;
   /* runtime */
   int first, count;
   GLuint label, logo_tex;
   int label_w, label_h, logo_w, logo_h;
} platform_t;

extern game_t     g_games[MAX_GAMES];
extern int        g_ngames;
extern platform_t g_plat[MAX_PLATFORMS];
extern int        g_nplat;
extern char       g_root[256], g_home[256], g_core_dir[256];
extern TTF_Font  *g_font_title, *g_font_sub, *g_font_small, *g_font_cover;

typedef struct { GLuint tex; int w, h; char key[200]; } label_t;

void   logf_(const char *fmt, ...);
double now_s(void);
void   draw_2d(GLuint tex, float x, float y, float w, float h, const float rgba[4], int mode);
/* glass rounded rectangle (FS_2D mode 5): fill rgba with a vertical sheen, 1 px inner border */
void   draw_round_rect(float x, float y, float w, float h, float radius, const float rgba[4], float border_alpha);
GLuint text_tex(TTF_Font *f, const char *s, int *w, int *h);
GLuint text_tex_wrapped(TTF_Font *f, const char *s, int wrap_px, int *w, int *h);   /* honours '\n' */
void   label_set(label_t *l, TTF_Font *f, const char *s);   /* re-renders only when s changes */
void   label_free(label_t *l);
void   gl_use_2d(void);        /* glUseProgram(g_prog_2d) + sampler 0 */

/* one frame of navigation for UI modules: press edges, with auto-repeat on the directions */
typedef struct { int up, down, left, right, confirm, back, options, start, l1, r1; } ui_nav_t;

/* UI callbacks implemented in main.c */
void   ui_language_changed(void);   /* rebuild text baked into textures after i18n_set_language() */
void   ui_cards_changed(void);      /* the active profile changed: regenerate the back of the cases */

#endif
