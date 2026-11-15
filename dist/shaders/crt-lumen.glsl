/* Lumen CRT (RetroArch GLSL, gl driver: GLSL ES 3.00 on GLES 3, 1.00 on GLES 2).
 * The look of Lumen's dashboard filter (src/crt.c) on the game image: gaussian
 * scanlines (wider on bright pixels), aperture grille, slight curvature with rounded
 * edges, halation and vignette. One pass, 5 bilinear fetches per pixel.
 * Scanlines follow the core's lines: 224/240p are drawn line by line; cores that render
 * above 300 lines (480i, Mupen64Plus-Next and PPSSPP at their enhanced resolutions) get
 * the same density by halving, so they keep their detail.
 * Presets: crt-lumen-soft.glslp, crt-lumen-strong.glslp (parameters below). */
#version 130

#pragma parameter CRT_SCAN "CRT scanlines" 0.30 0.0 1.0 0.05
#pragma parameter CRT_MASK "CRT aperture grille" 0.16 0.0 1.0 0.02
#pragma parameter CRT_CURV "CRT curvature" 0.022 0.0 0.10 0.002
#pragma parameter CRT_HALO "CRT halation" 0.14 0.0 0.5 0.02
#pragma parameter CRT_VIG "CRT vignette" 0.22 0.0 1.0 0.02

#if defined(VERTEX)

#if __VERSION__ >= 130
#define COMPAT_VARYING out
#define COMPAT_ATTRIBUTE in
#else
#define COMPAT_VARYING varying
#define COMPAT_ATTRIBUTE attribute
#endif
#ifdef GL_ES
#define COMPAT_PRECISION mediump
#else
#define COMPAT_PRECISION
#endif

COMPAT_ATTRIBUTE vec4 VertexCoord;
COMPAT_ATTRIBUTE vec4 TexCoord;
COMPAT_VARYING vec4 TEX0;
uniform mat4 MVPMatrix;

void main()
{
   gl_Position = MVPMatrix * VertexCoord;
   TEX0.xy = TexCoord.xy;
}

#elif defined(FRAGMENT)

#ifdef GL_ES
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif
#define COMPAT_PRECISION mediump
#else
#define COMPAT_PRECISION
#endif

#if __VERSION__ >= 130
#define COMPAT_VARYING in
#define COMPAT_TEXTURE texture
out COMPAT_PRECISION vec4 FragColor;
#else
#define COMPAT_VARYING varying
#define FragColor gl_FragColor
#define COMPAT_TEXTURE texture2D
#endif

uniform vec2 OutputSize;
uniform vec2 TextureSize;
uniform vec2 InputSize;
uniform sampler2D Texture;
COMPAT_VARYING vec4 TEX0;

#ifdef PARAMETER_UNIFORM
uniform COMPAT_PRECISION float CRT_SCAN;
uniform COMPAT_PRECISION float CRT_MASK;
uniform COMPAT_PRECISION float CRT_CURV;
uniform COMPAT_PRECISION float CRT_HALO;
uniform COMPAT_PRECISION float CRT_VIG;
#else
#define CRT_SCAN 0.30
#define CRT_MASK 0.16
#define CRT_CURV 0.022
#define CRT_HALO 0.14
#define CRT_VIG 0.22
#endif

void main()
{
   vec2 span = InputSize / TextureSize;          /* the image inside the (padded) texture */
   vec2 p = TEX0.xy / span * 2.0 - 1.0;
   p *= 1.0 + CRT_CURV * p.yx * p.yx;            /* barrel: corners curve away */
   vec2 uv = p * 0.5 + 0.5;

   /* virtual lines: the core's own up to 300, else halved until they fit */
   float lines = InputSize.y / exp2(max(ceil(log2(InputSize.y / 300.0)), 0.0));
   float yl = uv.y * lines;
   vec2 suv = uv;
   if (lines == InputSize.y) suv.y = (floor(yl) + 0.5) / lines;   /* native: sample the line's centre */

   vec2 o = vec2(2.6 / 1280.0, 2.6 / 720.0) * span;
   vec3 c = COMPAT_TEXTURE(Texture, suv * span).rgb;
   vec3 h = COMPAT_TEXTURE(Texture, uv * span + vec2(o.x, o.y)).rgb + COMPAT_TEXTURE(Texture, uv * span + vec2(-o.x, o.y)).rgb
          + COMPAT_TEXTURE(Texture, uv * span + vec2(o.x, -o.y)).rgb + COMPAT_TEXTURE(Texture, uv * span + vec2(-o.x, -o.y)).rgb;
   h *= 0.25;

   float f = fract(yl) - 0.5;
   float l = dot(c, vec3(0.299, 0.587, 0.114));
   float s = mix(0.24, 0.44, l);
   c *= mix(1.0, exp(-0.5 * f * f / (s * s)), CRT_SCAN);

   float mx = mod(floor(gl_FragCoord.x), 3.0);
   vec3 m = mix(vec3(1.0 - CRT_MASK), vec3(1.0), vec3(equal(vec3(mx), vec3(0.0, 1.0, 2.0))));
   float gain = pow(1.0 / ((1.0 - 0.35 * CRT_SCAN) * (1.0 - 0.667 * CRT_MASK)), 0.85);
   c = c * m * gain + h * h * CRT_HALO;

   float r2 = dot(p, p) * 0.5;
   c *= 1.0 - CRT_VIG * r2 * r2;
   vec2 hs = OutputSize * 0.5;
   float rad = OutputSize.y * (0.012 + CRT_CURV * 0.6);
   vec2 q = max(abs(p) * hs - (hs - rad), 0.0);
   c *= clamp(0.5 - (length(q) - rad) / 1.5, 0.0, 1.0);
   FragColor = vec4(c, 1.0);
}
#endif
