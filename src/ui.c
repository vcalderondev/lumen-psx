/* UI kit, see ui.h. */
#include <stdio.h>
#include <string.h>
#include "lumen.h"
#include "ui.h"
#include "theme.h"

static const float k_white[3] = { 1.0f, 1.0f, 1.0f };

void ui_fill(float x, float y, float w, float h, float radius, const float rgb[3], float a)
{
   const float c[4] = { rgb[0], rgb[1], rgb[2], a };
   if (a > 0.003f) ui_shape(x, y, w, h, radius, 0.0f, c);
}

void ui_scrim(float a)
{
   float c[4];
   if (a <= 0.003f) return;
   theme_color(TC_BACKDROP, c); c[3] = UI_SCRIM * a;
   draw_2d(0, 0, 0, W, H, c, 0);
}

void ui_card(float x, float y, float w, float h, float a)
{
   /* glass.card (white 9 %) over glass.base (the theme's card colour at 74 %), as one fill */
   const float base = 0.74f, tint = 0.09f, cover = 1.0f - (1.0f - base) * (1.0f - tint);
   float c[4];
   int i;
   if (a <= 0.003f) return;
   theme_color(TC_CARD, c);
   for (i = 0; i < 3; i++) c[i] = (c[i] * base * (1.0f - tint) + tint) / cover;
   c[3] = cover * a;
   ui_shadow(x, y, w, h, UI_CARD_R, 28, 12, 0.45f * a);
   draw_round_rect(x, y, w, h, UI_CARD_R, c, UI_EDGE * a);
}

void ui_rest(float x, float y, float w, float h, float radius, float a)
{
   const float edge[4] = { 1, 1, 1, UI_EDGE * a };
   if (a <= 0.003f) return;
   ui_fill(x, y, w, h, radius, k_white, UI_REST * a);
   ui_shape(x, y, w, h, radius, 1.0f, edge);
}

void ui_ring(float x, float y, float w, float h, float radius, float gap, float a)
{
   const float c[4] = { 1, 1, 1, 0.95f * a }, o = gap + 3.0f;
   if (a > 0.003f) ui_shape(x - o, y - o, w + 2 * o, h + 2 * o, radius + o, 3.0f, c);
}

void ui_focus(float x, float y, float w, float h, float radius, float a)
{
   if (a <= 0.003f) return;
   ui_shadow(x, y, w, h, radius, 12, 4, 0.35f * a);
   ui_fill(x, y, w, h, radius, k_white, UI_SELECTED * a);
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
