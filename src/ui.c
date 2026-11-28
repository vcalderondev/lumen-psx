/* UI kit, see ui.h. */
#include <stdio.h>
#include <string.h>
#include "lumen.h"
#include "ui.h"
#include "theme.h"

void ui_fill(float x, float y, float w, float h, float radius, const float rgb[3], float a)
{
   const float c[4] = { rgb[0], rgb[1], rgb[2], a };
   if (a > 0.003f) ui_shape(x, y, w, h, radius, 0.0f, c);
}

/* the next ui_scrim() draws nothing: the frame under it was drawn with it folded in
 * (main.c dim_fold) */
static int s_scrim_skip;
void ui_scrim_skip(int on) { s_scrim_skip = on; }

void ui_scrim(float a)
{
   float c[4];
   if (s_scrim_skip) { s_scrim_skip = 0; return; }
   if (a <= 0.003f) return;
   theme_color(TC_BACKDROP, c); c[3] = ui_scrim_alpha() * a;
   draw_2d(0, 0, 0, W, H, c, 0);
}

void ui_card(float x, float y, float w, float h, float a)
{
   /* the theme's card colour: opaque in Lumen's own look (card_alpha 1, the welcome's black);
    * a translucent theme (card_alpha < 1) gets a breath of the glass tint */
   const float ca = theme_card_alpha();
   const float base = ca >= 0.92f ? 0.86f + (ca - 0.92f) * (0.14f / 0.08f) : 0.86f * ca / 0.92f;
   const float tint = ca >= 1.0f ? 0.0f : 0.03f, cover = 1.0f - (1.0f - base) * (1.0f - tint);
   float c[4], g[3];
   int i;
   if (a <= 0.003f) return;
   theme_color(TC_CARD, c); theme_color(TC_GLASS, g);
   for (i = 0; i < 3; i++) c[i] = (c[i] * base * (1.0f - tint) + g[i] * tint) / cover;
   c[3] = cover * a;
   ui_glass_card(x, y, w, h, UI_CARD_R, c, UI_EDGE * a, 28, 12, 0.45f * a);
}

void ui_rest(float x, float y, float w, float h, float radius, float a)
{
   float edge[4];
   if (a <= 0.003f) return;
   if (radius > UI_CTL_R) radius = UI_CTL_R;
   theme_color(TC_GLASS, edge); edge[3] = UI_EDGE * a;
   ui_fill(x, y, w, h, radius, edge, UI_REST * a);
   ui_shape(x, y, w, h, radius, 1.0f, edge);
}

void ui_ring(float x, float y, float w, float h, float radius, float gap, float a)
{
   const float o = gap + 3.0f;
   float c[4];
   theme_color(TC_FOCUS, c); c[3] = 0.95f * a;
   if (a > 0.003f) ui_shape(x - o, y - o, w + 2 * o, h + 2 * o, radius + o, 3.0f, c);
}

void ui_focus(float x, float y, float w, float h, float radius, float a)
{
   float g[3];
   if (a <= 0.003f) return;
   if (radius > UI_CTL_R) radius = UI_CTL_R;
   theme_color(TC_GLASS, g);
   ui_shadow(x, y, w, h, radius, 12, 4, 0.35f * a);
   ui_fill(x, y, w, h, radius, g, UI_SELECTED * a);
   ui_ring(x, y, w, h, radius, 4.0f, a);
}

const char *ui_fit(TTF_Font *f, const char *s, float max_w, char *buf, size_t n)
{
   int w, h;
   size_t len;
   if (!f || n < 8 || TTF_SizeUTF8(f, s, &w, &h) != 0 || w <= max_w * g_ui_scale) return s;
   snprintf(buf, n - 4, "%s", s);
   len = strlen(buf);
   while (len > 0)
   {
      do len--; while (len > 0 && ((unsigned char)buf[len] & 0xC0) == 0x80);
      memcpy(buf + len, "\xE2\x80\xA6", 4);
      if (TTF_SizeUTF8(f, buf, &w, &h) != 0 || w <= max_w * g_ui_scale) break;
   }
   return buf;
}

void ui_kicker(label_t *l, const char *text, float x, float y, float a)
{
   char up[200];
   size_t i = 0;
   float c[4];
   const unsigned char *p = (const unsigned char *)text;
   while (*p && i + 3 < sizeof(up))
   {
      if (*p >= 'a' && *p <= 'z') up[i++] = (char)(*p++ - 32);
      else if (*p == 0xC3 && p[1] >= 0xA0 && p[1] <= 0xBE && p[1] != 0xB7) { up[i++] = (char)0xC3; up[i++] = (char)(p[1] - 0x20); p += 2; }
      else up[i++] = (char)*p++;
   }
   up[i] = 0;
   label_set(l, g_font_caps ? g_font_caps : g_font_small, up);
   theme_color(TC_DIM, c); c[3] = a;
   if (l->tex && a > 0.003f) draw_2d(l->tex, floorf(x), floorf(y), l->w, l->h, c, 0);
}

/* ------------------------------------------------------------------------ */
/* Controls (ui.h) */
static void put(const label_t *l, float x, float y, const float c[4])
{ if (l->tex && c[3] > 0.003f) draw_2d(l->tex, floorf(x), floorf(y), l->w, l->h, c, 0); }

/* the dark well a segmented control and a stepper sit in */
static void well(float x, float y, float w, float h, float a)
{
   const float in[4] = { 0, 0, 0, 0.34f * a };
   float ed[4];
   const float r = UI_WELL_R < UI_CTL_R ? UI_WELL_R : UI_CTL_R;
   theme_color(TC_GLASS, ed); ed[3] = 0.14f * a;
   ui_shape(x, y, w, h, r, 0.0f, in);
   ui_shape(x, y, w, h, r, 1.0f, ed);
}

float ui_switch(float xr, float cy, int on, float a)
{  /* the track in the accent when on, the knob always light (dark on a light accent) */
   const float tw = 46, th = 26, kd = 20, tx = xr - tw, ty = cy - th * 0.5f;
   float tc[4], kc[4] = { 1, 1, 1, a };
   if (on)
   {
      theme_color(TC_ACCENT, tc); tc[3] = a;
      if (tc[0] + tc[1] + tc[2] > 2.4f) kc[0] = kc[1] = kc[2] = 0.05f;
   }
   else { theme_color(TC_GLASS, tc); tc[3] = 0.18f * a; }
   ui_shape(tx, ty, tw, th, th * 0.5f, 0.0f, tc);
   ui_shape(tx + (on ? tw - kd - 3 : 3), ty + 3, kd, kd, kd * 0.5f, 0.0f, kc);
   return tw;
}

float ui_segmented(ui_seg_t *s, float xr, float cy, const char *const *opts, int n, int sel, float a)
{  /* every option in sight, the chosen one a filled pill in the text colour */
   const float sy = cy - UI_CTL_H * 0.5f, ri = 9 < UI_CTL_R ? 9 : UI_CTL_R;
   float total = 6, sx, on[4], ont[4] = { 0.043f, 0.047f, 0.063f, a }, dim[4];
   int k;
   if (n > 3) n = 3;
   for (k = 0; k < n; k++) { label_set(&s->l[k], g_font_caps ? g_font_caps : g_font_small, opts[k]); total += s->l[k].w + 28; }
   theme_color(TC_TEXT, on); on[3] = a;
   if (on[0] + on[1] + on[2] < 1.5f) ont[0] = ont[1] = ont[2] = 0.98f;   /* a dark text colour: light on it */
   theme_color(TC_DIM, dim); dim[3] = a;
   sx = xr - total;
   well(sx, sy, total, UI_CTL_H, a);
   sx += 3;
   for (k = 0; k < n; k++)
   {
      const float w = s->l[k].w + 28;
      if (k == sel) ui_shape(sx, sy + 3, w, UI_CTL_H - 6, ri, 0.0f, on);
      put(&s->l[k], sx + 14, cy - s->l[k].h * 0.5f, k == sel ? ont : dim);
      sx += w;
   }
   return total;
}

float ui_stepper(label_t *l, float xr, float cy, const char *value, float max_w, int at_lo, int at_hi, const float *dot, float a)
{  /* < value > in a well; an arrow dims at its end of the range */
   static label_t lt, gt;
   const float sy = cy - UI_CTL_H * 0.5f, padx = 12, gap = 10, dd = 16;
   float txt[4], dim[4], w, x;
   char buf[160];
   TTF_Font *f = g_font_caps ? g_font_caps : g_font_small;
   label_set(&lt, f, "\xE2\x80\xB9"); label_set(&gt, f, "\xE2\x80\xBA");
   w = 2 * padx + lt.w + gt.w + 2 * gap + (dot ? dd + 8 : 0);
   if (max_w - w < 40) max_w = w + 40;
   label_set(l, f, ui_fit(f, value, max_w - w, buf, sizeof(buf)));
   w += l->w;
   theme_color(TC_TEXT, txt); txt[3] = a;
   theme_color(TC_DIM, dim);
   well(xr - w, sy, w, UI_CTL_H, a);
   x = xr - w + padx;
   dim[3] = a * (at_lo ? 0.30f : 1.0f); put(&lt, x, cy - lt.h * 0.5f, dim); x += lt.w + gap;
   if (dot)
   {
      const float dc[4] = { dot[0], dot[1], dot[2], a };
      ui_shape(x, cy - dd * 0.5f, dd, dd, dd * 0.5f, 0.0f, dc); x += dd + 8;
   }
   put(l, x, cy - l->h * 0.5f, txt); x += l->w + gap;
   dim[3] = a * (at_hi ? 0.30f : 1.0f); put(&gt, x, cy - gt.h * 0.5f, dim);
   return w;
}

float ui_slider(label_t *l, float xr, float cy, float v, const char *text, float a)
{  /* a track with its filled part in the accent and a knob; the figure to its left */
   const float tw = 168, th = 6, kd = 18, tx = xr - tw;
   float track[4], fill[4], knob[4] = { 1, 1, 1, a }, txt[4];
   v = v < 0 ? 0 : v > 1 ? 1 : v;
   theme_color(TC_GLASS, track); track[3] = 0.18f * a;
   theme_color(TC_ACCENT, fill); fill[3] = a;
   theme_color(TC_TEXT, txt); txt[3] = 0.92f * a;
   ui_shape(tx, cy - th * 0.5f, tw, th, th * 0.5f, 0.0f, track);
   if (v > 0.001f) ui_shape(tx, cy - th * 0.5f, kd * 0.5f + (tw - kd) * v, th, th * 0.5f, 0.0f, fill);
   ui_shape(tx + (tw - kd) * v, cy - kd * 0.5f, kd, kd, kd * 0.5f, 0.0f, knob);
   label_set(l, g_font_caps ? g_font_caps : g_font_small, text ? text : "");
   put(l, tx - 14 - l->w, cy - l->h * 0.5f, txt);
   return tw + (l->w ? 14 + l->w : 0);
}

float ui_chevron(float xr, float cy, float a)
{
   float c[4];
   theme_color(TC_DIM, c); c[3] = a;
   glyph_draw("chevron-right", xr - glyph_w("chevron-right", 16), cy - 8, 16, c);
   return glyph_w("chevron-right", 16);
}

float ui_mark(float xr, float cy, float a)
{
   float c[4];
   theme_color(TC_ACCENT, c); c[3] = a;
   ui_shape(xr - 12, cy - 6, 12, 12, 6, 0.0f, c);
   return 12;
}

void ui_group(label_t *l, const char *text, float x, float y, float a) { ui_kicker(l, text, x, y, a); }
