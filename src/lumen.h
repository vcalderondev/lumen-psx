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
#define MAX_PLATFORMS 32   /* systems + Favourites + folders (collections.h) */

enum { KIND_DISC, KIND_CART, KIND_ARCADE };
typedef struct {
   char title[128], publisher[96], year[16], players[8];
   char content[512], cover[512], bg[512], music[512];
   int platform;
   int port;                   /* a native port of the game (the system's [library] ports folder): runs as a program, no core */
   /* runtime */
   GLuint tex;
   int tex_state;              /* 0 none, 2 decoding, 1 loaded, -1 placeholder */
   int has_cover;
   float col[3];               /* average cover colour */
   float acc[3];               /* accent: most saturated hue cluster of the cover */
   int tex_w, tex_h;           /* size of the cover texture (tile cover-fit) */
} game_t;

/* System declaration: <home>/systems/<id>.ini (see docs/systems.md). */
typedef struct {
   char id[32], name[48], logo[300], core[128];
   char layout[8], paths[512], exts[128], sound[300];
   char meta[300];             /* layout = stock: metadata file next to the declaration */
   char ports[512];            /* native ports listed with this system's games ([library] ports, default <root>/games/<id>-native) */
   int order, show_name, kind;
   char flat[32];              /* the launch's illustration: <home>/assets/launch/<flat>/ ([animation] flat, default the id) */
   /* [graphics]: per-core options and RetroArch overrides, "k=v, k=v" lists */
   char gfx_core[48], gfx_enh[640], gfx_orig[640], gfx_ra_enh[160], gfx_ra_orig[160];
   /* third level, "native": core options for a 720p / 1080p output (internal resolution
    * = the display's); has_native when the system declares either list */
   char gfx_nat720[640], gfx_nat1080[640], gfx_ra_nat[160];
   int has_gfx, has_native;
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
extern int        g_nreal_games, g_nreal_plat;   /* games/platforms of the library; the rest are collections */
extern char       g_root[256], g_home[256], g_core_dir[256];
extern TTF_Font  *g_font_title, *g_font_sub, *g_font_small, *g_font_cover;
extern TTF_Font  *g_font_caps, *g_font_medium;   /* kickers (Medium 15), buttons and names (Medium 21) */
extern float      g_ui_scale;   /* device px per logical px (1280x720 layout) */

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
void   gl_scissor(float x, float y, float w, float h);   /* logical rect, top-left origin */
void   ui_shadow(float x, float y, float w, float h, float radius, float blur, float dy, float a);
/* flat rounded rectangle: filled (stroke 0) or a ring of `stroke` px inside its edge */
void   ui_shape(float x, float y, float w, float h, float radius, float stroke, const float rgba[4]);
GLuint tex_from_surface(SDL_Surface *s);   /* mipmapped RGBA texture; the surface stays the caller's */
/* image (tw x th texels) cover-fitted into a rounded rectangle; ends in another program:
 * call gl_use_2d() before more draw_2d() */
void   draw_image(GLuint tex, int tw, int th, float x, float y, float w, float h, float radius, const float rgba[4]);

/* one frame of navigation for UI modules: press edges, with auto-repeat on the directions.
 * back = Circle or Square; erase = Square alone (the keyboard tells them apart) */
typedef struct { int up, down, left, right, confirm, back, erase, options, start, l1, r1; } ui_nav_t;

/* UI callbacks implemented in main.c */
void   ui_language_changed(void);   /* rebuild text baked into textures after i18n_set_language() */
void   ui_cards_changed(void);      /* the active profile changed: regenerate the back of the cases */
void   ui_avatar_changed(void);     /* the active profile's avatar.png changed: load it again */
int    ui_avatar(GLuint *tex, int *w, int *h);
void   ui_avatar_draw(float x, float y, float d, float a);   /* picture or initials, a circle of d px */   /* the active profile's picture; 0 = none (initials) */
void   ui_library_changed(void);    /* a setting changed which systems are listed: rescan next frame */

#endif
