/* Lumen CRT (RetroArch GLSL, gl driver: GLSL ES 3.00 on GLES 3, 1.00 on GLES 2).
 * The look of Lumen's dashboard filter (src/crt.c) on the game image: gaussian
 * scanlines (wider on bright pixels), aperture grille, slight curvature with rounded
 * edges, halation and vignette. Scanlines follow the core's lines: 224/240p are drawn
 * line by line; cores that render above 300 lines (480i, Mupen64Plus-Next and PPSSPP at
 * their enhanced resolutions) get the same density by halving, so they keep their detail.
 *
 * Built for the console's PowerVR GE8300 (one pass straight to the screen, no FBO):
 *   - everything that only depends on the parameters and the sizes (texture span, line
 *     count, brightness gain, mask levels, corner radius) is worked out per vertex and
 *     handed over as flat varyings: the pixel does no log2/exp2/pow and no divisions;
 *   - 1 bilinear fetch per pixel: the halation (a blur under one source pixel in the
 *     4-tap version) glows from the line's own colour;
 *   - the gaussian beam is (1 - x/8)^8 (3 squarings, no exp, no division; the width's
 *     1/s^2 is a quadratic in the brightness);
 *   - vignette, edge and beam fold into one scalar; colour maths in mediump (FP16 runs at
 *     twice the rate on Rogue), only the coordinates are highp; one sqrt (the rounded edge).
 * Measured with tools/crt-bench (RetroArch's draw path on psc-sim's llvmpipe): the filter
 * costs about 1/5 of the previous version's time. Its look stays within ~40 dB PSNR of it.
 * The previous, heavier maths (5 fetches, all highp) is kept as crt-lumen-quality.glsl.
 * Presets: crt-lumen-soft.glslp, crt-lumen-strong.glslp (parameters below). */
#version 130

#pragma parameter CRT_SCAN "CRT scanlines" 0.30 0.0 1.0 0.05
#pragma parameter CRT_MASK "CRT aperture grille" 0.16 0.0 1.0 0.02
#pragma parameter CRT_CURV "CRT curvature" 0.022 0.0 0.10 0.002
#pragma parameter CRT_HALO "CRT halation" 0.14 0.0 0.5 0.02
#pragma parameter CRT_VIG "CRT vignette" 0.22 0.0 1.0 0.02

#if __VERSION__ >= 300
#define COMPAT_FLAT flat
#else
#define COMPAT_FLAT
#endif

#if defined(VERTEX)

#if __VERSION__ >= 130
#define COMPAT_VARYING out
#define COMPAT_ATTRIBUTE in
#else
#define COMPAT_VARYING varying
#define COMPAT_ATTRIBUTE attribute
#endif

COMPAT_ATTRIBUTE vec4 VertexCoord;
COMPAT_ATTRIBUTE vec4 TexCoord;
COMPAT_VARYING vec2 vP;                 /* the screen, -1..1 before the curvature */
COMPAT_FLAT COMPAT_VARYING vec4 vSpan;  /* half the image's span in the texture (xy), line count / 2, span.y / line count */
COMPAT_FLAT COMPAT_VARYING vec4 vEdge;  /* half the output (px), its minus the corner radius */
COMPAT_FLAT COMPAT_VARYING vec4 vK;     /* mask: bright level, bright - dim; native lines (0/1); edge: 0.5 + radius / 1.5 */
uniform mat4 MVPMatrix;
uniform vec2 OutputSize;
uniform vec2 TextureSize;
uniform vec2 InputSize;

#ifdef PARAMETER_UNIFORM
uniform float CRT_SCAN;
uniform float CRT_MASK;
uniform float CRT_CURV;
#else
#define CRT_SCAN 0.30
#define CRT_MASK 0.16
#define CRT_CURV 0.022
#endif

void main()
{
   gl_Position = MVPMatrix * VertexCoord;
   vec2 span = InputSize / TextureSize;          /* the image inside the (padded) texture */
   vP = TexCoord.xy / span * 2.0 - 1.0;
   /* virtual lines: the core's own up to 300, else halved until they fit */
   float lines = InputSize.y / exp2(max(ceil(log2(InputSize.y / 300.0)), 0.0));
   vSpan = vec4(span * 0.5, lines * 0.5, span.y / lines);
   vec2 hs = OutputSize * 0.5;
   float rad = OutputSize.y * (0.012 + CRT_CURV * 0.6);
   vEdge = vec4(hs, hs - rad);
   /* the mask and the scanlines take light away: win most of it back (bright parts clip) */
   float gain = pow(1.0 / ((1.0 - 0.35 * CRT_SCAN) * (1.0 - 0.667 * CRT_MASK)), 0.85);
   vK = vec4(gain, gain * CRT_MASK, lines < InputSize.y - 0.5 ? 0.0 : 1.0, 0.5 + rad / 1.5);
}

#elif defined(FRAGMENT)

#ifdef GL_ES
precision mediump float;
#if defined(GL_FRAGMENT_PRECISION_HIGH) || __VERSION__ >= 300
#define HP highp
#else
#define HP mediump
#endif
#else
#define HP
#endif

#if __VERSION__ >= 130
#define COMPAT_VARYING in
#define COMPAT_TEXTURE texture
out vec4 FragColor;
#else
#define COMPAT_VARYING varying
#define FragColor gl_FragColor
#define COMPAT_TEXTURE texture2D
#endif

uniform sampler2D Texture;
COMPAT_VARYING HP vec2 vP;
COMPAT_FLAT COMPAT_VARYING HP vec4 vSpan;
COMPAT_FLAT COMPAT_VARYING HP vec4 vEdge;
COMPAT_FLAT COMPAT_VARYING HP vec4 vK;

#ifdef PARAMETER_UNIFORM
uniform HP float CRT_SCAN;   /* same precision as in the vertex shader (they must match) */
uniform HP float CRT_CURV;
uniform HP float CRT_HALO;
uniform HP float CRT_VIG;
#else
#define CRT_SCAN 0.30
#define CRT_CURV 0.022
#define CRT_HALO 0.14
#define CRT_VIG 0.22
#endif

void main()
{
   HP vec2 p = vP;
   p *= 1.0 + CRT_CURV * p.yx * p.yx;            /* barrel: corners curve away */
   HP vec2 t = p * vSpan.xy + vSpan.xy;          /* texture coordinates */
   HP float yl = p.y * vSpan.z + vSpan.z;        /* virtual line */
   HP float fl = floor(yl);
   HP vec2 tc = vec2(t.x, mix(t.y, (fl + 0.5) * vSpan.w, vK.z));   /* native: the line's centre */
   vec3 c = COMPAT_TEXTURE(Texture, tc).rgb;

   float f = yl - fl - 0.5;
   float l = dot(c, vec3(0.299, 0.587, 0.114));
   float g = max(1.0 - f * f * (17.36 + l * (-21.4 + 9.2 * l)) * (1.0 / 16.0), 0.0);
   g *= g; g *= g; g *= g;                       /* (1 - x/8)^8 ~ exp(-x), x = f^2 / 2s^2 */
   /* vignette and the tube's rounded edge (1.5 px antialiased): one scalar */
   float r2 = dot(p, p) * 0.5;
   HP vec2 q = max(abs(p) * vEdge.xy - vEdge.zw, 0.0);
   float k = (1.0 - CRT_VIG * r2 * r2) * clamp(vK.w - length(q) * (1.0 / 1.5), 0.0, 1.0);
   /* aperture grille: R, G, B stripes one device pixel wide (phase 1/6, 1/2, 5/6) */
   HP float mx = fract(gl_FragCoord.x * (1.0 / 3.0));
   vec3 m = vK.x - vK.y * step(0.25, abs(vec3(1.0 / 6.0, 0.5, 5.0 / 6.0) - mx));
   float bk = (1.0 - CRT_SCAN + CRT_SCAN * g) * k;
   c = c * (m * bk + c * (CRT_HALO * k));       /* halation (the line's colour) glows over the mask */
   FragColor = vec4(c, 1.0);
}
#endif
