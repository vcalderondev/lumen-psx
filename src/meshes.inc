/* Lumen — modelled hardware: consoles, cartridges and cases built in Blender
 * (tools/models/, one Python script per model), optimised by tools/models/optimize.py and
 * loaded from <home>/assets/models/<key>.lmdl. Included by main.c. A missing model falls
 * back to the procedural one (consoles.inc).
 *
 * .lmdl v2, little endian, millimetres, y up, the model's front towards +z, triangles
 * counter-clockwise seen from outside:
 *   "LMDL", u32 version = 2, u32 nparts, u32 nverts (<= 65535), u32 nindices, then
 *   nparts x { char name[48]; float rgba[4]; float on[4]; float gloss, spec;
 *              u32 kind, anim, chan; float pivot[3], axis[3]; float amount;
 *              u32 first, count; }                       (a range of the index buffer)
 *   nverts x { float pos[3]; i8 normal[3], pad; u16 uv[2]; }   (normalised)
 *   nindices x u16
 *   kind: 0 plastic, 1 label (the game's art x rgba), 2 screen (rgba off; the game's art
 *         x on when lit), 3 light (rgba off, on when lit, plus a glow), 4 glass
 *         (translucent, drawn last), 5 dark (unlit: inside slots), 6 back (the case's
 *         back card), 7 spine (the printed spine)
 *   anim: 0 none, 1 rotate about pivot / axis by amount degrees x channel, 2 move along
 *         axis by amount mm x channel
 *   chan: 1 open (dust flaps, case lid), 2 door, 3 power (switch, button), 4 eject
 * (v1, what Blender exports, is a triangle soup per object: optimize.py reads it.) */

static void glow_at(mat4 c, float x, float y, float z, int up, float size, const float col[3], float a);

enum { MK_PLASTIC, MK_LABEL, MK_SCREEN, MK_LIGHT, MK_GLASS, MK_DARK, MK_BACK, MK_SPINE };
enum { MA_NONE, MA_ROT, MA_MOVE };
enum { MC_NONE, MC_OPEN, MC_DOOR, MC_POWER, MC_EJECT, MC_COUNT };

typedef struct {
   char name[48];
   float rgba[4], on[4], gloss, spec, pivot[3], axis[3], amount;
   unsigned kind, anim, chan;
   int first, count;          /* index range in the model's buffer */
} mpart_t;

typedef struct {
   char key[32];
   int state;                 /* 0 not tried, 1 loaded, -1 missing / bad */
   GLuint vao, vbo, ibo;
   int nparts, nverts, nindices;
   mpart_t *part;
   float lo[3], hi[3];        /* bounds (mm) */
   unsigned char *cvert;      /* cost meter only: the vertices and indices, kept */
   unsigned short *cidx;
} model_t;

/* pose: the animated channels (0..1) and what lights up */
typedef struct { float ch[MC_COUNT]; float light, screen; } mpose_t;

#define MAX_MODELS 32
static model_t g_models[MAX_MODELS];
static int g_nmodels;

#define MDL_VSIZE 20
#define MDL_PSIZE 136
static int mdl_load(model_t *m)
{
   char path[400];
   FILE *f;
   unsigned hdr[5], k;
   unsigned char *verts = NULL, *praw = NULL;
   unsigned short *idx = NULL;
   size_t nv, ni, i;
   const double t0 = now_s();
   double t1;
   snprintf(path, sizeof(path), "%s/assets/models/%s.lmdl", g_home, m->key);
   if (!(f = fopen(path, "rb"))) return -1;
   if (fread(hdr, 4, 5, f) != 5 || memcmp(hdr, "LMDL", 4) || hdr[1] != 2 || hdr[2] == 0 || hdr[2] > 512 ||
       hdr[3] == 0 || hdr[3] > 65535 || hdr[4] == 0 || hdr[4] > 3000000 || hdr[4] % 3)
   { fclose(f); logf_("model %s: bad header (not a v2 model: tools/models/optimize.py)", m->key); return -1; }
   m->nparts = (int)hdr[2]; nv = hdr[3]; ni = hdr[4];
   m->part = calloc(m->nparts, sizeof(mpart_t));
   praw = malloc((size_t)m->nparts * MDL_PSIZE);
   verts = malloc(nv * MDL_VSIZE);
   idx = malloc(ni * 2);
   /* three reads: the part table, the vertex buffer and the index buffer as they are drawn */
   if (!m->part || !praw || !verts || !idx || fread(praw, MDL_PSIZE, m->nparts, f) != (size_t)m->nparts ||
       fread(verts, MDL_VSIZE, nv, f) != nv || fread(idx, 2, ni, f) != ni)
   {
      fclose(f);
      logf_("model %s: truncated", m->key);
      goto bad;
   }
   fclose(f);
   for (k = 0; k < (unsigned)m->nparts; k++)
   {
      mpart_t *p = &m->part[k];
      const unsigned char *r = praw + (size_t)k * MDL_PSIZE;
      unsigned u[5];
      memcpy(p->name, r, 48); p->name[47] = 0;
      memcpy(p->rgba, r + 48, 16); memcpy(p->on, r + 64, 16);
      memcpy(&p->gloss, r + 80, 4); memcpy(&p->spec, r + 84, 4);
      memcpy(u, r + 88, 12);
      memcpy(p->pivot, r + 100, 12); memcpy(p->axis, r + 112, 12); memcpy(&p->amount, r + 124, 4);
      memcpy(u + 3, r + 128, 8);
      p->kind = u[0]; p->anim = u[1]; p->chan = u[2] < MC_COUNT ? u[2] : 0;
      if (u[3] > ni || u[4] > ni - u[3] || u[4] % 3) { logf_("model %s: bad part %u", m->key, k); goto bad; }
      p->first = (int)u[3]; p->count = (int)u[4];
   }
   for (k = 0; k < 3; k++) { m->lo[k] = 1e9f; m->hi[k] = -1e9f; }
   for (i = 0; i < nv; i++)
   {
      float v[3];
      memcpy(v, verts + i * MDL_VSIZE, 12);
      for (k = 0; k < 3; k++) { if (v[k] < m->lo[k]) m->lo[k] = v[k]; if (v[k] > m->hi[k]) m->hi[k] = v[k]; }
   }
   for (i = 0; i < ni; i++) if (idx[i] >= nv) { logf_("model %s: bad index", m->key); goto bad; }
   m->nverts = (int)nv; m->nindices = (int)ni;
   t1 = now_s();
   glGenVertexArrays(1, &m->vao);
   glBindVertexArray(m->vao);
   glGenBuffers(1, &m->vbo);
   glBindBuffer(GL_ARRAY_BUFFER, m->vbo);
   glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(nv * MDL_VSIZE), verts, GL_STATIC_DRAW);
   glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, MDL_VSIZE, (void *)0);
   glVertexAttribPointer(1, 2, GL_UNSIGNED_SHORT, GL_TRUE, MDL_VSIZE, (void *)16);
   glVertexAttribPointer(2, 3, GL_BYTE, GL_TRUE, MDL_VSIZE, (void *)12);
   glEnableVertexAttribArray(0);
   glEnableVertexAttribArray(1);
   glEnableVertexAttribArray(2);
   glGenBuffers(1, &m->ibo);
   glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m->ibo);   /* kept by the vertex array */
   glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(ni * 2), idx, GL_STATIC_DRAW);
   glBindVertexArray(g_vao_quad);
   glBindBuffer(GL_ARRAY_BUFFER, g_vbo_quad);
   free(praw);
   if (getenv("LUMEN_COST")) { glFinish(); m->cvert = verts; m->cidx = idx; } else { free(verts); free(idx); }
   logf_("model %s: %d parts, %d triangles, %d vertices, %.0f x %.0f x %.0f mm, read %.1f ms, upload %.1f ms", m->key, m->nparts,
         m->nindices / 3, m->nverts, m->hi[0] - m->lo[0], m->hi[1] - m->lo[1], m->hi[2] - m->lo[2],
         (t1 - t0) * 1000.0, (now_s() - t1) * 1000.0);
   return 1;
bad:
   free(praw); free(verts); free(idx); free(m->part); m->part = NULL; m->nparts = 0;
   return -1;
}

/* the model for key, loaded on first use; NULL when there is none */
static model_t *mdl_get(const char *key)
{
   int i;
   for (i = 0; i < g_nmodels; i++)
      if (!strcmp(g_models[i].key, key)) return g_models[i].state > 0 ? &g_models[i] : NULL;
   if (g_nmodels >= MAX_MODELS) return NULL;
   snprintf(g_models[g_nmodels].key, sizeof(g_models[0].key), "%s", key);
   g_models[g_nmodels].state = mdl_load(&g_models[g_nmodels]);
   g_nmodels++;
   return g_models[g_nmodels - 1].state > 0 ? &g_models[g_nmodels - 1] : NULL;
}


/* cost meter for the meshes (LUMEN_COST=1): what a tile-based deferred renderer shades.
 * Blended triangles are all shaded (no hidden surface removal): the sum of their areas.
 * Opaque, depth-tested ones are shaded once per pixel: the union of what they cover,
 * kept in a quarter-resolution mask for the frame. Culled faces cost nothing. */
#define MC_W 480
#define MC_H 270
static unsigned char g_mc_mask[MC_W * MC_H];
static double g_mc_tris, g_mc_verts, g_mc_draws, g_mc_px_blend, g_mc_px_opaque;
static void mc_frame(void) { memset(g_mc_mask, 0, sizeof(g_mc_mask)); g_mc_tris = g_mc_verts = g_mc_draws = g_mc_px_blend = g_mc_px_opaque = 0; }
static void mc_tri(const float *a, const float *b, const float *c, int blend, int cull)
{
   float area = (b[0] - a[0]) * (c[1] - a[1]) - (c[0] - a[0]) * (b[1] - a[1]);   /* > 0: counter-clockwise */
   int x0, x1, y0, y1, x, y;
   if (cull && area * cull <= 0.0f) return;
   if (blend) { g_mc_px_blend += fabsf(area) * 0.5f * 16.0f; return; }
   x0 = (int)floorf(fminf(a[0], fminf(b[0], c[0]))); x1 = (int)ceilf(fmaxf(a[0], fmaxf(b[0], c[0])));
   y0 = (int)floorf(fminf(a[1], fminf(b[1], c[1]))); y1 = (int)ceilf(fmaxf(a[1], fmaxf(b[1], c[1])));
   if (x0 < 0) x0 = 0;
   if (y0 < 0) y0 = 0;
   if (x1 > MC_W - 1) x1 = MC_W - 1;
   if (y1 > MC_H - 1) y1 = MC_H - 1;
   for (y = y0; y <= y1; y++)
      for (x = x0; x <= x1; x++)
      {
         const float px = x + 0.5f, py = y + 0.5f;
         const float e0 = (b[0] - a[0]) * (py - a[1]) - (px - a[0]) * (b[1] - a[1]);
         const float e1 = (c[0] - b[0]) * (py - b[1]) - (px - b[0]) * (c[1] - b[1]);
         const float e2 = (a[0] - c[0]) * (py - c[1]) - (px - c[0]) * (a[1] - c[1]);
         if ((e0 >= 0 && e1 >= 0 && e2 >= 0) || (e0 <= 0 && e1 <= 0 && e2 <= 0))
            if (!g_mc_mask[y * MC_W + x]) { g_mc_mask[y * MC_W + x] = 1; g_mc_px_opaque += 16.0; }
      }
}
static void mc_part(const model_t *m, const mpart_t *p, const mat4 *mvp)
{
   const float *M = mvp->m;
   const int blend = glIsEnabled(GL_BLEND);
   int cull = 0, i, k;
   GLint ff = GL_CCW, cf = GL_BACK;
   if (!m->cvert) return;
   if (glIsEnabled(GL_CULL_FACE))
   {
      glGetIntegerv(GL_FRONT_FACE, &ff); glGetIntegerv(GL_CULL_FACE_MODE, &cf);
      cull = (ff == GL_CCW) == (cf == GL_BACK) ? 1 : -1;
   }
   g_mc_draws += 1; g_mc_tris += p->count / 3;
   {  /* vertices the draw runs through the vertex shader: the ones its indices name */
      static unsigned char seen[65536];
      memset(seen, 0, sizeof(seen));
      for (i = 0; i < p->count; i++) { const int x = m->cidx[p->first + i]; if (!seen[x]) { seen[x] = 1; g_mc_verts += 1; } }
   }
   for (i = 0; i + 2 < p->count; i += 3)
   {
      float s[3][2];
      for (k = 0; k < 3; k++)
      {
         float v[3], x, y, w;
         memcpy(v, m->cvert + (size_t)m->cidx[p->first + i + k] * MDL_VSIZE, 12);
         x = M[0] * v[0] + M[4] * v[1] + M[8] * v[2] + M[12]; y = M[1] * v[0] + M[5] * v[1] + M[9] * v[2] + M[13];
         w = M[3] * v[0] + M[7] * v[1] + M[11] * v[2] + M[15];
         if (w < 0.01f) break;
         s[k][0] = (clampf(x / w, -1.0f, 1.0f) * 0.5f + 0.5f) * MC_W; s[k][1] = (clampf(y / w, -1.0f, 1.0f) * 0.5f + 0.5f) * MC_H;
      }
      if (k == 3) mc_tri(s[0], s[1], s[2], blend, cull);
   }
}

/* rotation by a radians about a unit axis */
static mat4 m4_rot_axis(const float ax[3], float a)
{
   mat4 r = m4_identity();
   const float c = cosf(a), s = sinf(a), t = 1.0f - c, x = ax[0], y = ax[1], z = ax[2];
   r.m[0] = t * x * x + c;     r.m[4] = t * x * y - s * z; r.m[8]  = t * x * z + s * y;
   r.m[1] = t * x * y + s * z; r.m[5] = t * y * y + c;     r.m[9]  = t * y * z - s * x;
   r.m[2] = t * x * z - s * y; r.m[6] = t * y * z + s * x; r.m[10] = t * z * z + c;
   return r;
}

/* The modelled hardware has its own two small programs (flat colour / textured) instead
 * of the box shader's chain of modes: the lighting (sky / ground ambient, wrapped
 * diffuse, fill light, Fresnel) is worked out per vertex, and a fragment costs one
 * normalise and one pow (the Blinn highlight), plus the texture fetch where there is one.
 * mat: gloss exponent, specular strength, emissive (0 lit .. 1 unlit), Fresnel weight of
 * the highlight. refl_y < 50: the floor reflection (fades with the height below it). */
static const char *VS_MESH =
   "#version 300 es\n"
   "layout(location=0) in vec3 pos; layout(location=1) in vec2 uv; layout(location=2) in vec3 nrm;\n"
   "uniform mat4 mvp, model; uniform vec3 light, cam, tint; uniform mediump vec4 mat; uniform float alpha, refl_y;\n"
   "out vec2 v_uv; out vec3 v_n; out vec3 v_h; out vec3 v_lit; out vec3 v_sa;\n"
   "void main(){\n"
   "  vec3 n = normalize(mat3(model) * nrm);\n"
   "  vec3 w = (model * vec4(pos, 1.0)).xyz;\n"
   "  vec3 v = normalize(cam - w);\n"
   "  float wrap = max((dot(n, light) + 0.35) / 1.35, 0.0);\n"
   "  vec3 amb = mix(vec3(0.30, 0.29, 0.28), vec3(0.50, 0.54, 0.62), n.y * 0.5 + 0.5);\n"
   "  float fill = max(dot(n, vec3(0.7664, 0.2874, 0.5748)), 0.0) * 0.18;\n"
   "  float fr = 0.04 + 0.96 * pow(1.0 - max(dot(n, v), 0.0), 5.0);\n"
   "  float a = alpha;\n"
   "  if (refl_y < 50.0) { float k = clamp((w.y - (refl_y - 0.55)) / 0.55, 0.0, 1.0); a *= k * k * 0.26; }\n"
   "  v_lit = tint * mix(amb * 0.72 + wrap * 0.58 + fill, vec3(1.0), mat.z);\n"
   "  v_sa = vec3(mat.y * (0.6 + mat.w * fr), fr * 0.05 * (1.0 - mat.z), a);\n"
   "  v_uv = uv; v_n = n; v_h = normalize(light + v);\n"
   "  gl_Position = mvp * vec4(pos, 1.0);\n"
   "}\n";
static const char *FS_MESH_FLAT =
   "#version 300 es\n"
   "precision mediump float;\n"
   "in vec3 v_n; in vec3 v_h; in vec3 v_lit; in vec3 v_sa;\n"
   "uniform vec4 mat; out vec4 frag;\n"
   "void main(){ frag = vec4(v_lit + vec3(pow(max(dot(normalize(v_n), v_h), 0.0), mat.x) * v_sa.x + v_sa.y), v_sa.z); }\n";
static const char *FS_MESH_TEX =
   "#version 300 es\n"
   "precision mediump float;\n"
   "in vec2 v_uv; in vec3 v_n; in vec3 v_h; in vec3 v_lit; in vec3 v_sa;\n"
   "uniform sampler2D tex; uniform vec4 mat; out vec4 frag;\n"
   "void main(){ frag = vec4(texture(tex, v_uv).rgb * v_lit + vec3(pow(max(dot(normalize(v_n), v_h), 0.0), mat.x) * v_sa.x + v_sa.y), v_sa.z); }\n";

typedef struct { GLuint prog; GLint mvp, model, tint, mat, alpha, refl; } mprog_t;
static mprog_t g_mp[2];        /* 0 flat, 1 textured */
static float   g_mdl_zunits = -2.0f;   /* depth offset of printed parts, in units of the depth buffer */
static float   g_refl_y = 100.0f;   /* floor of the reflection pass (>= 50: off), see launch_refl() */

static void mdl_setup(void)
{
   /* the launch scene's light and camera never move (main loop, launch_warmup) */
   const float l[3] = { -0.5f, 0.7f, 1.0f }, cam[3] = { 0.0f, -0.22f, 4.7f };
   const float ll = sqrtf(l[0] * l[0] + l[1] * l[1] + l[2] * l[2]);
   GLint zbits = 16;
   int i;
   glGetIntegerv(GL_DEPTH_BITS, &zbits);
   g_mdl_zunits = -2.0f * (float)(1 << (zbits > 16 ? (zbits > 24 ? 24 : zbits) - 16 : 0));
   for (i = 0; i < 2; i++)
   {
      mprog_t *p = &g_mp[i];
      p->prog = link_prog(VS_MESH, i ? FS_MESH_TEX : FS_MESH_FLAT);
      p->mvp = glGetUniformLocation(p->prog, "mvp"); p->model = glGetUniformLocation(p->prog, "model");
      p->tint = glGetUniformLocation(p->prog, "tint"); p->mat = glGetUniformLocation(p->prog, "mat");
      p->alpha = glGetUniformLocation(p->prog, "alpha"); p->refl = glGetUniformLocation(p->prog, "refl_y");
      glUseProgram(p->prog);
      glUniform3f(glGetUniformLocation(p->prog, "light"), l[0] / ll, l[1] / ll, l[2] / ll);
      glUniform3fv(glGetUniformLocation(p->prog, "cam"), 1, cam);
      if (i) glUniform1i(glGetUniformLocation(p->prog, "tex"), 0);
   }
}

/* the reflection pass of the launch scene: both the box shader and the mesh programs fade
 * what they draw with the height below floor_y (>= 50: off) */
static void launch_refl(float floor_y)
{
   g_refl_y = floor_y;
   glUniform1f(u_refl, floor_y);   /* g_prog_box is the program in use in the 3D pass */
}

/* how a part is drawn: 0 / 1 the mesh programs (flat / textured), 2 the box shader (the
 * printed spine), and its tint, texture and material */
typedef struct { int prog; GLuint tex; float tint[3], mat[4], alpha; } mdraw_t;
static void mdl_part_look(const mpart_t *p, const game_t *g, const mpose_t *ps, float alpha, mdraw_t *d)
{
   int k;
   d->prog = 0; d->tex = 0; d->alpha = alpha * p->rgba[3];
   for (k = 0; k < 3; k++) d->tint[k] = p->rgba[k];
   d->mat[0] = p->gloss > 1.0f ? p->gloss : 24.0f; d->mat[1] = p->spec; d->mat[2] = 0.0f; d->mat[3] = 2.0f;
   switch (p->kind)
   {
   case MK_LABEL:
      if (g && g->tex) { d->tex = g->tex; d->prog = 1; }
      break;
   case MK_SCREEN:
   {
      const float on = ps ? ps->screen : 0.0f;
      d->mat[2] = 1.0f; d->mat[1] = 0.0f;
      if (on > 0.02f && g && g->tex)
      {  /* backlit art with a small sharp highlight */
         d->tex = g->tex; d->prog = 1;
         for (k = 0; k < 3; k++) d->tint[k] = p->on[k] * on;
         d->mat[0] = 200.0f; d->mat[1] = 0.25f / 0.6f; d->mat[3] = 0.0f;
      }
      break;
   }
   case MK_LIGHT:
   {
      const float on = ps ? ps->light : 0.0f;
      for (k = 0; k < 3; k++) d->tint[k] = lerpf(p->rgba[k], p->on[k], on);
      d->mat[2] = 1.0f; d->mat[1] = 0.0f;
      break;
   }
   case MK_DARK: d->mat[2] = 1.0f; d->mat[1] = 0.0f; break;
   case MK_BACK:
      if (g && g->back_tex) { d->tex = g->back_tex; d->prog = 1; d->tint[0] = d->tint[1] = d->tint[2] = 1.0f; }
      break;
   case MK_SPINE:
      if (g && g->spine_tex) { d->tex = g->spine_tex; d->prog = 2; d->tint[0] = d->tint[1] = d->tint[2] = 1.0f; }
      break;
   default: break;
   }
}

static mat4 mdl_part_matrix(const mpart_t *p, mat4 base, const mpose_t *ps)
{
   const float v = ps && p->chan ? ps->ch[p->chan] : 0.0f;
   if (p->anim == MA_ROT && v != 0.0f)
      return m4_mul(base, m4_mul(m4_translate(p->pivot[0], p->pivot[1], p->pivot[2]),
                    m4_mul(m4_rot_axis(p->axis, p->amount * v * (float)M_PI / 180.0f),
                           m4_translate(-p->pivot[0], -p->pivot[1], -p->pivot[2]))));
   if (p->anim == MA_MOVE && v != 0.0f)
      return m4_mul(base, m4_translate(p->axis[0] * p->amount * v, p->axis[1] * p->amount * v, p->axis[2] * p->amount * v));
   return base;
}

/* Draw a model in frame base (mm). Three passes, by what the GPU has to do for them:
 *   0 opaque parts with blending off and depth writes on: a tile-based GPU (the PSC's
 *     PowerVR) then shades each pixel once, whatever is stacked behind it (with blending
 *     on, as everything was drawn before, it shades every layer of every part);
 *   1 the printed spine (box shader), then the glow of the lights that are on;
 *   2 glass, blended, no depth writes.
 * A model that is fading (alpha < 1) or mirrored in the floor is blended as a whole. */
static void mdl_draw(const model_t *m, mat4 base, const game_t *g, const mpose_t *ps, float alpha)
{
   const float *b = base.m;
   const float det = b[0] * (b[5] * b[10] - b[9] * b[6]) - b[4] * (b[1] * b[10] - b[9] * b[2]) + b[8] * (b[1] * b[6] - b[5] * b[2]);
   const int refl = g_refl_y < 50.0f, fading = alpha < 0.995f || refl;
   int pass, i, cur = -1, glows = 0, printed;
   glBindVertexArray(m->vao);
   glFrontFace(det < 0.0f ? GL_CW : GL_CCW);   /* the reflection is a mirror image */
   for (pass = 0; pass < 3; pass++)
   {
      /* back faces: the depth test hides them for free in the opaque pass, the inside of
       * a slot is seen from behind and glass shows both its faces, so they are only
       * culled where every layer would be shaded and blended: in the floor reflection
       * (no depth test) and in the solid parts of a model that is fading in */
      if (pass == 0 && !fading) glDisable(GL_BLEND);
      if (refl || (fading && pass < 2)) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
      if (pass == 2)
      {
         if (glows)
         {  /* the glow of the lights that are on, at their pivots, facing the model's
             * front; under the glass (a lens over an LED dims it) */
            glDisable(GL_CULL_FACE);
            glFrontFace(GL_CCW);
            glUseProgram(g_prog_box); cur = 2;
            glBindVertexArray(g_vao_quad);
            for (i = 0; i < m->nparts; i++)
            {
               const mpart_t *p = &m->part[i];
               if (p->kind == MK_LIGHT) glow_at(base, p->pivot[0], p->pivot[1], p->pivot[2] + 0.6f, 0, 18.0f, p->on, 0.7f * ps->light);
            }
            glBindVertexArray(m->vao);
            glFrontFace(det < 0.0f ? GL_CW : GL_CCW);
            if (refl) glEnable(GL_CULL_FACE);
         }
         glDepthMask(GL_FALSE);
      }
      for (i = 0; i < m->nparts; i++)
      {
         const mpart_t *p = &m->part[i];
         mdraw_t d;
         mat4 model, mvp;
         mdl_part_look(p, g, ps, alpha, &d);
         if ((p->kind == MK_GLASS ? 2 : d.prog == 2 ? 1 : 0) != pass) continue;
         printed = p->kind == MK_LABEL || p->kind == MK_SCREEN || p->kind == MK_BACK || p->kind == MK_SPINE;
         if (printed)
         {  /* print lies in the plane of the paper or plastic behind it: seen from its own
             * side only, and nudged towards the eye by two steps of a 16-bit depth buffer
             * (some 0.06 mm of the model at the launch's distances: less than what lies
             * over it, a lens over a screen), so it never fights with them for the depth
             * test (stripes across the label, or the cover showing through inside) */
            glEnable(GL_CULL_FACE);
            glEnable(GL_POLYGON_OFFSET_FILL);
            glPolygonOffset(0.0f, g_mdl_zunits);
         }
         if (p->kind == MK_LIGHT && ps && ps->light > 0.0f) glows++;
         model = mdl_part_matrix(p, base, ps);
         mvp = m4_mul(g_vp, model);
         if (d.prog != cur)
         {
            cur = d.prog;
            glUseProgram(cur == 2 ? g_prog_box : g_mp[cur].prog);
            if (cur < 2) glUniform1f(g_mp[cur].refl, g_refl_y);
         }
         if (cur == 2)
         {
            glUniformMatrix4fv(u_mvp, 1, GL_FALSE, mvp.m);
            glUniformMatrix4fv(u_model, 1, GL_FALSE, model.m);
            glUniform3fv(u_tint, 1, d.tint);
            glUniform1f(u_alpha, d.alpha);
            glUniform1i(u_mode, 3);
            glBindTexture(GL_TEXTURE_2D, d.tex);
         }
         else
         {
            const mprog_t *mp = &g_mp[cur];
            glUniformMatrix4fv(mp->mvp, 1, GL_FALSE, mvp.m);
            glUniformMatrix4fv(mp->model, 1, GL_FALSE, model.m);
            glUniform3fv(mp->tint, 1, d.tint);
            glUniform4fv(mp->mat, 1, d.mat);
            glUniform1f(mp->alpha, d.alpha);
            if (cur == 1) glBindTexture(GL_TEXTURE_2D, d.tex);
         }
         if (g_cost_on) mc_part(m, p, &mvp);
         glDrawElements(GL_TRIANGLES, p->count, GL_UNSIGNED_SHORT, (void *)(size_t)(p->first * 2));
         if (printed)
         {
            glDisable(GL_POLYGON_OFFSET_FILL);
            if (!(refl || (fading && pass < 2))) glDisable(GL_CULL_FACE);
         }
      }
      if (pass == 0) glEnable(GL_BLEND);
   }
   glDepthMask(GL_TRUE);
   glDisable(GL_CULL_FACE);
   glFrontFace(GL_CCW);
   glUseProgram(g_prog_box);
   glBindVertexArray(g_vao_quad);
}

/* model keys: the cartridge / case of a shape, and the console it goes into */
static const char *mdl_media_key(int shape)
{
   switch (shape)
   {
   case SHAPE_JEWEL: return "case_jewel";
   case SHAPE_DVD: return "case_dvd";
   case SHAPE_UMD: return "case_umd";
   case SHAPE_CART_SNES: return "cart_snes";
   case SHAPE_CART_MD: return "cart_md";
   case SHAPE_CART_NES: return "cart_nes";
   case SHAPE_CART_N64: return "cart_n64";
   case SHAPE_CART_GB: return "cart_gb";
   case SHAPE_CART_GBA: return "cart_gba";
   case SHAPE_HUCARD: return "hucard";
   default: return "";
   }
}
static const char *mdl_console_key(int shape)
{
   switch (shape)
   {
   case SHAPE_CART_SNES: return "console_snes";
   case SHAPE_CART_MD: return "console_md";
   case SHAPE_CART_NES: return "console_nes";
   case SHAPE_CART_N64: return "console_n64";
   case SHAPE_CART_GB: return "console_gb";
   case SHAPE_CART_GBA: return "console_gba";
   case SHAPE_HUCARD: return "console_pce";
   default: return "";
   }
}
