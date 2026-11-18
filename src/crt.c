/* CRT filter for the dashboard (crt.h). One extra full-screen pass at the end of the frame:
 *   - the frame is drawn into an offscreen colour + depth target the size of the window.
 *     With MSAA on (setting msaa, the window's samples) the target keeps it: PowerVR's
 *     GL_EXT_multisampled_render_to_texture resolves on chip for free; without it a
 *     multisampled renderbuffer is resolved with one blit (psc-sim's Mesa);
 *   - the pass draws one triangle over the window: 1 bilinear fetch and mediump ALU for
 *     the scanlines, the aperture grille, the curvature, the halation, the vignette and the
 *     rounded edge (constants from the CPU). No mipmaps, no second target, no extra resolve.
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

static const char *VS_CRT =
   "#version 300 es\n"
   "out highp vec2 vP;\n"
   "void main() {\n"
   "   vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"   /* (0,0) (2,0) (0,2) */
   "   vP = p * 2.0 - 1.0;\n"                                                     /* the screen: -1..1 */
   "   gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
   "}\n";

/* Colour maths in mediump (FP16: twice the rate on the GE8300), coordinates in highp;
 * everything that only depends on the level and the size is worked out on the CPU
 * (crt_end) and comes in as uniforms. Per pixel: 1 bilinear fetch (the halation glows
 * from the pixel's own colour: the old 4-tap blur was 2.6 px wide and the difference is
 * under 1 dB), one sqrt, no exp or division. */
static const char *FS_CRT =
   "#version 300 es\n"
   "precision mediump float;\n"
   "uniform sampler2D uTex;\n"
   "uniform vec4 uA;\n"            /* scanline depth, curvature, halation, vignette */
   "uniform highp vec4 uB;\n"      /* virtual lines, line phase, mask bright level, bright - dim */
   "uniform highp vec4 uE;\n"      /* half the window (px), the same minus the corner radius */
   "uniform highp vec4 uH;\n"      /* z: 0.5 + corner radius / 1.5 */
   "in highp vec2 vP;\n"
   "out vec4 oc;\n"
   "void main() {\n"
   "   highp vec2 p = vP;\n"
   "   p *= 1.0 + uA.y * p.yx * p.yx;\n"                    /* barrel: corners curve away */
   "   highp vec2 uv = p * 0.5 + 0.5;\n"
   "   vec3 c = texture(uTex, uv).rgb;\n"
   /* scanlines: a gaussian beam per virtual line, wider on bright pixels: (1 - x/8)^8 ~
    * exp(-x), x = f^2 / 2s^2, 1/s^2 a quadratic in the brightness (no exp, no division) */
   "   highp float yl = uv.y * uB.x + uB.y;\n"
   "   float f = fract(yl) - 0.5;\n"
   "   float l = dot(c, vec3(0.299, 0.587, 0.114));\n"
   "   float g = max(1.0 - f * f * (17.36 + l * (-21.4 + 9.2 * l)) * (1.0 / 16.0), 0.0);\n"
   "   g *= g; g *= g; g *= g;\n"
   /* vignette and the tube's rounded edge (1.5 px antialiased): one scalar */
   "   float r2 = dot(p, p) * 0.5;\n"
   "   highp vec2 q = max(abs(p) * uE.xy - uE.zw, 0.0);\n"
   "   float k = (1.0 - uA.w * r2 * r2) * clamp(uH.z - length(q) * (1.0 / 1.5), 0.0, 1.0);\n"
   /* aperture grille: R, G, B stripes one device pixel wide (phase 1/6, 1/2, 5/6) */
   "   highp float mx = fract(gl_FragCoord.x * (1.0 / 3.0));\n"
   "   vec3 m = uB.z - uB.w * step(0.25, abs(vec3(1.0 / 6.0, 0.5, 5.0 / 6.0) - mx));\n"
   "   c = c * (m * ((1.0 - uA.x + uA.x * g) * k) + c * (uA.z * k));\n"   /* halation (c^2) glows over the mask */
   "   oc = vec4(c, 1.0);\n"
   "}\n";

typedef void (*fbtex_ms_fn)(GLenum, GLenum, GLenum, GLuint, GLint, GLsizei);
typedef void (*rb_ms_fn)(GLenum, GLsizei, GLenum, GLsizei, GLsizei);

static GLuint s_prog, s_vao, s_fbo, s_tex, s_depth, s_msfbo, s_mscol;
static GLint  s_uTex, s_uA, s_uB, s_uE, s_uH;
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

static int program_init(void)
{
   GLuint vs = compile(GL_VERTEX_SHADER, VS_CRT), fs = compile(GL_FRAGMENT_SHADER, FS_CRT);
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
   s_uA = glGetUniformLocation(s_prog, "uA");
   s_uB = glGetUniformLocation(s_prog, "uB");
   s_uE = glGetUniformLocation(s_prog, "uE");
   s_uH = glGetUniformLocation(s_prog, "uH");
   glGenVertexArrays(1, &s_vao);   /* no attributes: the vertex shader uses gl_VertexID */
   return 0;
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
      if (!s_prog && program_init() < 0) s_failed = 1;
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
   GLint prog = 0, tex = 0, vao = 0, unit = 0;
   GLboolean blend, depth, scissor;
   float gain, per, rad, hx, hy;
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
   glInvalidateFramebuffer(GL_FRAMEBUFFER, 2, k_win);     /* every window pixel is drawn below */

   glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
   glGetIntegerv(GL_ACTIVE_TEXTURE, &unit);
   glActiveTexture(GL_TEXTURE0);
   glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
   glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
   blend = glIsEnabled(GL_BLEND); depth = glIsEnabled(GL_DEPTH_TEST); scissor = glIsEnabled(GL_SCISSOR_TEST);
   glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_SCISSOR_TEST);
   glViewport(0, 0, s_w, s_h);

   /* the mask and the scanlines take light away: win most of it back (bright parts clip) */
   gain = powf(1.0f / ((1.0f - 0.35f * k_lv[lv].scan) * (1.0f - 0.667f * k_lv[lv].mask)), 0.85f);
   per = (float)(s_h >= 1000 ? k_lv[lv].per1080 : k_lv[lv].per720);
   glUseProgram(s_prog);
   glUniform1i(s_uTex, 0);
   /* corner radius ~3% of the height (more with more curvature) */
   rad = s_h * (0.012f + k_lv[lv].curv * 0.6f);
   hx = s_w * 0.5f; hy = s_h * 0.5f;
   glUniform4f(s_uA, k_lv[lv].scan, k_lv[lv].curv, k_lv[lv].halo, k_lv[lv].vig);
   glUniform4f(s_uB, s_h / per, 0.5f - 0.5f / per, gain, gain * k_lv[lv].mask);
   glUniform4f(s_uE, hx, hy, hx - rad, hy - rad);
   glUniform4f(s_uH, 0.0f, 0.0f, 0.5f + rad / 1.5f, 0.0f);
   glBindTexture(GL_TEXTURE_2D, s_tex);
   glBindVertexArray(s_vao);
   glDrawArrays(GL_TRIANGLES, 0, 3);

   glBindVertexArray(vao);
   glBindTexture(GL_TEXTURE_2D, tex);
   glActiveTexture(unit);
   glUseProgram(prog);
   if (blend) glEnable(GL_BLEND);
   if (depth) glEnable(GL_DEPTH_TEST);
   if (scissor) glEnable(GL_SCISSOR_TEST);
}
