/* CRT filter (setting "crt": off | soft | strong, Settings -> Display): the frame is drawn
 * into an offscreen target and put on screen through a CRT shader (scanlines, aperture
 * grille, slight curvature, halation, vignette). The same look for the games comes from
 * dist/shaders/crt-lumen-<level>.glslp, handed to RetroArch by scripts/run.sh. */
#ifndef LUMEN_CRT_H
#define LUMEN_CRT_H

#include "gl.h"

enum { CRT_OFF, CRT_SOFT, CRT_STRONG, CRT_LEVELS };

int         crt_level(void);              /* the setting, CRT_OFF..CRT_STRONG */
const char *crt_id(int level);            /* "off" | "soft" | "strong" */
/* start a frame w x h (device px): binds the scene target when the filter is on (else the
 * window), returns 1 when on; the viewport is the caller's */
int         crt_begin(int w, int h);
/* at startup: compile the shader and allocate the target now (when the filter is on), so
 * the first frame does not take the hit; nothing is bound */
void        crt_prepare(int w, int h);
GLuint      crt_target(void);             /* framebuffer the frame draws into (0: window) */
/* end the frame: draws the scene to the window through the shader (no-op when off).
 * Leaves the window bound; blend, depth, scissor, program, VAO, array buffer, clear colour
 * and texture are restored. Setting crt_pass (pixel | mesh) picks how the pass is drawn. */
void        crt_end(void);

#endif
