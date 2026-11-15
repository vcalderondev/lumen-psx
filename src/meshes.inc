/* Lumen — modelled hardware: consoles, cartridges and cases built in Blender
 * (tools/models/, one Python script per model) and exported as .lmdl, loaded from <home>/assets/models/<key>.lmdl.
 * Included by main.c. A missing model falls back to the procedural one (consoles.inc).
 *
 * .lmdl v1, little endian, millimetres, y up, the model's front towards +z:
 *   "LMDL", u32 version = 1, u32 nparts, then per part:
 *     char name[48]; float rgba[4]; float on[4]; float gloss, spec;
 *     u32 kind, anim, chan; float pivot[3], axis[3]; float amount; u32 nverts;
 *     nverts x { float pos[3], uv[2], normal[3] }   (triangles)
 *   kind: 0 plastic, 1 label (the game's art x rgba), 2 screen (rgba off; the game's art
 *         x on when lit), 3 light (rgba off, on when lit, plus a glow), 4 glass
 *         (translucent, drawn last), 5 dark (unlit: inside slots), 6 back (the case's
 *         back card), 7 spine (the printed spine)
 *   anim: 0 none, 1 rotate about pivot / axis by amount degrees x channel, 2 move along
 *         axis by amount mm x channel
 *   chan: 1 open (dust flaps, case lid), 2 door, 3 power (switch, button), 4 eject */

static void glow_at(mat4 c, float x, float y, float z, int up, float size, const float col[3], float a);

enum { MK_PLASTIC, MK_LABEL, MK_SCREEN, MK_LIGHT, MK_GLASS, MK_DARK, MK_BACK, MK_SPINE };
enum { MA_NONE, MA_ROT, MA_MOVE };
enum { MC_NONE, MC_OPEN, MC_DOOR, MC_POWER, MC_EJECT, MC_COUNT };

typedef struct {
   char name[48];
   float rgba[4], on[4], gloss, spec, pivot[3], axis[3], amount;
   unsigned kind, anim, chan;
   int first, count;          /* vertex range in the model's buffer */
} mpart_t;

typedef struct {
   char key[32];
   int state;                 /* 0 not tried, 1 loaded, -1 missing / bad */
   GLuint vao, vbo;
   int nparts;
   mpart_t *part;
   float lo[3], hi[3];        /* bounds (mm) */
} model_t;

/* pose: the animated channels (0..1) and what lights up */
typedef struct { float ch[MC_COUNT]; float light, screen; } mpose_t;

#define MAX_MODELS 32
static model_t g_models[MAX_MODELS];
static int g_nmodels;

static int mdl_load(model_t *m)
{
   char path[400];
   FILE *f;
   unsigned hdr[3], k;
   float *verts = NULL;
   size_t nv = 0, cap = 0;
   snprintf(path, sizeof(path), "%s/assets/models/%s.lmdl", g_home, m->key);
   if (!(f = fopen(path, "rb"))) return -1;
   if (fread(hdr, 4, 3, f) != 3 || memcmp(hdr, "LMDL", 4) || hdr[1] != 1 || hdr[2] == 0 || hdr[2] > 512)
   { fclose(f); logf_("model %s: bad header", m->key); return -1; }
   m->nparts = (int)hdr[2];
   m->part = calloc(m->nparts, sizeof(mpart_t));
   for (k = 0; k < 3; k++) { m->lo[k] = 1e9f; m->hi[k] = -1e9f; }
   for (k = 0; m->part && k < (unsigned)m->nparts; k++)
   {
      mpart_t *p = &m->part[k];
      unsigned u[3], n;
      size_t i;
      if (fread(p->name, 1, 48, f) != 48 || fread(p->rgba, 4, 4, f) != 4 || fread(p->on, 4, 4, f) != 4 ||
          fread(&p->gloss, 4, 1, f) != 1 || fread(&p->spec, 4, 1, f) != 1 || fread(u, 4, 3, f) != 3 ||
          fread(p->pivot, 4, 3, f) != 3 || fread(p->axis, 4, 3, f) != 3 || fread(&p->amount, 4, 1, f) != 1 ||
          fread(&n, 4, 1, f) != 1 || n > 4000000)
         break;
      p->name[47] = 0; p->kind = u[0]; p->anim = u[1]; p->chan = u[2] < MC_COUNT ? u[2] : 0;
      if (nv + n > cap)
      {
         float *nb;
         cap = (nv + n) * 2;
         if (!(nb = realloc(verts, cap * 32))) break;
         verts = nb;
      }
      if (fread(verts + nv * 8, 32, n, f) != n) break;
      p->first = (int)nv; p->count = (int)n;
      for (i = 0; i < n; i++)
      {
         const float *v = verts + (nv + i) * 8;
         int a;
         for (a = 0; a < 3; a++) { if (v[a] < m->lo[a]) m->lo[a] = v[a]; if (v[a] > m->hi[a]) m->hi[a] = v[a]; }
      }
      nv += n;
   }
   fclose(f);
   if (!m->part || k != (unsigned)m->nparts || !nv)
   {
      logf_("model %s: truncated or empty (%u of %d parts)", m->key, k, m->nparts);
      free(verts); free(m->part); m->part = NULL; m->nparts = 0;
      return -1;
   }
   glGenVertexArrays(1, &m->vao);
   glBindVertexArray(m->vao);
   glGenBuffers(1, &m->vbo);
   glBindBuffer(GL_ARRAY_BUFFER, m->vbo);
   glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(nv * 32), verts, GL_STATIC_DRAW);
   glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 32, (void *)0);
   glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 32, (void *)12);
   glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, 32, (void *)20);
   glEnableVertexAttribArray(0);
   glEnableVertexAttribArray(1);
   glEnableVertexAttribArray(2);
   glBindVertexArray(g_vao_quad);
   glBindBuffer(GL_ARRAY_BUFFER, g_vbo_quad);
   free(verts);
   logf_("model %s: %d parts, %zu vertices, %.0f x %.0f x %.0f mm", m->key, m->nparts, nv,
         m->hi[0] - m->lo[0], m->hi[1] - m->lo[1], m->hi[2] - m->lo[2]);
   return 1;
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

static void mdl_part_draw(const model_t *m, const mpart_t *p, mat4 base, const game_t *g, const mpose_t *ps, float alpha)
{
   const float v = ps && p->chan ? ps->ch[p->chan] : 0.0f;
   mat4 model = base, mvp;
   float tint[3];
   GLuint tex = 0;
   int mode = 18;
   (void)m;
   if (p->anim == MA_ROT && v != 0.0f)
      model = m4_mul(base, m4_mul(m4_translate(p->pivot[0], p->pivot[1], p->pivot[2]),
                     m4_mul(m4_rot_axis(p->axis, p->amount * v * (float)M_PI / 180.0f),
                            m4_translate(-p->pivot[0], -p->pivot[1], -p->pivot[2]))));
   else if (p->anim == MA_MOVE && v != 0.0f)
      model = m4_mul(base, m4_translate(p->axis[0] * p->amount * v, p->axis[1] * p->amount * v, p->axis[2] * p->amount * v));
   tint[0] = p->rgba[0]; tint[1] = p->rgba[1]; tint[2] = p->rgba[2];
   switch (p->kind)
   {
   case MK_LABEL:
      if (g && g->tex) { tex = g->tex; mode = 19; }
      break;
   case MK_SCREEN:
   {
      const float on = ps ? ps->screen : 0.0f;
      if (on > 0.02f && g && g->tex)
      {
         tex = g->tex; mode = 7;
         tint[0] = p->on[0] * on; tint[1] = p->on[1] * on; tint[2] = p->on[2] * on;
      }
      else mode = 20;
      break;
   }
   case MK_LIGHT:
   {
      const float on = ps ? ps->light : 0.0f;
      int k;
      for (k = 0; k < 3; k++) tint[k] = lerpf(p->rgba[k], p->on[k], on);
      mode = 20;
      break;
   }
   case MK_DARK: mode = 20; break;
   case MK_BACK:
      if (g && g->back_tex) { tex = g->back_tex; mode = 19; tint[0] = tint[1] = tint[2] = 1.0f; }
      break;
   case MK_SPINE:
      if (g && g->spine_tex) { tex = g->spine_tex; mode = 3; tint[0] = tint[1] = tint[2] = 1.0f; }
      break;
   default: break;
   }
   mvp = m4_mul(g_vp, model);
   glUniformMatrix4fv(u_mvp, 1, GL_FALSE, mvp.m);
   glUniformMatrix4fv(u_model, 1, GL_FALSE, model.m);
   glUniform3fv(u_tint, 1, tint);
   glUniform1f(u_alpha, alpha * p->rgba[3]);
   glUniform1i(u_mode, mode);
   glUniform4f(u_aux3, 0, 0, p->gloss > 1.0f ? p->gloss : 24.0f, p->spec);
   glBindTexture(GL_TEXTURE_2D, tex ? tex : g_white);
   glDrawArrays(GL_TRIANGLES, p->first, p->count);
   if (p->kind == MK_LIGHT && ps && ps->light > 0.0f)
   {  /* the glow of a lit light, at its pivot, facing the model's front */
      static float gl[3];
      const float *q = p->pivot;
      gl[0] = p->on[0]; gl[1] = p->on[1]; gl[2] = p->on[2];
      glBindVertexArray(g_vao_quad);
      glow_at(base, q[0], q[1], q[2] + 0.6f, 0, 18.0f, gl, 0.7f * ps->light);
      glBindVertexArray(m->vao);
   }
}

/* draw a model in frame base (mm); glass parts last, without depth writes */
static void mdl_draw(const model_t *m, mat4 base, const game_t *g, const mpose_t *ps, float alpha)
{
   int i;
   glBindVertexArray(m->vao);
   for (i = 0; i < m->nparts; i++)
      if (m->part[i].kind != MK_GLASS) mdl_part_draw(m, &m->part[i], base, g, ps, alpha);
   glDepthMask(GL_FALSE);
   for (i = 0; i < m->nparts; i++)
      if (m->part[i].kind == MK_GLASS) mdl_part_draw(m, &m->part[i], base, g, ps, alpha);
   glDepthMask(GL_TRUE);
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
