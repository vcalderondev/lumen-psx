/* Animated per-game scenes (low-poly layers + MIDI), format: lumen-scenes/FORMAT.md.
 * A C port of the reference player lumen-scenes/player/lumen-scene.mjs: same camera,
 * flat-shaded meshes, fog, generators (xorshift32), point sprites and shaders.
 *
 * Threads: scene_request() hands a folder to a worker that reads and parses every file
 * (the USB is exFAT over FUSE: no file I/O on the main thread); scene_poll() takes the
 * parsed scene on the main thread and uploads it to GL. Rendering goes to the scene's own
 * FBO at "resolution" x the output size, which the caller composites. */
#ifndef LUMEN_SCENE_H
#define LUMEN_SCENE_H
#include "gl.h"

typedef struct scene scene_t;

int      scene_init(void);                       /* start the loader; 0 ok */
void     scene_shutdown(void);                   /* join the loader, drop pending results */
void     scene_request(const char *dir);         /* load <dir>/scene.json ("" = cancel) */
/* main thread: the result of the latest request, if ready. Returns an uploaded scene (the
 * caller owns it) or NULL; *failed = 1 when the folder could not be loaded. dir_out gets
 * the folder of the result. */
scene_t *scene_poll(char *dir_out, int n, int *failed);
void     scene_free(scene_t *s);                 /* GL objects included (main thread) */
/* music of the scene: absolute path of its .mid or NULL; *volume from scene.json */
const char *scene_music(const scene_t *s, float *volume);
/* draw the scene at time t (seconds since it appeared) for an output of out_w x out_h
 * device pixels. Leaves its FBO bound (size in *fw, *fh) and returns its colour texture.
 * GL state changed: program, VAO (Lumen's quad VAO is rebound), array buffer, viewport,
 * blend (enabled, SRC_ALPHA / ONE_MINUS_SRC_ALPHA), depth test (disabled), depth mask (on). */
GLuint   scene_render(scene_t *s, double t, int out_w, int out_h, int *fw, int *fh);
int      scene_cost_points(const scene_t *s);     /* points drawn last frame (cost meter) */
int      scene_triangles(const scene_t *s);

#endif
