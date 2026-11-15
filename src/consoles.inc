/* Lumen — 3D models of the cartridges and of the consoles they go into (launch
 * animation, insert_cartridge). Included by main.c (uses its box shader, case_part and
 * draw_quad_box). Every model is drawn in millimetres from the real hardware's
 * measurements, in a frame scaled by the caller; colours are the original plastics.
 * No logos or brand lettering are drawn. */

#define PI_F 3.14159265f

/* ---- primitives ------------------------------------------------------------------ */

/* one wall quad of a slab: centred at (cx, cy) of m's plane, facing angle ang (in the
 * plane), len along the edge, d through the slab */
static void wall_q(mat4 m, float cx, float cy, float ang, float len, float d, const float col[3], float alpha)
{
   mat4 q;
   if (len <= 0.0f) return;
   q = m4_mul(m, m4_mul(m4_translate(cx, cy, 0), m4_mul(m4_rot_z(ang), m4_mul(m4_rot_y(PI_F * 0.5f), m4_scale(d, len, 1)))));
   draw_quad_box(q, 0, col, alpha, 12);
}

/* rounded-rectangle face at z of m (back: facing -z), radii r = { top-left, top-right,
 * bottom-right, bottom-left } as seen from the front; optional rectangular hole hole[4]
 * = { centre x, centre y, half w, half h } (front-face coordinates) */
static void rr_face(mat4 m, float z, int back, float w, float h, const float r[4], const float *hole,
                    GLuint tex, const float col[3], float alpha)
{
   mat4 q = m4_mul(m, m4_mul(m4_translate(0, 0, z), m4_mul(m4_rot_y(back ? PI_F : 0.0f), m4_scale(w, h, 1))));
   if (back) glUniform4f(u_aux, r[1], r[0], r[3], r[2]); else glUniform4f(u_aux, r[0], r[1], r[2], r[3]);
   glUniform4f(u_aux2, w, h, hole ? hole[2] : 0.0f, hole ? hole[3] : 0.0f);
   glUniform4f(u_aux3, hole ? (back ? -hole[0] : hole[0]) : 0.0f, hole ? hole[1] : 0.0f, 0, 0);
   draw_quad_box(q, tex, col, alpha, tex ? 17 : 16);
}

/* Rounded slab centred on m's origin: w x h in m's xy plane, d thick along z, corner
 * radii r (see rr_face). cf / cb: front / back colours (NULL: not drawn), cs: walls.
 * hole: in the front face. skip: bit 0..3 leaves out the straight top / right / bottom
 * / left wall (the caller draws it with an opening). */
static void slab_ex(mat4 m, float w, float h, float d, const float r[4], const float *cf, const float *cb,
                    const float cs[3], float alpha, const float *hole, int skip)
{
   const float hw = w * 0.5f, hh = h * 0.5f;
   const float cx[4] = { -hw + r[0], hw - r[1], hw - r[2], -hw + r[3] };
   const float cy[4] = { hh - r[0], hh - r[1], -hh + r[2], -hh + r[3] };
   const float a0[4] = { PI_F * 0.5f, 0.0f, -PI_F * 0.5f, PI_F };
   int i, j;
   if (cf) rr_face(m, d * 0.5f, 0, w, h, r, hole, 0, cf, alpha);
   if (cb) rr_face(m, -d * 0.5f, 1, w, h, r, NULL, 0, cb, alpha);
   if (!(skip & 1)) wall_q(m, (cx[0] + cx[1]) * 0.5f, hh, PI_F * 0.5f, cx[1] - cx[0], d, cs, alpha);
   if (!(skip & 2)) wall_q(m, hw, (cy[1] + cy[2]) * 0.5f, 0.0f, cy[1] - cy[2], d, cs, alpha);
   if (!(skip & 4)) wall_q(m, (cx[2] + cx[3]) * 0.5f, -hh, -PI_F * 0.5f, cx[2] - cx[3], d, cs, alpha);
   if (!(skip & 8)) wall_q(m, -hw, (cy[0] + cy[3]) * 0.5f, PI_F, cy[0] - cy[3], d, cs, alpha);
   for (i = 0; i < 4; i++)
   {
      const int n = r[i] > 12.0f ? 6 : (r[i] > 0.0f ? 3 : 0);
      const float da = PI_F * 0.5f / (n ? n : 1), ch = 2.0f * r[i] * sinf(da * 0.5f), ro = r[i] * cosf(da * 0.5f);
      for (j = 0; j < n; j++)
      {
         const float tm = a0[i] + (j + 0.5f) * da;
         wall_q(m, cx[i] + ro * cosf(tm), cy[i] + ro * sinf(tm), tm, ch * 1.02f, d, cs, alpha);
      }
   }
}
static void slab(mat4 m, float w, float h, float d, const float r[4], const float *cf, const float *cb, const float cs[3])
{
   slab_ex(m, w, h, d, r, cf, cb, cs, 1.0f, NULL, 0);
}

/* frame for a slab lying flat (its front facing up), centred at (x, y, z) of c: the
 * slab's w is along x, its h along z (its "top" towards the back), its d upwards. A
 * hole at world z = zh is at face y = z - zh... callers use plan_hole(). */
static mat4 up_frame(mat4 c, float x, float y, float z)
{
   return m4_mul(c, m4_mul(m4_translate(x, y, z), m4_rot_x(-PI_F * 0.5f)));
}
/* a flat (up-facing) slab: plan w x dz at (x, z), from y0 to y1; radii as seen from
 * above: { back-left, back-right, front-right, front-left } */
static void plan_slab(mat4 c, float x, float z, float w, float dz, float y0, float y1, const float r[4],
                      const float *top, const float side[3], const float *hole_xz)
{
   float hole[4];
   if (hole_xz) { hole[0] = hole_xz[0] - x; hole[1] = z - hole_xz[1]; hole[2] = hole_xz[2]; hole[3] = hole_xz[3]; }
   slab_ex(up_frame(c, x, (y0 + y1) * 0.5f, z), w, dz, y1 - y0, r, top, NULL, side, 1.0f, hole_xz ? hole : NULL, 0);
}
/* a flat quad facing up / facing the front */
static void up_quad(mat4 c, float x, float y, float z, float w, float dz, const float col[3], int mode)
{
   case_part(c, x, y, z, 0, -PI_F * 0.5f, w, dz, 0, col, 1, mode);
}
static void front_quad(mat4 c, float x, float y, float z, float w, float h, const float col[3], int mode)
{
   case_part(c, x, y, z, 0, 0, w, h, 0, col, 1, mode);
}
/* a round button / light: diameter dia, on a surface facing up or the front */
static void up_round(mat4 c, float x, float y, float z, float dia, const float col[3])
{
   case_part(c, x, y, z, 0, -PI_F * 0.5f, dia, dia, 0, col, 1, 14);
}
static void front_round(mat4 c, float x, float y, float z, float dia, const float col[3])
{
   case_part(c, x, y, z, 0, 0, dia, dia, 0, col, 1, 14);
}
/* additive glow (a light that is on) */
static void glow_at(mat4 c, float x, float y, float z, int up, float size, const float col[3], float a)
{
   if (a <= 0.0f) return;
   scene_blend_add();
   glDepthMask(GL_FALSE);
   case_part(c, x, y, z, 0, up ? -PI_F * 0.5f : 0.0f, size, size, 0, col, a, 8);
   glDepthMask(GL_TRUE);
   scene_blend_over();
}
static void mulc(float out[3], const float c[3], float k) { out[0] = c[0] * k; out[1] = c[1] * k; out[2] = c[2] * k; }
static const float R0[4] = { 0, 0, 0, 0 };
static const float C_BLACK[3] = { 0.035f, 0.035f, 0.04f };
static const float C_LED_RED[3] = { 1.0f, 0.22f, 0.12f };

/* ---- cartridges ------------------------------------------------------------------ */

/* real size of each cartridge (mm): width, height, thickness */
static void cart_mm(const platform_t *pl, float *w, float *h, float *d)
{
   const model_t *mm = mdl_get(mdl_media_key(pl->shape));
   if (mm) { *w = mm->hi[0] - mm->lo[0]; *h = mm->hi[1] - mm->lo[1]; *d = mm->hi[2] - mm->lo[2]; return; }
   switch (pl->shape)
   {
   case SHAPE_CART_SNES: *w = 125; *h = 125; *d = 20; break;   /* Super Famicom / PAL */
   case SHAPE_CART_MD:   *w = 102; *h = 130; *d = 20; break;   /* Mega Drive (JP / PAL) */
   case SHAPE_CART_NES:  *w = 120; *h = 133; *d = 18; break;
   case SHAPE_CART_N64:  *w = 116; *h = 75;  *d = 20; break;
   case SHAPE_CART_GB:   *w = 57;  *h = 65;  *d = 8;  break;
   case SHAPE_CART_GBA:  *w = 57;  *h = 35;  *d = 8;  break;
   case SHAPE_HUCARD:    *w = 54;  *h = 85;  *d = 2;  break;
   default:              *w = 120; *h = 130; *d = 17; break;
   }
}
/* thickness of a cartridge / card as a fraction of its height */
static float cart_depth_k(const platform_t *pl)
{
   float w, h, d;
   cart_mm(pl, &w, &h, &d);
   return d / h;
}

/* label: a recess (darker frame) with the game's art in it */
static void cart_label(mat4 m, const game_t *g, float x, float y, float z, float w, float h, float frame, float rad,
                       const float recess[3], float alpha)
{
   static const float one[3] = { 1, 1, 1 };
   const float rf[4] = { rad + frame * 0.5f, rad + frame * 0.5f, rad + frame * 0.5f, rad + frame * 0.5f };
   const float rl[4] = { rad, rad, rad, rad };
   mat4 t = m4_mul(m, m4_translate(x, y, 0));
   if (frame > 0.0f) rr_face(t, z, 0, w + frame * 2, h + frame * 2, rf, NULL, 0, recess, alpha);
   rr_face(t, z + 0.15f, 0, w, h, rl, NULL, g->tex, one, alpha);
}

/* Cartridge (no photo of the game's own): the real shape of the platform's cartridge,
 * stretched to the declared aspect, in the platform colour, with the game's art on its
 * label. base: the cartridge's frame, height pl->h. */
static void draw_cart(const game_t *g, const platform_t *pl, mat4 base, float alpha)
{
   float BW, BH, BD, body[3], side[3], back[3], dark[3], lite[3];
   mat4 m;
   int k;
   const model_t *mdl = mdl_get(mdl_media_key(pl->shape));
   if (mdl)
   {  /* the modelled cartridge, its bounds fitted to the declared size, centred */
      const float sx = pl->w / (mdl->hi[0] - mdl->lo[0]), sy = pl->h / (mdl->hi[1] - mdl->lo[1]);
      m = m4_mul(base, m4_mul(m4_scale(sx, sy, sy), m4_translate(-(mdl->lo[0] + mdl->hi[0]) * 0.5f,
                 -(mdl->lo[1] + mdl->hi[1]) * 0.5f, -(mdl->lo[2] + mdl->hi[2]) * 0.5f)));
      mdl_draw(mdl, m, g, NULL, alpha);
      return;
   }
   cart_mm(pl, &BW, &BH, &BD);
   m = m4_mul(base, m4_scale(pl->w / BW, pl->h / BH, pl->h / BH));
   mulc(body, pl->color, 1.0f); mulc(side, pl->color, 0.80f); mulc(back, pl->color, 0.70f);
   mulc(dark, pl->color, 0.55f);
   for (k = 0; k < 3; k++) lite[k] = clampf(pl->color[k] * 1.12f + 0.03f, 0, 1);
   switch (pl->shape)
   {
   case SHAPE_CART_SNES:
   {  /* square body; the upper part narrower (the two side grips) with a round top */
      const float rl[4] = { 2, 2, 5, 5 }, ru[4] = { 22, 22, 0, 0 };
      slab_ex(m4_mul(m, m4_translate(0, -20, 0)), BW, 85, BD, rl, body, back, side, alpha, NULL, 0);
      slab_ex(m4_mul(m, m4_translate(0, 42.5f, 0)), BW - 12, 40, BD, ru, body, back, side, alpha, NULL, 4);
      rr_face(m4_mul(m, m4_translate(0, 30, 0)), BD * 0.5f + 0.1f, 0, BW - 30, 1.6f, R0, NULL, 0, dark, alpha);
      rr_face(m4_mul(m, m4_translate(0, 45, 0)), BD * 0.5f + 0.1f, 0, BW - 40, 1.6f, R0, NULL, 0, dark, alpha);
      cart_label(m, g, 0, -22, BD * 0.5f + 0.2f, 96, 58, 3, 2, dark, alpha);
      break;
   }
   case SHAPE_CART_MD:
   {  /* tall rectangle, a curved lip at the top, large label in a frame, grip ridges */
      const float rb[4] = { 10, 10, 4, 4 }, rt[4] = { 8, 8, 0, 0 };
      slab_ex(m4_mul(m, m4_translate(0, -6, 0)), BW, BH - 12, BD, rb, body, back, side, alpha, NULL, 0);
      slab_ex(m4_mul(m, m4_translate(0, BH * 0.5f - 6, -2)), BW - 4, 12, BD - 4, rt, lite, back, side, alpha, NULL, 4);
      cart_label(m, g, 0, 6, BD * 0.5f + 0.2f, 84, 84, 3, 2, dark, alpha);
      for (k = 0; k < 6; k++)
         rr_face(m4_mul(m, m4_translate(0, -BH * 0.5f + 6 + k * 3.2f, 0)), BD * 0.5f + 0.2f, 0, BW - 20, 1.4f, R0, NULL, 0, lite, alpha);
      break;
   }
   case SHAPE_CART_NES:
   {  /* tall box, a step at the top end, label high in a recess, ridged grip below */
      const float rr[4] = { 3, 3, 3, 3 };
      slab_ex(m, BW, BH, BD, rr, body, back, side, alpha, NULL, 0);
      rr_face(m4_mul(m, m4_translate(0, BH * 0.5f - 4, 0)), BD * 0.5f + 0.1f, 0, BW, 8, R0, NULL, 0, dark, alpha);
      cart_label(m, g, 0, 20, BD * 0.5f + 0.2f, 96, 66, 4, 1, dark, alpha);
      for (k = 0; k < 11; k++)
         rr_face(m4_mul(m, m4_translate(0, -BH * 0.5f + 7 + k * 3.3f, 0)), BD * 0.5f + 0.2f, 0, BW - 14, 1.5f, R0, NULL, 0, dark, alpha);
      break;
   }
   case SHAPE_CART_N64:
   {  /* wide upper body with round shoulders, the narrower connector end below */
      const float ru[4] = { 12, 12, 2, 2 }, rl[4] = { 0, 0, 4, 4 };
      slab_ex(m4_mul(m, m4_translate(0, 11.5f, 0)), BW, 52, BD, ru, body, back, side, alpha, NULL, 0);
      slab_ex(m4_mul(m, m4_translate(0, -26, 0)), BW - 20, 23, BD, rl, body, back, side, alpha, NULL, 1);
      cart_label(m, g, 0, 12, BD * 0.5f + 0.2f, 70, 38, 3, 3, dark, alpha);
      for (k = 0; k < 6; k++)
      {
         rr_face(m4_mul(m, m4_translate(-55 + k * 2.4f, -6, 0)), BD * 0.5f + 0.2f, 0, 1.2f, 14, R0, NULL, 0, dark, alpha);
         rr_face(m4_mul(m, m4_translate(55 - k * 2.4f, -6, 0)), BD * 0.5f + 0.2f, 0, 1.2f, 14, R0, NULL, 0, dark, alpha);
      }
      break;
   }
   case SHAPE_CART_GB:
   {  /* the cut top-right corner, ridges and an arrow up top, the recessed label */
      const float rr[4] = { 2, 11, 2, 2 };
      slab_ex(m, BW, BH, BD, rr, body, back, side, alpha, NULL, 0);
      for (k = 0; k < 3; k++)
         rr_face(m4_mul(m, m4_translate(-6, 27 - k * 2.6f, 0)), BD * 0.5f + 0.1f, 0, 34, 1.0f, R0, NULL, 0, dark, alpha);
      cart_label(m, g, 0, -9, BD * 0.5f + 0.2f, 45, 38, 1.5f, 1.5f, dark, alpha);
      break;
   }
   case SHAPE_CART_GBA:
   {  /* thicker top end with the grip, label over the lower part */
      const float rl[4] = { 0, 0, 3, 3 }, rt[4] = { 3, 3, 0, 0 };
      slab_ex(m4_mul(m, m4_translate(0, -4.5f, -0.5f)), BW, 26, BD - 1, rl, body, back, side, alpha, NULL, 1);
      slab_ex(m4_mul(m, m4_translate(0, 13, 0)), BW, 9, BD, rt, body, back, side, alpha, NULL, 4);
      for (k = 0; k < 4; k++)
         rr_face(m4_mul(m, m4_translate(0, 10 + k * 1.8f, 0)), BD * 0.5f + 0.1f, 0, BW - 10, 0.8f, R0, NULL, 0, dark, alpha);
      cart_label(m, g, 0, -4.5f, (BD - 1) * 0.5f - 0.3f, 47, 21, 1.2f, 1.2f, dark, alpha);
      break;
   }
   case SHAPE_HUCARD:
   {  /* a thin card: printed face, the gold contacts along the bottom edge */
      static const float gold[3] = { 0.86f, 0.68f, 0.30f };
      const float rr[4] = { 3, 3, 3, 3 };
      slab_ex(m, BW, BH, BD, rr, body, back, side, alpha, NULL, 0);
      cart_label(m, g, 0, 5, BD * 0.5f + 0.1f, 50, 70, 0, 2, dark, alpha);
      for (k = 0; k < 10; k++)
         rr_face(m4_mul(m, m4_translate(-20 + k * 4.4f, -BH * 0.5f + 5, 0)), BD * 0.5f + 0.1f, 0, 3, 7, R0, NULL, 0, gold, alpha);
      break;
   }
   default:
   {
      const float rr[4] = { 4, 4, 4, 4 };
      slab_ex(m, BW, BH, BD, rr, body, back, side, alpha, NULL, 0);
      cart_label(m, g, 0, 18, BD * 0.5f + 0.2f, 98, 76, 3, 2, dark, alpha);
      for (k = 0; k < 5; k++)
         rr_face(m4_mul(m, m4_translate(0, -BH * 0.5f + 8 + k * 4.0f, 0)), BD * 0.5f + 0.2f, 0, BW * 0.6f, 1.5f, R0, NULL, 0, dark, alpha);
      break;
   }
   }
}

/* ---- consoles: what each one is, at what scale ------------------------------------ */

typedef struct {
   float k;        /* world units per mm */
   float cart_w;   /* real width of its cartridge (mm) */
   float seat;     /* fraction of the cartridge's height inside once it is seated */
   float BW, BH, BD;  /* body (mm) */
   float slot_y;   /* front loader: centre of the opening below the top (mm, < 0) */
   float slot_x;   /* front loader: centre of the opening across (mm; the NES door is off-centre) */
   float slot_z;   /* handheld: centre of the cartridge through the body (mm) */
} con_spec_t;

static con_spec_t con_spec(const platform_t *pl)
{
   con_spec_t s;
   float cw, ch, cd;
   memset(&s, 0, sizeof(s));
   cart_mm(pl, &cw, &ch, &cd);
   s.cart_w = cw;
   if (pl->slot == SLOT_HANDHELD)
   {
      /* GBA: the pak goes all the way in, its top 8 mm under the top plateau (ref/gba.md 4) */
      if (pl->shape == SHAPE_CART_GBA) { s.BW = 144.5f; s.BH = 82; s.BD = 24.5f; s.seat = 1.23f; s.slot_z = -4; s.k = 1.90f / s.BW; }
      else                             { s.BW = 90; s.BH = 148; s.BD = 32; s.seat = 1.0f; s.slot_z = -11; s.k = 1.75f / s.BH; }
   }
   else if (pl->slot == SLOT_FRONT)
   {
      /* PC Engine: the card lies label up in the open tray on top, 3 mm under the plateau,
       * and slides back into the slot until its near end is at the front edge */
      if (pl->shape == SHAPE_HUCARD) { s.BW = 140; s.BH = 38; s.BD = 140; s.slot_y = -3.0f; s.seat = 1.0f; }
      else                           { s.BW = 256; s.BH = 85; s.BD = 203; s.slot_y = -12.75f; s.slot_x = -21.7f; s.seat = 0.97f; }
      s.k = 2.0f / s.BW;
   }
   else
   {
      switch (pl->shape)
      {
      case SHAPE_CART_SNES: s.BW = 200; s.BH = 72; s.BD = 242; s.seat = 0.46f; break;   /* ~40 mm of 87 go in */
      case SHAPE_CART_MD:   s.BW = 280; s.BH = 70; s.BD = 212; s.seat = 0.43f; break;   /* ~30 mm of 70 go in */
      case SHAPE_CART_N64:  s.BW = 260; s.BH = 73; s.BD = 190; s.seat = 0.22f; break;   /* ~17 mm of 77 go in */
      default:              s.BW = 230; s.BH = 65; s.BD = 200; s.seat = 0.55f; break;
      }
      s.k = 2.0f / s.BW;
   }
   return s;
}

/* ---- top loaders -------------------------------------------------------------------
 * Frame (mm): origin at the centre of the slot opening on the surface around it (y = 0),
 * x right, z towards the front. sw x sd: the opening; open: dust flaps 0 shut .. 1 pushed
 * in; pw: power 0 off .. 1 on (switch and LED). */

/* the well under the opening and its two spring flaps, hinged at the long edges */
static void slot_well(mat4 c, float sw, float sd, float depth, float open, const float flap[3])
{
   const float a = open * 1.45f;
   mat4 f;
   case_part(c, 0, -depth * 0.5f, sd * 0.5f, PI_F, 0, sw, depth, 0, C_BLACK, 1, 1);
   case_part(c, 0, -depth * 0.5f, -sd * 0.5f, 0, 0, sw, depth, 0, C_BLACK, 1, 1);
   case_part(c, -sw * 0.5f, -depth * 0.5f, 0, PI_F * 0.5f, 0, sd, depth, 0, C_BLACK, 1, 1);
   case_part(c, sw * 0.5f, -depth * 0.5f, 0, -PI_F * 0.5f, 0, sd, depth, 0, C_BLACK, 1, 1);
   up_quad(c, 0, -depth, 0, sw, sd, C_BLACK, 1);
   f = m4_mul(c, m4_mul(m4_translate(0, -1.5f, -sd * 0.5f), m4_rot_x(a)));
   up_quad(f, 0, 0, sd * 0.25f, sw - 1, sd * 0.5f, flap, 12);
   f = m4_mul(c, m4_mul(m4_translate(0, -1.5f, sd * 0.5f), m4_rot_x(-a)));
   up_quad(f, 0, 0, -sd * 0.25f, sw - 1, sd * 0.5f, flap, 12);
}

/* a slide switch on a top surface at y: track along z, knob at kz */
static void slide_switch(mat4 c, float x, float y, float z, float len, float kz, const float knob[3])
{
   const float rk[4] = { 2, 2, 2, 2 };
   up_quad(c, x, y + 0.1f, z, 9, len, C_BLACK, 1);
   plan_slab(c, x, kz, 15, 11, y, y + 6, rk, knob, knob, NULL);
}

/* Super Famicom / PAL Super Nintendo: light grey, rounded; a raised deck round the slot
 * with the power (left) and reset (right) slides and the eject lever in front of it;
 * two controller ports and the power LED on the front. */
static void con_snes(mat4 c, float sw, float sd, float open, float pw)
{
   static const float body[3] = { 0.78f, 0.78f, 0.80f }, side[3] = { 0.66f, 0.66f, 0.69f };
   static const float deck[3] = { 0.82f, 0.82f, 0.84f }, mid[3] = { 0.55f, 0.55f, 0.60f };
   static const float knob[3] = { 0.62f, 0.62f, 0.66f }, eject[3] = { 0.42f, 0.42f, 0.50f };
   const float rb[4] = { 18, 18, 26, 26 }, rd[4] = { 30, 30, 34, 34 }, re[4] = { 3, 3, 3, 3 };
   const float hole[4] = { 0, 0, sw * 0.5f + 1, sd * 0.5f + 1 };
   float led[3];
   int k;
   plan_slab(c, 0, -14, 200, 242, -72, -12, rb, body, side, hole);
   plan_slab(c, 0, -10, 168, 150, -12, 0, rd, deck, side, hole);
   slot_well(c, sw, sd, 60, open, mid);
   for (k = 0; k < 4; k++) up_quad(c, 0, 0.1f, -66 - k * 5.0f, 110, 1.6f, mid, 12);   /* grooves behind */
   slide_switch(c, -52, 0, 38, 26, lerpf(46, 30, pw), knob);                           /* POWER */
   slide_switch(c, 52, 0, 38, 26, 46, knob);                                            /* RESET */
   plan_slab(c, 0, 40, 34, 12, 0, 4, re, eject, eject, NULL);                           /* EJECT */
   /* front: dark base line, the ports, the LED */
   front_quad(c, 0, -67, 107.2f, 186, 7, mid, 12);
   front_quad(c, -52, -46, 107.2f, 24, 15, C_BLACK, 12);
   front_quad(c, -18, -46, 107.2f, 24, 15, C_BLACK, 12);
   led[0] = 0.25f + 0.75f * pw; led[1] = 0.03f + 0.10f * pw; led[2] = 0.03f + 0.06f * pw;
   front_round(c, -80, -42, 107.3f, 4, led);
   glow_at(c, -80, -42, 107.5f, 0, 30, C_LED_RED, 0.7f * pw);
}

/* Mega Drive (model 1): black; the big round raised panel round the slot (with its
 * groove), power slide and reset on either side, volume slide and headphone socket at
 * the front corners, ridges on the front of the top, controller ports and LED in front. */
static void con_md(mat4 c, float sw, float sd, float open, float pw)
{
   static const float body[3] = { 0.06f, 0.06f, 0.065f }, side[3] = { 0.045f, 0.045f, 0.05f };
   static const float ring[3] = { 0.10f, 0.10f, 0.11f }, mid[3] = { 0.16f, 0.16f, 0.18f };
   static const float knob[3] = { 0.24f, 0.24f, 0.27f };
   const float rb[4] = { 12, 12, 16, 16 }, rc[4] = { 85, 85, 85, 85 };
   const float hole[4] = { 0, 0, sw * 0.5f + 1, sd * 0.5f + 1 };
   float led[3];
   int k;
   plan_slab(c, 0, 5, 280, 212, -70, -16, rb, body, side, hole);
   plan_slab(c, 0, 0, 170, 170, -16, 0, rc, NULL, side, NULL);                          /* the round panel */
   glUniform4f(u_aux, (sw * 0.5f + 1) / 85.0f, (sd * 0.5f + 1) / 85.0f, 0.80f, 0);
   up_quad(c, 0, 0, 0, 170, 170, ring, 15);
   slot_well(c, sw, sd, 54, open, mid);
   slide_switch(c, -112, -16, -20, 30, lerpf(-8, 8, pw), knob);                         /* POWER */
   up_round(c, 112, -15.6f, -20, 15, knob);                                              /* RESET */
   up_quad(c, 100, -15.9f, 78, 42, 8, C_BLACK, 1);                                       /* VOLUME */
   plan_slab(c, 88, 78, 8, 12, -16, -11, rb, knob, knob, NULL);
   up_round(c, -112, -15.8f, 78, 9, C_BLACK);                                            /* PHONES */
   for (k = 0; k < 6; k++) up_quad(c, 0, -15.9f, 86 + k * 4.0f, 120, 1.6f, mid, 12);
   front_quad(c, -98, -44, 111.2f, 26, 15, mid, 12);
   front_quad(c, -62, -44, 111.2f, 26, 15, mid, 12);
   led[0] = 0.25f + 0.75f * pw; led[1] = 0.03f + 0.10f * pw; led[2] = 0.03f + 0.06f * pw;
   front_round(c, 108, -36, 111.3f, 4, led);
   glow_at(c, 108, -36, 111.5f, 0, 30, C_LED_RED, 0.7f * pw);
}

/* Nintendo 64: charcoal; the slot in a raised hump with its two spring doors, the
 * memory expansion lid in front of it, power and reset slides on either side, a round
 * front with the four controller ports, the LED by the power slide. */
static void con_n64(mat4 c, float sw, float sd, float open, float pw)
{
   static const float body[3] = { 0.20f, 0.20f, 0.23f }, side[3] = { 0.16f, 0.16f, 0.19f };
   static const float hump[3] = { 0.22f, 0.22f, 0.25f }, mid[3] = { 0.30f, 0.30f, 0.34f };
   static const float knob[3] = { 0.42f, 0.42f, 0.46f };
   const float rb[4] = { 22, 22, 58, 58 }, rh[4] = { 22, 22, 22, 22 }, rl[4] = { 4, 4, 4, 4 };
   const float hole[4] = { 0, 0, sw * 0.5f + 1, sd * 0.5f + 1 };
   float led[3];
   int k;
   plan_slab(c, 0, 20, 260, 190, -73, -28, rb, body, side, hole);
   plan_slab(c, 0, 0, 150, 116, -28, 0, rh, hump, side, hole);
   slot_well(c, sw, sd, 60, open, mid);
   for (k = 0; k < 5; k++) up_quad(c, 0, 0.1f, -38 - k * 4.0f, 110, 1.4f, side, 12);
   plan_slab(c, 0, 88, 84, 34, -28, -27.4f, rl, mid, side, NULL);                       /* expansion lid */
   up_quad(c, 0, -27.3f, 88, 70, 1.2f, side, 12);
   slide_switch(c, -88, -28, 72, 28, lerpf(78, 64, pw), knob);                           /* POWER */
   slide_switch(c, 88, -28, 72, 28, 78, knob);                                           /* RESET */
   for (k = 0; k < 4; k++)                                                               /* controller ports */
   {
      const float rp[4] = { 4, 4, 4, 4 };
      rr_face(m4_mul(c, m4_translate(-66 + k * 44.0f, -55, 0)), 115.2f, 0, 20, 14, rp, NULL, 0, C_BLACK, 1);
   }
   led[0] = 0.25f + 0.75f * pw; led[1] = 0.03f + 0.10f * pw; led[2] = 0.03f + 0.06f * pw;
   up_round(c, -112, -27.8f, 72, 5, led);
   glow_at(c, -112, -27.6f, 72, 1, 34, C_LED_RED, 0.7f * pw);
}

/* any other top loader: a plain rounded console */
static void con_generic(mat4 c, float sw, float sd, float open, float pw)
{
   static const float body[3] = { 0.30f, 0.30f, 0.34f }, side[3] = { 0.22f, 0.22f, 0.25f }, mid[3] = { 0.18f, 0.18f, 0.20f };
   const float rb[4] = { 14, 14, 20, 20 };
   const float hole[4] = { 0, 0, sw * 0.5f + 1, sd * 0.5f + 1 };
   float led[3];
   int k;
   plan_slab(c, 0, 10, 230, 200, -65, 0, rb, body, side, hole);
   slot_well(c, sw, sd, 55, open, mid);
   for (k = 0; k < 4; k++) up_quad(c, 0, 0.1f, -50 - k * 6.0f, 140, 2, mid, 12);
   led[0] = 0.25f + 0.75f * pw; led[1] = 0.03f + 0.10f * pw; led[2] = 0.03f + 0.06f * pw;
   front_round(c, -85, -35, 110.2f, 5, led);
   glow_at(c, -85, -35, 110.4f, 0, 30, C_LED_RED, 0.7f * pw);
}

static void draw_topcon(const platform_t *pl, mat4 c, float sw, float sd, float open, float pw)
{
   const model_t *m = mdl_get(mdl_console_key(pl->shape));
   if (m)
   {
      mpose_t ps;
      memset(&ps, 0, sizeof(ps));
      ps.ch[MC_OPEN] = open; ps.ch[MC_POWER] = pw; ps.light = pw;
      mdl_draw(m, c, NULL, &ps, 1.0f);
      return;
   }
   switch (pl->shape)
   {
   case SHAPE_CART_SNES: con_snes(c, sw, sd, open, pw); break;
   case SHAPE_CART_MD:   con_md(c, sw, sd, open, pw); break;
   case SHAPE_CART_N64:  con_n64(c, sw, sd, open, pw); break;
   default:              con_generic(c, sw, sd, open, pw); break;
   }
}

/* ---- front loaders -----------------------------------------------------------------
 * Frame (mm): top at y = 0, body centred on x and z, the front face at z = D/2 with the
 * opening (sw x sh) centred at y = slot_y; a tunnel of length tl behind it. */

static void tunnel(mat4 c, float zf, float y, float sw, float sh, float tl)
{
   case_part(c, -sw * 0.5f, y, zf - tl * 0.5f, PI_F * 0.5f, 0, tl, sh, 0, C_BLACK, 1, 1);
   case_part(c, sw * 0.5f, y, zf - tl * 0.5f, -PI_F * 0.5f, 0, tl, sh, 0, C_BLACK, 1, 1);
   case_part(c, 0, y + sh * 0.5f, zf - tl * 0.5f, 0, PI_F * 0.5f, sw, tl, 0, C_BLACK, 1, 1);
   case_part(c, 0, y - sh * 0.5f, zf - tl * 0.5f, 0, -PI_F * 0.5f, sw, tl, 0, C_BLACK, 1, 1);
   case_part(c, 0, y, zf - tl, 0, 0, sw, sh, 0, C_BLACK, 1, 1);
}

/* NES (front loader): two-tone grey box; the door in the upper front swings in as the
 * cartridge goes in (and shuts once it is pushed down), the dark band below with POWER,
 * RESET, the LED and the two controller ports; ridges on the back of the top. */
static void con_nes(const con_spec_t *s, mat4 c, float sw, float sh, float tl, float flap, float pw)
{
   static const float body[3] = { 0.80f, 0.79f, 0.76f }, side[3] = { 0.70f, 0.69f, 0.66f };
   static const float band[3] = { 0.25f, 0.25f, 0.27f }, door[3] = { 0.74f, 0.73f, 0.70f };
   static const float btn[3] = { 0.66f, 0.66f, 0.68f }, groove[3] = { 0.55f, 0.54f, 0.52f };
   const float zf = s->BD * 0.5f, rbtn[4] = { 2, 2, 2, 2 };
   const float hole[4] = { 0, s->slot_y + s->BH * 0.5f, sw * 0.5f + 1, sh * 0.5f + 1 };
   float led[3];
   mat4 f;
   int k;
   slab_ex(m4_mul(c, m4_translate(0, -s->BH * 0.5f, 0)), s->BW, s->BH, s->BD, R0, body, NULL, side, 1, hole, 0);
   for (k = 0; k < 9; k++) up_quad(c, 0, 0.1f, -90 + k * 6.0f, 210, 2, groove, 12);
   up_quad(c, 0, 0.1f, 40, 170, 1.4f, groove, 12);                                        /* the lid's outline */
   up_quad(c, -85, 0.1f, 70, 1.4f, 60, groove, 12);
   up_quad(c, 85, 0.1f, 70, 1.4f, 60, groove, 12);
   front_quad(c, 0, -62, zf + 0.1f, s->BW, 26, band, 12);
   tunnel(c, zf, s->slot_y, sw, sh, tl);
   f = m4_mul(c, m4_mul(m4_translate(0, s->slot_y + sh * 0.5f, zf - 1.5f), m4_rot_x(flap * 1.45f)));
   front_quad(f, 0, -sh * 0.5f, 0, sw, sh, door, 12);
   for (k = 0; k < 3; k++) front_quad(f, 0, -sh * (0.25f + k * 0.25f), 0.1f, sw - 10, 1.2f, side, 12);
   /* POWER (pressed in when on), RESET, the LED, the two ports */
   slab(m4_mul(c, m4_translate(-92, -62, zf + 2.5f - 2.0f * pw)), 24, 10, 5, rbtn, btn, NULL, side);
   slab(m4_mul(c, m4_translate(-62, -62, zf + 2.5f)), 24, 10, 5, rbtn, btn, NULL, side);
   led[0] = 0.25f + 0.75f * pw; led[1] = 0.03f + 0.10f * pw; led[2] = 0.03f + 0.06f * pw;
   front_round(c, -116, -62, zf + 0.3f, 5, led);
   glow_at(c, -116, -62, zf + 0.5f, 0, 32, C_LED_RED, 0.7f * pw);
   for (k = 0; k < 2; k++)
   {
      front_quad(c, 40 + k * 40.0f, -62, zf + 0.2f, 22, 22, btn, 12);
      front_quad(c, 40 + k * 40.0f, -62, zf + 0.3f, 10, 14, C_BLACK, 1);
   }
}

/* PC Engine: small and white on a grey base; the card slot in the front, the power
 * slide beside it (it moves over the card to lock it), the controller socket, vents. */
static void con_pce(const con_spec_t *s, mat4 c, float sw, float sh, float tl, float pw)
{
   static const float body[3] = { 0.92f, 0.92f, 0.90f }, side[3] = { 0.84f, 0.84f, 0.82f };
   static const float base[3] = { 0.30f, 0.30f, 0.33f }, vent[3] = { 0.70f, 0.70f, 0.70f };
   static const float knob[3] = { 0.40f, 0.40f, 0.44f };
   const float zf = s->BD * 0.5f, rt[4] = { 5, 5, 0, 0 }, rk[4] = { 1.5f, 1.5f, 1.5f, 1.5f };
   const float hole[4] = { 0, s->slot_y + 15.0f, sw * 0.5f + 0.5f, sh * 0.5f + 0.5f };
   int k;
   slab_ex(m4_mul(c, m4_translate(0, -15, 0)), s->BW, 30, s->BD, rt, body, NULL, side, 1, hole, 0);
   slab(m4_mul(c, m4_translate(0, -34, 0)), s->BW - 2, 8, s->BD - 2, R0, base, NULL, base);
   for (k = 0; k < 10; k++) up_quad(c, 0, 0.1f, -60 + k * 5.0f, 100, 1.6f, vent, 12);
   tunnel(c, zf, s->slot_y, sw, sh, tl);
   front_quad(c, 44, s->slot_y, zf + 0.1f, 30, 7, C_BLACK, 1);                          /* power track */
   slab(m4_mul(c, m4_translate(lerpf(52, 36, pw), s->slot_y, zf + 2)), 11, 8, 4, rk, knob, NULL, knob);
   front_round(c, -44, s->slot_y, zf + 0.1f, 14, knob);
   front_round(c, -44, s->slot_y, zf + 0.2f, 7, C_BLACK);
}

static void draw_frontcon(const platform_t *pl, const con_spec_t *s, mat4 c, float sw, float sh, float tl, float flap, float pw)
{
   const model_t *m = mdl_get(mdl_console_key(pl->shape));
   if (m)
   {
      mpose_t ps;
      memset(&ps, 0, sizeof(ps));
      ps.ch[MC_DOOR] = flap; ps.ch[MC_POWER] = pw; ps.light = pw;
      mdl_draw(m, c, NULL, &ps, 1.0f);
      return;
   }
   if (pl->shape == SHAPE_HUCARD) con_pce(s, c, sw, sh, tl, pw);
   else con_nes(s, c, sw, sh, tl, flap, pw);
}

/* ---- handhelds ---------------------------------------------------------------------
 * Frame (mm): centred on the body, the screen towards +z, the cartridge slot through the
 * top at z = slot_z (the cartridge goes in from the back's side). sw x sd: the slot;
 * screen: 0 off .. 1 on (the game's art, backlit); pw: power LED. */

/* the top wall with the slot opening (the slab is drawn with skip = 1) */
static void top_with_slot(mat4 c, float BW, float BH, float BD, const float r[4], float sw, float sd, float sz, const float col[3])
{
   const float y = BH * 0.5f, x0 = -BW * 0.5f + r[0], x1 = BW * 0.5f - r[1];
   const float zf = BD * 0.5f, zb = -BD * 0.5f, s0 = sz - sd * 0.5f, s1 = sz + sd * 0.5f;
   up_quad(c, (x0 + x1) * 0.5f, y, (s1 + zf) * 0.5f, x1 - x0, zf - s1, col, 12);
   up_quad(c, (x0 + x1) * 0.5f, y, (zb + s0) * 0.5f, x1 - x0, s0 - zb, col, 12);
   up_quad(c, (x0 - sw * 0.5f) * 0.5f, y, sz, -sw * 0.5f - x0, sd, col, 12);
   up_quad(c, (x1 + sw * 0.5f) * 0.5f, y, sz, x1 - sw * 0.5f, sd, col, 12);
}

static void screen_draw(const game_t *g, mat4 c, float x, float y, float z, float w, float h, float on,
                        const float off[3], const float tint[3])
{
   if (on < 0.02f || !g->tex) { front_quad(c, x, y, z, w, h, off, 1); return; }
   {
      float tn[3], gl[3];
      tn[0] = tint[0] * on; tn[1] = tint[1] * on; tn[2] = tint[2] * on;
      case_part(c, x, y, z, 0, 0, w, h, g->tex, tn, 1, 7);
      gl[0] = g->col[0] * 0.8f + 0.2f; gl[1] = g->col[1] * 0.8f + 0.2f; gl[2] = g->col[2] * 0.8f + 0.2f;
      glow_at(c, x, y, z + 0.4f, 0, w * 1.6f, gl, 0.30f * on);
   }
}

/* Game Boy (DMG): warm grey, the big round bottom-right corner; slate bezel with the
 * battery LED, the green screen, the D-pad, magenta A / B, slanted SELECT / START and the
 * speaker slots; the ON / OFF slide on the top; at the back the cartridge sits in an open
 * recess (label showing) under the top. */
static void hh_dmg(const game_t *g, const con_spec_t *s, mat4 c, float sw, float sd, float screen, float pw)
{
   static const float body[3] = { 0.78f, 0.77f, 0.74f }, side[3] = { 0.67f, 0.66f, 0.63f }, back[3] = { 0.72f, 0.71f, 0.68f };
   static const float slate[3] = { 0.31f, 0.32f, 0.41f }, scr_off[3] = { 0.52f, 0.56f, 0.30f }, scr_on[3] = { 0.70f, 0.86f, 0.48f };
   static const float pad[3] = { 0.09f, 0.09f, 0.10f }, ab[3] = { 0.55f, 0.10f, 0.33f }, ss[3] = { 0.44f, 0.44f, 0.48f };
   static const float recess[3] = { 0.36f, 0.36f, 0.35f };
   const float BW = s->BW, BH = s->BH, BD = s->BD, zf = BD * 0.5f, zb = -BD * 0.5f, top = BH * 0.5f;
   const float r[4] = { 4, 4, 30, 4 }, rb[4] = { 4, 4, 14, 4 }, rp[4] = { 2, 2, 2, 2 }, rab[4] = { 5.5f, 5.5f, 5.5f, 5.5f };
   const float wx = sw * 0.5f + 1.5f, wy0 = top - 68, rz = s->slot_z + sd * 0.5f + 1;   /* back recess */
   const float hole[4] = { 0, (top + wy0) * 0.5f, wx, (top - wy0) * 0.5f };
   float led[3];
   int k;
   slab_ex(c, BW, BH, BD, r, body, NULL, side, 1, NULL, 1);
   {  /* back face with the recess window (hole given in front-face coordinates) */
      const float hb[4] = { 0, hole[1], hole[2], hole[3] + 1 };
      rr_face(c, zb, 1, BW, BH, r, hb, 0, back, 1);
   }
   top_with_slot(c, BW, BH, BD, r, wx * 2, rz - zb, (rz + zb) * 0.5f, side);
   case_part(c, 0, (top + wy0) * 0.5f, rz, PI_F, 0, wx * 2, top - wy0, 0, recess, 1, 12);            /* recess floor */
   case_part(c, -wx, (top + wy0) * 0.5f, (rz + zb) * 0.5f, PI_F * 0.5f, 0, rz - zb, top - wy0, 0, recess, 1, 12);
   case_part(c, wx, (top + wy0) * 0.5f, (rz + zb) * 0.5f, -PI_F * 0.5f, 0, rz - zb, top - wy0, 0, recess, 1, 12);
   up_quad(c, 0, wy0, (rz + zb) * 0.5f, wx * 2, rz - zb, recess, 12);
   for (k = 0; k < 4; k++) case_part(c, 0, -top + 30 + k * 3.0f, zb - 0.2f, PI_F, 0, 40, 1.2f, 0, side, 1, 12);   /* battery cover grip */
   /* ON / OFF slide on the top, left */
   up_quad(c, -27, top + 0.1f, 6, 22, 5, pad, 1);
   plan_slab(c, lerpf(-33, -21, pw), 6, 8, 6, top, top + 2.5f, rp, ss, ss, NULL);
   /* front */
   rr_face(m4_mul(c, m4_translate(0, 35, 0)), zf + 0.15f, 0, 76, 57, rb, NULL, 0, slate, 1);
   screen_draw(g, c, 3, 36, zf + 0.3f, 47, 43, screen, scr_off, scr_on);
   led[0] = 0.25f + 0.75f * pw; led[1] = 0.03f + 0.10f * pw; led[2] = 0.03f + 0.06f * pw;
   front_round(c, -30, 44, zf + 0.3f, 4, led);
   glow_at(c, -30, 44, zf + 0.5f, 0, 22, C_LED_RED, 0.7f * pw);
   slab(m4_mul(c, m4_translate(-24, -21, zf + 1.5f)), 25, 8, 3, rp, pad, NULL, pad);
   slab(m4_mul(c, m4_translate(-24, -21, zf + 1.5f)), 8, 25, 3, rp, pad, NULL, pad);
   slab(m4_mul(c, m4_translate(29, -15, zf + 1.5f)), 11, 11, 3, rab, ab, NULL, ab);
   slab(m4_mul(c, m4_translate(14, -23, zf + 1.5f)), 11, 11, 3, rab, ab, NULL, ab);
   for (k = 0; k < 2; k++)
      slab(m4_mul(c, m4_mul(m4_translate(-9 + k * 16.0f, -47, zf + 0.8f), m4_rot_z(0.45f))), 14, 4, 1.6f, rp, ss, NULL, ss);
   for (k = 0; k < 6; k++)
      case_part(m4_mul(c, m4_mul(m4_translate(18 + k * 4.5f, -56 + k * 0.8f, 0), m4_rot_z(0.45f))), 0, 0, zf + 0.2f, 0, 0, 2.2f, 20, 0, pad, 1, 12);
}

/* Game Boy Advance: indigo, landscape with round ends; the screen in its dark window,
 * D-pad left, A / B right, START / SELECT under the D-pad, L / R on the shoulders, the
 * power LED, the speaker; the cartridge stands out of the top at the back. */
static void hh_gba(const game_t *g, const con_spec_t *s, mat4 c, float sw, float sd, float screen, float pw)
{
   static const float body[3] = { 0.31f, 0.29f, 0.58f }, side[3] = { 0.25f, 0.24f, 0.48f }, back[3] = { 0.27f, 0.26f, 0.52f };
   static const float win[3] = { 0.05f, 0.05f, 0.07f }, scr_off[3] = { 0.06f, 0.07f, 0.06f }, scr_on[3] = { 1, 1, 1 };
   static const float pad[3] = { 0.10f, 0.10f, 0.13f }, btn[3] = { 0.62f, 0.62f, 0.68f }, sh[3] = { 0.20f, 0.19f, 0.36f };
   static const float led_on[3] = { 0.25f, 1.0f, 0.35f };
   const float BW = s->BW, BH = s->BH, BD = s->BD, zf = BD * 0.5f, zb = -BD * 0.5f, top = BH * 0.5f;
   const float r[4] = { 16, 16, 34, 34 }, rw[4] = { 6, 6, 6, 6 }, rp[4] = { 1.5f, 1.5f, 1.5f, 1.5f };
   const float rab[4] = { 5.5f, 5.5f, 5.5f, 5.5f }, rs[4] = { 3, 3, 3, 3 };
   float led[3];
   int k;
   slab_ex(c, BW, BH, BD, r, body, back, side, 1, NULL, 1);
   top_with_slot(c, BW, BH, BD, r, sw, sd, s->slot_z, side);
   case_part(c, 0, top - 13, s->slot_z, 0, 0, sw, 26, 0, C_BLACK, 1, 1);                  /* inside the slot */
   case_part(c, 0, top - 13, s->slot_z - sd * 0.5f, 0, 0, sw, 26, 0, C_BLACK, 1, 1);
   plan_slab(c, -52, -2, 36, 16, top, top + 3, rs, sh, sh, NULL);                         /* L / R */
   plan_slab(c, 52, -2, 36, 16, top, top + 3, rs, sh, sh, NULL);
   for (k = 0; k < 3; k++) case_part(c, 0, -top + 14 + k * 3.0f, zb - 0.2f, PI_F, 0, 50, 1.2f, 0, side, 1, 12);
   /* front */
   rr_face(m4_mul(c, m4_translate(0, 4, 0)), zf + 0.15f, 0, 90, 62, rw, NULL, 0, win, 1);
   screen_draw(g, c, 0, 5, zf + 0.3f, 61, 41, screen, scr_off, scr_on);
   slab(m4_mul(c, m4_translate(-55, 4, zf + 1.2f)), 22, 7, 2.4f, rp, pad, NULL, pad);
   slab(m4_mul(c, m4_translate(-55, 4, zf + 1.2f)), 7, 22, 2.4f, rp, pad, NULL, pad);
   slab(m4_mul(c, m4_translate(58, 10, zf + 1.5f)), 11, 11, 3, rab, btn, NULL, btn);
   slab(m4_mul(c, m4_translate(45, 1, zf + 1.5f)), 11, 11, 3, rab, btn, NULL, btn);
   front_round(c, -55, -24, zf + 0.3f, 6, btn);
   front_round(c, -55, -33, zf + 0.3f, 6, btn);
   for (k = 0; k < 6; k++) front_round(c, 46 + (k % 3) * 5.0f, -24 - (k / 3) * 5.0f, zf + 0.2f, 2.6f, pad);
   led[0] = 0.10f + 0.15f * pw; led[1] = 0.12f + 0.88f * pw; led[2] = 0.10f + 0.25f * pw;
   front_round(c, -55, 24, zf + 0.3f, 3.5f, led);
   glow_at(c, -55, 24, zf + 0.5f, 0, 20, led_on, 0.6f * pw);
}

static void draw_handheld(const game_t *g, const platform_t *pl, const con_spec_t *s, mat4 c, float sw, float sd, float screen, float pw)
{
   const model_t *m = mdl_get(mdl_console_key(pl->shape));
   if (m)
   {
      mpose_t ps;
      memset(&ps, 0, sizeof(ps));
      ps.ch[MC_POWER] = pw; ps.light = pw; ps.screen = screen;
      mdl_draw(m, c, g, &ps, 1.0f);
      return;
   }
   if (pl->shape == SHAPE_CART_GBA) hh_gba(g, s, c, sw, sd, screen, pw);
   else hh_dmg(g, s, c, sw, sd, screen, pw);
}
