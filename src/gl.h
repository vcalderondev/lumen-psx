/* Small GL/maths helpers for Lumen (OpenGL ES 3.0). */
#ifndef LUMEN_GL_H
#define LUMEN_GL_H

#include <GLES3/gl3.h>
#include <math.h>
#include <string.h>

typedef struct { float m[16]; } mat4;

static inline mat4 m4_identity(void)
{
   mat4 r; memset(&r, 0, sizeof(r));
   r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
   return r;
}

static inline mat4 m4_mul(mat4 a, mat4 b) /* a * b, column-major */
{
   mat4 r; int c, k, i;
   for (c = 0; c < 4; c++)
      for (i = 0; i < 4; i++)
      {
         float s = 0.0f;
         for (k = 0; k < 4; k++)
            s += a.m[k * 4 + i] * b.m[c * 4 + k];
         r.m[c * 4 + i] = s;
      }
   return r;
}

static inline mat4 m4_translate(float x, float y, float z)
{
   mat4 r = m4_identity();
   r.m[12] = x; r.m[13] = y; r.m[14] = z;
   return r;
}

static inline mat4 m4_scale(float x, float y, float z)
{
   mat4 r = m4_identity();
   r.m[0] = x; r.m[5] = y; r.m[10] = z;
   return r;
}

static inline mat4 m4_ortho(float l, float r_, float b, float t)
{
   mat4 r = m4_identity();
   r.m[0]  = 2.0f / (r_ - l);
   r.m[5]  = 2.0f / (t - b);
   r.m[10] = -1.0f;
   r.m[12] = -(r_ + l) / (r_ - l);
   r.m[13] = -(t + b) / (t - b);
   return r;
}

static inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* Damped spring: slightly under-damped, gives the small overshoot ("bounce"). */
typedef struct { float x, v; } spring;

static inline void spring_step(spring *s, float target, float k, float c, float dt)
{
   float a = (target - s->x) * k - s->v * c;
   s->v += a * dt;
   s->x += s->v * dt;
}

#endif
