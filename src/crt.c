/* CRT filter for the dashboard (crt.h). One extra pass at the end of the frame:
 *   - the frame is drawn into an offscreen colour + depth target the size of the window.
 *     With MSAA on (setting msaa, the window's samples) the target keeps it: PowerVR's
 *     GL_EXT_multisampled_render_to_texture resolves on chip for free; without it a
 *     multisampled renderbuffer is resolved with one blit (psc-sim's Mesa);
 *   - the pass puts the target on the window: 1 bilinear fetch per pixel and mediump ALU
 *     for the scanlines, the aperture grille, the halation, the curvature, the vignette
 *     and the rounded edge (constants from the CPU). No mipmaps, no second target, no
 *     extra resolve. Drawn as one triangle, or (setting crt_pass=mesh) as a curved mesh
 *     that takes the curvature, the vignette and the edge off the pixel.
 * Tile-based GPU: the target's depth and the window's previous contents are invalidated
 * so neither is ever written to or read back from memory.
 * The RetroArch preset (dist/shaders/crt-lumen.glsl) is the same maths on the game image. */
#include <SDL2/SDL.h>
#include <string.h>
#include "lumen.h"
#include "settings.h"
#include "crt.h"

static const char *const k_ids[CRT_LEVELS] = { "off", "soft", "strong" };

/* per level: scanline depth, mask depth, curvature, halation, vignette, scanline period
 * at 720p and 1080p (device px per virtual line: whole numbers, so no moire at the centre) */
static const struct { float scan, mask, curv, halo, vig; int per720, per1080; } k_lv[CRT_LEVELS] = {
   { 0, 0, 0, 0, 0, 0, 0 },
   { 0.30f, 0.16f, 0.022f, 0.14f, 0.22f, 2, 3 },
   { 0.55f, 0.32f, 0.045f, 0.24f, 0.34f, 3, 4 },
};

/* The pixel's maths shared by both ways of drawing the pass (below), on a colour c, a
 * virtual line yl (highp) and k, the vignette times the edge's coverage. Colour maths in
 * mediump (FP16: twice the rate on the GE8300); no exp, no division. The halation glows
 * from the pixel's own colour (the old 4-tap blur was 2.6 px wide; under 1 dB apart). */
#define FS_HEAD \
   "#version 300 es\n" \
   "precision mediump float;\n" \
   "uniform sampler2D uTex;\n" \
   "flat in vec4 uA;\n"            /* 1 - scanline depth, scanline depth, halation */ \
   "flat in vec4 uB;\n"            /* mask: bright level, bright - dim */ \
   "out vec4 oc;\n"
/* the level's constants reach the pixel as flat varyings: the vertex shader copies the
 * uniforms (kA, kB...) over, so the pixel has no uniform to load */
#define VS_CONST \
   "uniform vec4 kA, kB;\n" \
   "flat out mediump vec4 uA, uB;\n"
#define FS_TAIL \
   /* scanlines: a gaussian beam per virtual line, wider on bright pixels: (1 - x/8)^8 ~ \
    * exp(-x), x = f^2 / 2s^2, s = 0.24 + 0.2 brightness; 1/16s^2 as a quadratic of r+g+b */ \
   "   float f = fract(yl) - 0.5;\n" \
   "   float l = c.r + c.g + c.b;\n" \
   "   float g = 1.0 - f * f * (1.085 + l * (-0.44583 + 0.06389 * l));\n" \
   "   g *= g; g *= g; g *= g;\n" \
   /* aperture grille: R, G, B stripes one device pixel wide */ \
   "   highp float mx = fract(gl_FragCoord.x * (1.0 / 3.0));\n" \
   "   vec3 m = uB.x - uB.y * min(abs(vec3(0.5, 1.5, 2.5) - mx * 3.0), 1.0);\n" \
   "   float bk = (uA.x + uA.y * g) * k;\n" \
   "   c = c * (m * bk + c * (uA.z * k));\n"                /* halation (c^2) glows over the mask */ \
   "   oc = vec4(c, 1.0);\n" \
   "}\n"

/* Way 1, per pixel (the default): one triangle over the window; the pixel works out the
 * curvature, the vignette and the rounded edge (one sqrt) itself. */
static const char *VS_PIX =
   "#version 300 es\n"
   VS_CONST
   "out highp vec2 vP;\n"
   "flat out highp vec4 uC, uE, uK;\n"
   "uniform vec4 kC, kE, kK;\n"
   "void main() {\n"
   "   uA = kA; uB = kB; uC = kC; uE = kE; uK = kK;\n"
   "   vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"   /* (0,0) (2,0) (0,2) */
   "   vP = p * 2.0 - 1.0;\n"                                                     /* the screen: -1..1 */
   "   gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
   "}\n";
static const char *FS_PIX =
   FS_HEAD
   "flat in highp vec4 uC;\n"      /* curvature, vignette / 4, virtual lines, line phase */
   "flat in highp vec4 uE;\n"      /* half the window (px), the same minus the corner radius */
   "flat in highp vec4 uK;\n"      /* 0.5 + corner radius / 1.5 */
   "in highp vec2 vP;\n"
   "void main() {\n"
   "   highp vec2 p = vP;\n"
   "   p *= 1.0 + uC.x * p.yx * p.yx;\n"                    /* barrel: corners curve away */
   "   highp vec2 uv = p * 0.5 + 0.5;\n"
   "   vec3 c = texture(uTex, uv).rgb;\n"
   "   highp float yl = uv.y * uC.z + uC.w;\n"
   /* vignette and the tube's rounded edge (1.5 px antialiased): one scalar */
   "   highp vec2 p2 = p * p;\n"
   "   float r2 = p2.x + p2.y;\n"
   "   highp vec2 q = max(abs(p) * uE.xy - uE.zw, 0.0);\n"
   "   float k = (1.0 - uC.y * r2 * r2) * clamp(uK.x - length(q) * (1.0 / 1.5), 0.0, 1.0);\n"
   FS_TAIL;

/* Way 2, a curved mesh (setting crt_pass=mesh): the curvature, the vignette and the
 * rounded, antialiased edge are worked out per vertex, so the pixel is left with a fetch
 * at an interpolated coordinate (no dependent read) and the shared maths: about a third
 * less work per pixel, for about 1800 triangles. The mesh (mesh_build) holds, per
 * vertex, the point of the frame it shows (aP.xy, -1..1, slightly more on the antialiasing
 * ring) and the edge's coverage (aP.z: 1, or 0 on the outer ring). The vertex goes where
 * the barrel distortion p' = p (1 + curv p.yx^2) puts that point on the screen: the
 * inverse, by fixed-point iteration (it converges fast, curv is small). */
static const char *VS_MESH =
   "#version 300 es\n"
   "layout(location = 0) in vec3 aP;\n"
   VS_CONST
   "uniform vec4 kC;\n"            /* curvature, vignette / 4, virtual lines, line phase */
   "out highp vec2 vUv;\n"
   "out highp float vYl;\n"
   "out mediump float vK;\n"
   "void main() {\n"
   "   uA = kA; uB = kB;\n"
   "   vec4 uC = kC;\n"
   "   vec2 s = aP.xy;\n"
   "   for (int i = 0; i < 5; i++) s = aP.xy / (1.0 + uC.x * s.yx * s.yx);\n"
   "   vUv = aP.xy * 0.5 + 0.5;\n"
   "   vYl = vUv.y * uC.z + uC.w;\n"
   "   float r2 = dot(aP.xy, aP.xy);\n"
   "   vK = aP.z * (1.0 - uC.y * r2 * r2);\n"               /* edge coverage x vignette */
   "   gl_Position = vec4(s, 0.0, 1.0);\n"
   "}\n";
static const char *FS_MESH =
   FS_HEAD
   "in highp vec2 vUv;\n"
   "in highp float vYl;\n"
   "in mediump float vK;\n"
   "void main() {\n"
   "   vec3 c = texture(uTex, vUv).rgb;\n"
   "   highp float yl = vYl;\n"
   "   float k = vK;\n"
   FS_TAIL;

/* mesh: cells per half width / height up to where the corner starts, lines across each
 * corner (the arc's segments), plus the antialiasing ring */
#ifndef MESH_NX
#define MESH_NX 12
#define MESH_NY 8
#define MESH_NC 4
#endif
#define MESH_LX (2 * (MESH_NX + MESH_NC + 1) + 1)
#define MESH_LY (2 * (MESH_NY + MESH_NC + 1) + 1)

typedef void (*fbtex_ms_fn)(GLenum, GLenum, GLenum, GLuint, GLint, GLsizei);
typedef void (*rb_ms_fn)(GLenum, GLsizei, GLenum, GLsizei, GLsizei);

static GLuint s_prog, s_vao, s_vbo, s_ibo, s_fbo, s_tex, s_depth, s_msfbo, s_mscol;
static GLint  s_uTex, s_uA, s_uB, s_uC, s_uE, s_uK;
static int    s_mesh = -1;                     /* the program in s_prog: 0 per pixel, 1 mesh */
static int    s_mesh_w, s_mesh_h, s_mesh_lv;   /* what the mesh was built for */
static int    s_w, s_h, s_samples, s_mode;   /* mode: 0 plain, 1 on-chip MSAA, 2 MSAA + blit */
static int    s_failed, s_on, s_level;

int crt_level(void)
{
   const char *v = settings_get("crt", "off");
   int i;
   for (i = 0; i < CRT_LEVELS; i++) if (!strcmp(v, k_ids[i])) return i;
   return CRT_OFF;
}

const char *crt_id(int level) { return level >= 0 && level < CRT_LEVELS ? k_ids[level] : k_ids[0]; }

GLuint crt_target(void) { return s_on ? (s_mode == 2 ? s_msfbo : s_fbo) : 0; }

static GLuint compile(GLenum type, const char *src)
{
   GLuint s = glCreateShader(type);
   GLint ok = 0;
   glShaderSource(s, 1, &src, NULL);
   glCompileShader(s);
   glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
   if (!ok)
   {
      char log[1024];
      glGetShaderInfoLog(s, sizeof(log), NULL, log);
      logf_("crt: shader compile failed: %s", log);
      glDeleteShader(s);
      return 0;
   }
   return s;
}

static int program_init(int mesh)
{
   GLuint vs = compile(GL_VERTEX_SHADER, mesh ? VS_MESH : VS_PIX), fs = compile(GL_FRAGMENT_SHADER, mesh ? FS_MESH : FS_PIX);
   GLint ok = 0;
   if (!vs || !fs) return -1;
   s_prog = glCreateProgram();
   glAttachShader(s_prog, vs);
   glAttachShader(s_prog, fs);
   glLinkProgram(s_prog);
   glDeleteShader(vs);
   glDeleteShader(fs);
   glGetProgramiv(s_prog, GL_LINK_STATUS, &ok);
   if (!ok) { logf_("crt: program link failed"); glDeleteProgram(s_prog); s_prog = 0; return -1; }
   s_uTex = glGetUniformLocation(s_prog, "uTex");
   s_uA = glGetUniformLocation(s_prog, "kA");
   s_uB = glGetUniformLocation(s_prog, "kB");
   s_uC = glGetUniformLocation(s_prog, "kC");
   s_uE = glGetUniformLocation(s_prog, "kE");
   s_uK = glGetUniformLocation(s_prog, "kK");
   if (!s_vao)
   {
      glGenVertexArrays(1, &s_vao);
      glGenBuffers(1, &s_vbo);
      glGenBuffers(1, &s_ibo);
   }
   s_mesh = mesh;
   s_mesh_w = s_mesh_h = 0;
   return 0;
}

/* one axis of the mesh, in pixels from the centre: n cells out to where the corner starts
 * (e = half - rad), MESH_NC lines across the corner up to the edge's inner side
 * (rad - 0.75), one more on its outer side (rad + 0.75); mirrored. Returns the count. */
static int mesh_axis(float *a, int n, float half, float rad)
{
   const float e = half - rad;
   const int h = n + MESH_NC + 1;   /* lines on one side, without the centre */
   int i;
   a[h] = 0.0f;
   for (i = 1; i <= h; i++)
   {
      const float v = i <= n ? e * i / n
                    : (i < h ? e + (rad - 0.75f) * (i - n) / MESH_NC : e + rad + 0.75f);
      a[h + i] = v; a[h - i] = -v;
   }
   return 2 * h + 1;
}

/* The mesh for a w x h window and corner radius rad (px): a grid over the frame whose
 * corner squares are squeezed onto quarter circles (a point at (dx, dy) from the corner's
 * centre goes to the arc of radius max(dx, dy)), so its border is the rounded rectangle.
 * The outer ring of vertices lies 1.5 px further out with coverage 0: the edge's
 * antialiasing. Bound to s_vao; the caller restores the bindings. */
static void mesh_build(int w, int h, float rad)
{
   static float xs[MESH_LX], ys[MESH_LY], v[MESH_LX * MESH_LY * 3];
   static GLushort idx[(MESH_LX - 1) * (MESH_LY - 1) * 6];
   const float hx = w * 0.5f, hy = h * 0.5f, ex = hx - rad, ey = hy - rad;
   const int nx = mesh_axis(xs, MESH_NX, hx, rad), ny = mesh_axis(ys, MESH_NY, hy, rad);
   int ix, iy, n = 0;
   for (iy = 0; iy < ny; iy++)
      for (ix = 0; ix < nx; ix++)
      {
         float x = xs[ix], y = ys[iy];
         float dx = fabsf(x) - ex, dy = fabsf(y) - ey;
         if (dx > 0.0f && dy > 0.0f)
         {
            const float k = (dx > dy ? dx : dy) / sqrtf(dx * dx + dy * dy);
            x = (x < 0.0f ? -1.0f : 1.0f) * (ex + dx * k);
            y = (y < 0.0f ? -1.0f : 1.0f) * (ey + dy * k);
         }
         v[n++] = x / hx; v[n++] = y / hy;
         v[n++] = (ix == 0 || iy == 0 || ix == nx - 1 || iy == ny - 1) ? 0.0f : 1.0f;
      }
   n = 0;
   for (iy = 0; iy < ny - 1; iy++)
      for (ix = 0; ix < nx - 1; ix++)
      {
         const GLushort a = (GLushort)(iy * nx + ix), b = (GLushort)(a + 1), c = (GLushort)(a + nx), d = (GLushort)(c + 1);
         idx[n++] = a; idx[n++] = b; idx[n++] = c;
         idx[n++] = b; idx[n++] = d; idx[n++] = c;
      }
   glBindVertexArray(s_vao);
   glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
   glBufferData(GL_ARRAY_BUFFER, sizeof(v), v, GL_STATIC_DRAW);
   glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, (void *)0);
   glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_ibo);   /* kept by the VAO */
   glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(idx), idx, GL_STATIC_DRAW);
}

static void target_free(void)
{
   if (s_fbo) glDeleteFramebuffers(1, &s_fbo);
   if (s_msfbo) glDeleteFramebuffers(1, &s_msfbo);
   if (s_tex) glDeleteTextures(1, &s_tex);
   if (s_depth) glDeleteRenderbuffers(1, &s_depth);
   if (s_mscol) glDeleteRenderbuffers(1, &s_mscol);
   s_fbo = s_msfbo = s_tex = s_depth = s_mscol = 0;
   s_w = s_h = 0;
}

static int target_init(int w, int h)
{
   GLint prev_fb = 0, prev_tex = 0, prev_rb = 0, samples = 0;
   int ok;
   static fbtex_ms_fn fbtex_ms;
   static rb_ms_fn rb_ms;
   static int probed;
   if (!probed)
   {
      probed = 1;
      if (SDL_GL_ExtensionSupported("GL_EXT_multisampled_render_to_texture"))
      {
         fbtex_ms = (fbtex_ms_fn)SDL_GL_GetProcAddress("glFramebufferTexture2DMultisampleEXT");
         rb_ms = (rb_ms_fn)SDL_GL_GetProcAddress("glRenderbufferStorageMultisampleEXT");
      }
   }
   glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fb);
   glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
   glGetIntegerv(GL_RENDERBUFFER_BINDING, &prev_rb);
   glBindFramebuffer(GL_FRAMEBUFFER, 0);
   glGetIntegerv(GL_SAMPLES, &samples);   /* the window's: the setting msaa */
   target_free();
   s_samples = samples;
   s_mode = samples <= 0 ? 0 : (fbtex_ms && rb_ms ? 1 : 2);

   glGenTextures(1, &s_tex);
   glBindTexture(GL_TEXTURE_2D, s_tex);
   glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGB8, w, h);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
   glGenFramebuffers(1, &s_fbo);
   glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
   glGenRenderbuffers(1, &s_depth);
   glBindRenderbuffer(GL_RENDERBUFFER, s_depth);
   if (s_mode == 1)
   {
      fbtex_ms(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_tex, 0, samples);
      rb_ms(GL_RENDERBUFFER, samples, GL_DEPTH_COMPONENT16, w, h);
      glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, s_depth);
   }
   else
   {
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_tex, 0);
      if (s_mode == 2)
      {  /* draw into a multisampled FBO, resolve into s_fbo (colour only) */
         glGenFramebuffers(1, &s_msfbo);
         glBindFramebuffer(GL_FRAMEBUFFER, s_msfbo);
         glGenRenderbuffers(1, &s_mscol);
         glBindRenderbuffer(GL_RENDERBUFFER, s_mscol);
         glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_RGB8, w, h);
         glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, s_mscol);
         glBindRenderbuffer(GL_RENDERBUFFER, s_depth);
         glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_DEPTH_COMPONENT16, w, h);
      }
      else glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, w, h);
      glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, s_depth);
   }
   ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
   if (ok && s_mode == 2)
   {
      glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
      ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
   }
   glBindFramebuffer(GL_FRAMEBUFFER, prev_fb);
   glBindTexture(GL_TEXTURE_2D, prev_tex);
   glBindRenderbuffer(GL_RENDERBUFFER, prev_rb);
   if (!ok) { logf_("crt: %dx%d target incomplete (mode %d)", w, h, s_mode); target_free(); return -1; }
   s_w = w; s_h = h;
   logf_("crt: target %dx%d, MSAA %d (%s)", w, h, samples,
         s_mode == 1 ? "on-chip resolve" : (s_mode == 2 ? "blit resolve" : "none"));
   return 0;
}

int crt_begin(int w, int h)
{
   s_on = 0;
   s_level = crt_level();
   if (s_level != CRT_OFF && !s_failed)
   {
      const int mesh = !strcmp(settings_get("crt_pass", "pixel"), "mesh");
      if (s_prog && mesh != s_mesh) { glDeleteProgram(s_prog); s_prog = 0; }   /* the setting changed */
      if (!s_prog && program_init(mesh) < 0) s_failed = 1;
      if (!s_failed && (w != s_w || h != s_h) && target_init(w, h) < 0) s_failed = 1;
      if (s_failed) logf_("crt: filter unavailable, drawing without it");
      else s_on = 1;
   }
   else if (s_level == CRT_OFF && s_fbo) target_free();   /* switched off: give the memory back */
   glBindFramebuffer(GL_FRAMEBUFFER, crt_target());
   return s_on;
}

void crt_prepare(int w, int h)
{
   GLint fb = 0;
   glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fb);
   crt_begin(w, h);
   s_on = 0;
   glBindFramebuffer(GL_FRAMEBUFFER, fb);
}

void crt_end(void)
{
   static const GLenum k_depth[1] = { GL_DEPTH_ATTACHMENT }, k_win[2] = { GL_COLOR, GL_DEPTH };
   GLint prog = 0, tex = 0, vao = 0, unit = 0, abuf = 0;
   GLboolean blend, depth, scissor;
   GLfloat cc[4];
   float gain, per, rad;
   const int lv = s_level;
   if (!s_on) return;
   if (s_mode == 2)
   {
      glBindFramebuffer(GL_READ_FRAMEBUFFER, s_msfbo);
      glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_fbo);
      glBlitFramebuffer(0, 0, s_w, s_h, 0, 0, s_w, s_h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
      glBindFramebuffer(GL_FRAMEBUFFER, s_msfbo);
   }
   glInvalidateFramebuffer(GL_FRAMEBUFFER, 1, k_depth);   /* the scene's depth is never stored */
   glBindFramebuffer(GL_FRAMEBUFFER, 0);
   glInvalidateFramebuffer(GL_FRAMEBUFFER, 2, k_win);     /* nothing of the old window is kept */

   glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
   glGetIntegerv(GL_ACTIVE_TEXTURE, &unit);
   glActiveTexture(GL_TEXTURE0);
   glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
   glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
   glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &abuf);
   glGetFloatv(GL_COLOR_CLEAR_VALUE, cc);
   blend = glIsEnabled(GL_BLEND); depth = glIsEnabled(GL_DEPTH_TEST); scissor = glIsEnabled(GL_SCISSOR_TEST);
   glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_SCISSOR_TEST);
   glViewport(0, 0, s_w, s_h);
   /* the mask and the scanlines take light away: win most of it back (bright parts clip) */
   gain = powf(1.0f / ((1.0f - 0.35f * k_lv[lv].scan) * (1.0f - 0.667f * k_lv[lv].mask)), 0.85f);
   per = (float)(s_h >= 1000 ? k_lv[lv].per1080 : k_lv[lv].per720);
   rad = s_h * (0.012f + k_lv[lv].curv * 0.6f);   /* corner radius ~3% of the height (more with more curvature) */
   glUseProgram(s_prog);
   glUniform1i(s_uTex, 0);
   glUniform4f(s_uA, 1.0f - k_lv[lv].scan, k_lv[lv].scan, k_lv[lv].halo, 0.0f);
   glUniform4f(s_uB, gain, gain * k_lv[lv].mask, 0.0f, 0.0f);
   glUniform4f(s_uC, k_lv[lv].curv, k_lv[lv].vig * 0.25f, s_h / per, 0.5f - 0.5f / per);
   glBindTexture(GL_TEXTURE_2D, s_tex);
   glBindVertexArray(s_vao);
   if (s_mesh)
   {
      if (s_mesh_w != s_w || s_mesh_h != s_h || s_mesh_lv != lv)
      {
         mesh_build(s_w, s_h, rad);
         s_mesh_w = s_w; s_mesh_h = s_h; s_mesh_lv = lv;
      }
      glEnableVertexAttribArray(0);
      glClearColor(0.0f, 0.0f, 0.0f, 1.0f);               /* around the tube: black (the mesh stops at its edge) */
      glClear(GL_COLOR_BUFFER_BIT);
      glDrawElements(GL_TRIANGLES, (MESH_LX - 1) * (MESH_LY - 1) * 6, GL_UNSIGNED_SHORT, (void *)0);
   }
   else
   {
      const float hx = s_w * 0.5f, hy = s_h * 0.5f;
      glUniform4f(s_uE, hx, hy, hx - rad, hy - rad);
      glUniform4f(s_uK, 0.5f + rad / 1.5f, 0.0f, 0.0f, 0.0f);
      glDisableVertexAttribArray(0);                      /* gl_VertexID only */
      glDrawArrays(GL_TRIANGLES, 0, 3);
   }

   glClearColor(cc[0], cc[1], cc[2], cc[3]);
   glBindBuffer(GL_ARRAY_BUFFER, (GLuint)abuf);
   glBindVertexArray(vao);
   glBindTexture(GL_TEXTURE_2D, tex);
   glActiveTexture(unit);
   glUseProgram(prog);
   if (blend) glEnable(GL_BLEND);
   if (depth) glEnable(GL_DEPTH_TEST);
   if (scissor) glEnable(GL_SCISSOR_TEST);
}
