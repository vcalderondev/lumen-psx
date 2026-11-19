/* crt-bench: time a RetroArch-style shader pass offscreen and save the frame.
 *
 * Reproduces what RetroArch 1.22's gl driver (gfx/drivers/gl2.c, shader_glsl.c) does with
 * a one-pass .glslp on a 1280x720 screen and a 4:3 core: upload the core's frame into a
 * padded texture, then either
 *   fbo    - the pass has a scale_type: it renders into a viewport-sized FBO, then the
 *            stock shader copies that FBO to the back buffer (an extra pass);
 *   direct - no scale_type: the pass renders straight into the back buffer's viewport;
 *   none   - shaders off: the stock shader draws the core's frame.
 * The shader source gets the same preamble RetroArch gives it (#version 300 es, VERTEX /
 * FRAGMENT, PARAMETER_UNIFORM) and the same uniforms (MVPMatrix, OutputSize, TextureSize,
 * InputSize, Texture, the parameters). Runs on EGL surfaceless (psc-sim's Mesa llvmpipe).
 *
 * usage: crt-bench MODE SHADER.glsl|- SRC.ppm OUT.ppm FRAMES "SCAN MASK CURV HALO VIG"
 *   dash   - a full-screen-triangle pass over a 1280x720 frame (SRC): SHADER is a prefix,
 *            PREFIX.vs / PREFIX.fs. The dashboard's CRT pass as it was in 0.20.0
 *            (extract-dash.py pulls its shaders out of that src/crt.c) or dash-off (a copy)
 *   live   - the dashboard's CRT pass as it is: this program links src/crt.c and calls
 *            crt_begin() / crt_end() on a 1280x720 pbuffer; the level is soft, or strong
 *            when SCAN > 0.5
 * prints: ms/frame (best of 3 runs of FRAMES frames each) */
#define EGL_EGLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

#define SCR_W 1280
#define SCR_H 720

static const char *STOCK =
   "#if defined(VERTEX)\n"
   "in vec4 VertexCoord; in vec4 TexCoord; out vec2 tc; uniform mat4 MVPMatrix;\n"
   "void main() { gl_Position = MVPMatrix * VertexCoord; tc = TexCoord.xy; }\n"
   "#else\n"
   "precision mediump float; uniform sampler2D Texture; in vec2 tc; out vec4 o;\n"
   "void main() { o = texture(Texture, tc); }\n"
   "#endif\n";

static void die(const char *m) { fprintf(stderr, "crt-bench: %s\n", m); exit(1); }

static char *slurp(const char *path)
{
   FILE *f = fopen(path, "rb");
   long n;
   char *b;
   if (!f) die("cannot open shader");
   fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
   b = malloc(n + 1);
   if (fread(b, 1, n, f) != (size_t)n) die("short read");
   b[n] = 0; fclose(f);
   return b;
}

static GLuint compile(GLenum type, const char *body)
{
   /* RetroArch: replaces the file's #version with the GLES one, then its defines */
   const char *src[3];
   const char *v = strstr(body, "#version");
   GLuint s = glCreateShader(type);
   GLint ok;
   if (v) { v += 8; while (*v && *v != '\n') v++; body = v; }
   src[0] = "#version 300 es\n";
   src[1] = type == GL_VERTEX_SHADER
      ? "#define VERTEX\n#define PARAMETER_UNIFORM\n"
      : "#define FRAGMENT\n#define PARAMETER_UNIFORM\n";
   src[2] = body;
   glShaderSource(s, 3, src, NULL);
   glCompileShader(s);
   glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
   if (!ok) { char log[4096]; glGetShaderInfoLog(s, sizeof log, NULL, log); fprintf(stderr, "%s\n", log); die("compile"); }
   return s;
}

static GLuint program(const char *body)
{
   GLuint p = glCreateProgram();
   GLint ok;
   glAttachShader(p, compile(GL_VERTEX_SHADER, body));
   glAttachShader(p, compile(GL_FRAGMENT_SHADER, body));
   glBindAttribLocation(p, 0, "VertexCoord");
   glBindAttribLocation(p, 1, "TexCoord");
   glLinkProgram(p);
   glGetProgramiv(p, GL_LINK_STATUS, &ok);
   if (!ok) { char log[4096]; glGetProgramInfoLog(p, sizeof log, NULL, log); fprintf(stderr, "%s\n", log); die("link"); }
   return p;
}

static unsigned char *read_ppm(const char *path, int *w, int *h)
{
   FILE *f = fopen(path, "rb");
   int max;
   unsigned char *rgb, *rgba;
   int i;
   if (!f || fscanf(f, "P6 %d %d %d", w, h, &max) != 3) die("bad ppm");
   fgetc(f);
   rgb = malloc(*w * *h * 3);
   if (fread(rgb, 3, *w * *h, f) != (size_t)(*w * *h)) die("short ppm");
   fclose(f);
   rgba = malloc(*w * *h * 4);
   for (i = 0; i < *w * *h; i++) { memcpy(rgba + i * 4, rgb + i * 3, 3); rgba[i * 4 + 3] = 255; }
   free(rgb);
   return rgba;
}

static void write_ppm(const char *path, int x, int y, int w, int h)
{
   unsigned char *px = malloc(w * h * 4);
   FILE *f = fopen(path, "wb");
   int r, i;
   glReadPixels(x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
   fprintf(f, "P6 %d %d 255\n", w, h);
   for (r = h - 1; r >= 0; r--)
      for (i = 0; i < w; i++) fwrite(px + (r * w + i) * 4, 1, 3, f);
   fclose(f);
   free(px);
}

static void uniforms(GLuint p, float ow, float oh, float iw, float ih, float tw, float th, const float *par)
{
   static const char *names[5] = { "CRT_SCAN", "CRT_MASK", "CRT_CURV", "CRT_HALO", "CRT_VIG" };
   static const float mvp[16] = { 2, 0, 0, 0, 0, 2, 0, 0, 0, 0, -1, 0, -1, -1, 0, 1 };   /* ortho 0..1 */
   int i;
   glUseProgram(p);
   glUniformMatrix4fv(glGetUniformLocation(p, "MVPMatrix"), 1, GL_FALSE, mvp);
   glUniform2f(glGetUniformLocation(p, "OutputSize"), ow, oh);
   glUniform2f(glGetUniformLocation(p, "InputSize"), iw, ih);
   glUniform2f(glGetUniformLocation(p, "TextureSize"), tw, th);
   glUniform1i(glGetUniformLocation(p, "Texture"), 0);
   for (i = 0; i < 5; i++) glUniform1f(glGetUniformLocation(p, names[i]), par[i]);
}

static void quad(GLuint vbo, float xamt, float yamt, int fbo_src)
{
   /* RetroArch's quad: vertices 0..1, texture coordinates 0..span; the core's frame has
    * its top row at t = 0, an FBO its bottom row */
   const float y0 = fbo_src ? 0 : yamt, y1 = fbo_src ? yamt : 0;
   const float v[16] = { 0, 0, 0, y0,  1, 0, xamt, y0,  0, 1, 0, y1,  1, 1, xamt, y1 };
   glBindBuffer(GL_ARRAY_BUFFER, vbo);
   glBufferData(GL_ARRAY_BUFFER, sizeof v, v, GL_STREAM_DRAW);
   glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, (void *)0);
   glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, (void *)8);
   glEnableVertexAttribArray(0);
   glEnableVertexAttribArray(1);
   glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static void dash_uniforms(GLuint p, const float *par)
{
   const float w = SCR_W, h = SCR_H, per = par[0] > 0.5f ? 3.0f : 2.0f;   /* soft 2, strong 3 (720p) */
   const float gain = powf(1.0f / ((1.0f - 0.35f * par[0]) * (1.0f - 0.667f * par[1])), 0.85f);
   const float rad = h * (0.012f + par[2] * 0.6f);
   glUseProgram(p);
   glUniform1i(glGetUniformLocation(p, "uTex"), 0);
   /* 0.20.0: uRes, uA (scan mask curv halo), uB (vig lines gain phase) */
   glUniform4f(glGetUniformLocation(p, "uRes"), w, h, rad, 2.6f / 1280.0f);
   glUniform4f(glGetUniformLocation(p, "uA"), par[0], par[1], par[2], par[3]);
   glUniform4f(glGetUniformLocation(p, "uB"), par[4], h / per, gain, 0.5f - 0.5f / per);
}

/* what src/crt.c needs from Lumen */
#include <stdarg.h>
#include "crt.h"
static const char *g_level = "soft";
void logf_(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); fputc('\n', stderr); }
const char *settings_get(const char *key, const char *def)
{
   if (!strcmp(key, "crt")) return g_level;
   if (!strcmp(key, "crt_pass") && getenv("CRT_BENCH_MESH")) return "mesh";
   return def;
}

static GLuint dash_program(const char *prefix)
{
   char path[512];
   GLuint p = glCreateProgram(), s[2];
   GLenum types[2] = { GL_VERTEX_SHADER, GL_FRAGMENT_SHADER };
   GLint ok;
   int i;
   for (i = 0; i < 2; i++)
   {
      const char *src;
      snprintf(path, sizeof path, "%s.%s", prefix, i ? "fs" : "vs");
      src = slurp(path);
      s[i] = glCreateShader(types[i]);
      glShaderSource(s[i], 1, &src, NULL);
      glCompileShader(s[i]);
      glGetShaderiv(s[i], GL_COMPILE_STATUS, &ok);
      if (!ok) { char log[4096]; glGetShaderInfoLog(s[i], sizeof log, NULL, log); fprintf(stderr, "%s\n", log); die("dash compile"); }
      glAttachShader(p, s[i]);
   }
   glLinkProgram(p);
   glGetProgramiv(p, GL_LINK_STATUS, &ok);
   if (!ok) die("dash link");
   return p;
}

static double now(void)
{
   struct timespec t;
   clock_gettime(CLOCK_MONOTONIC, &t);
   return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

int main(int argc, char **argv)
{
   EGLDisplay dpy;
   EGLConfig cfg;
   EGLContext ctx;
   EGLint n;
   static const EGLint cfg_attr[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
      EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE };
   static const EGLint ctx_attr[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE };
   const char *mode;
   int live, frames, iw, ih, tw = 1024, th = 512, vpw, vpx, run, f;
   float par[5], xamt, yamt;
   unsigned char *src;
   GLuint crt = 0, stock, vao, vbo, srctex, scr, scrtex, fbo = 0, fbotex = 0;
   double best = 1e30, pass_best = 1e30;
   const int pass_only = getenv("CRT_BENCH_PASS") != NULL;

   if (argc != 7) die("usage: crt-bench fbo|direct|none SHADER.glsl|- SRC.ppm OUT.ppm FRAMES \"SCAN MASK CURV HALO VIG\"");
   mode = argv[1];
   frames = atoi(argv[5]);
   if (sscanf(argv[6], "%f %f %f %f %f", &par[0], &par[1], &par[2], &par[3], &par[4]) != 5) die("params");
   src = read_ppm(argv[3], &iw, &ih);
   if (iw > 512) { tw = 1024; th = 512; }
   if (iw > 1024 || ih > 512) { tw = 2048; th = 1024; }
   xamt = (float)iw / tw; yamt = (float)ih / th;
   vpw = SCR_H * 4 / 3; vpx = (SCR_W - vpw) / 2;   /* 4:3 viewport, pillarboxed */
   if (!strcmp(argv[1], "dash") || !strcmp(argv[1], "live")) { vpw = SCR_W; vpx = 0; tw = SCR_W; th = SCR_H; }

   dpy = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
   if (!eglInitialize(dpy, NULL, NULL)) die("egl init");
   eglBindAPI(EGL_OPENGL_ES_API);
   if (!eglChooseConfig(dpy, cfg_attr, &cfg, 1, &n) || !n) die("egl config");
   ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
   if (!strcmp(argv[1], "live"))
   {  /* src/crt.c draws to the window: a pbuffer the screen's size */
      const EGLint pb_attr[] = { EGL_WIDTH, SCR_W, EGL_HEIGHT, SCR_H, EGL_NONE };
      EGLSurface pb = eglCreatePbufferSurface(dpy, cfg, pb_attr);
      if (pb == EGL_NO_SURFACE || !eglMakeCurrent(dpy, pb, pb, ctx)) die("egl pbuffer");
   }
   else if (!eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) die("egl current");

   stock = program(STOCK);
   live = !strcmp(mode, "live");
   if (live) g_level = par[0] > 0.5f ? "strong" : "soft";
   else if (!strcmp(mode, "dash")) crt = dash_program(argv[2]);
   else if (strcmp(mode, "none")) crt = program(slurp(argv[2]));
   glGenVertexArrays(1, &vao); glBindVertexArray(vao);
   glGenBuffers(1, &vbo);

   glGenTextures(1, &srctex);   /* the core's frame (RetroArch: filter_linear0 = true) */
   glBindTexture(GL_TEXTURE_2D, srctex);
   glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tw, th, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

   glGenTextures(1, &scrtex);   /* the "back buffer" */
   glBindTexture(GL_TEXTURE_2D, scrtex);
   glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, SCR_W, SCR_H);
   glGenFramebuffers(1, &scr);
   glBindFramebuffer(GL_FRAMEBUFFER, scr);
   glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, scrtex, 0);

   if (!strcmp(mode, "fbo"))
   {  /* scale_type0 = viewport: an RGBA8 target the viewport's size */
      glGenTextures(1, &fbotex);
      glBindTexture(GL_TEXTURE_2D, fbotex);
      glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, vpw, SCR_H);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      glGenFramebuffers(1, &fbo);
      glBindFramebuffer(GL_FRAMEBUFFER, fbo);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fbotex, 0);
   }

   for (run = 0; run < 4; run++)   /* run 0 warms up */
   {
      double t0;
      glFinish();
      t0 = now();
      for (f = 0; f < frames; f++)
      {
         glActiveTexture(GL_TEXTURE0);
         glBindTexture(GL_TEXTURE_2D, srctex);
         if ((strcmp(mode, "dash") && !live) || (run == 0 && f == 0))   /* the dashboard draws its scene itself */
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, iw, ih, GL_RGBA, GL_UNSIGNED_BYTE, src);
         if (live)
         {
            double t1;
            if (run == 0 && f == 0)
            {  /* the scene, once: SRC drawn into crt.c's target */
               crt_begin(SCR_W, SCR_H);
               glViewport(0, 0, SCR_W, SCR_H);
               uniforms(stock, SCR_W, SCR_H, iw, ih, tw, th, par);
               quad(vbo, 1, 1, 1);
               glBindVertexArray(vao);
            }
            glBindFramebuffer(GL_FRAMEBUFFER, crt_target());
            if (pass_only) { glFinish(); t1 = now(); }
            crt_end();
            if (pass_only) { glFinish(); if (run && now() - t1 < pass_best) pass_best = now() - t1; }
            glBindVertexArray(vao);
         }
         else if (!strcmp(mode, "dash"))
         {  /* the scene's target is already drawn: one triangle over the window */
            glBindFramebuffer(GL_FRAMEBUFFER, scr);
            glViewport(0, 0, SCR_W, SCR_H);
            double t1;
            dash_uniforms(crt, par);
            if (pass_only) { glFinish(); t1 = now(); }
            glDrawArrays(GL_TRIANGLES, 0, 3);
            if (pass_only) { glFinish(); if (run && now() - t1 < pass_best) pass_best = now() - t1; }
         }
         else if (fbo)
         {
            double t1, t2;
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glViewport(0, 0, vpw, SCR_H);
            uniforms(crt, vpw, SCR_H, iw, ih, tw, th, par);
            if (pass_only) { glFinish(); t1 = now(); }
            quad(vbo, xamt, yamt, 0);
            if (pass_only) { glFinish(); t2 = now() - t1; }
            glBindFramebuffer(GL_FRAMEBUFFER, scr);
            glClear(GL_COLOR_BUFFER_BIT);
            glViewport(vpx, 0, vpw, SCR_H);
            glBindTexture(GL_TEXTURE_2D, fbotex);
            uniforms(stock, vpw, SCR_H, vpw, SCR_H, vpw, SCR_H, par);
            if (pass_only) { glFinish(); t1 = now(); }
            quad(vbo, 1, 1, 1);
            if (pass_only) { glFinish(); t2 += now() - t1; if (run && t2 < pass_best) pass_best = t2; }
         }
         else
         {
            double t1;
            glBindFramebuffer(GL_FRAMEBUFFER, scr);
            glClear(GL_COLOR_BUFFER_BIT);
            glViewport(vpx, 0, vpw, SCR_H);
            uniforms(crt ? crt : stock, vpw, SCR_H, iw, ih, tw, th, par);
            if (pass_only) { glFinish(); t1 = now(); }
            quad(vbo, xamt, yamt, 0);
            if (pass_only) { glFinish(); if (run && now() - t1 < pass_best) pass_best = now() - t1; }
         }
         glFlush();
      }
      glFinish();
      if (run && now() - t0 < best) best = now() - t0;
   }
   /* CRT_BENCH_PASS=1: the fastest single pass alone (no upload, no clear): the number to
    * compare shader variants with; the default is the whole frame, as RetroArch draws it */
   printf("%.3f\n", pass_only ? pass_best : best / frames);
   glBindFramebuffer(GL_FRAMEBUFFER, live ? 0 : scr);
   write_ppm(argv[4], vpx, 0, vpw, SCR_H);
   return 0;
}
