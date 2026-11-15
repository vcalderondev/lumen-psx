/* Image helpers, see image.h. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <SDL2/SDL_image.h>
#include "lumen.h"
#include "image.h"
#include "settings.h"

/* area average of the source rectangle (sx, sy, sw, sh) into a dw x dh surface; colours
 * are weighted by alpha so transparent pixels do not darken the edges */
static SDL_Surface *scale_down(SDL_Surface *src, int sx, int sy, int sw, int sh, int dw, int dh)
{
   /* ABGR8888 by its masks: SDL_CreateRGBSurfaceWithFormat is SDL 2.0.5, the console has 2.0.4 */
   SDL_Surface *dst = SDL_CreateRGBSurface(0, dw, dh, 32, 0x000000FFu, 0x0000FF00u, 0x00FF0000u, 0xFF000000u);
   int x, y;
   if (!dst) return NULL;
   for (y = 0; y < dh; y++)
   {
      int y0 = sy + (int)((long long)y * sh / dh), y1 = sy + (int)((long long)(y + 1) * sh / dh);
      unsigned char *out = (unsigned char *)dst->pixels + y * dst->pitch;
      if (y1 <= y0) y1 = y0 + 1;
      for (x = 0; x < dw; x++, out += 4)
      {
         int x0 = sx + (int)((long long)x * sw / dw), x1 = sx + (int)((long long)(x + 1) * sw / dw);
         unsigned long long r = 0, g = 0, b = 0, a = 0;
         unsigned n;
         int u, v;
         if (x1 <= x0) x1 = x0 + 1;
         n = (unsigned)((x1 - x0) * (y1 - y0));
         for (v = y0; v < y1; v++)
         {
            const unsigned char *p = (const unsigned char *)src->pixels + v * src->pitch + x0 * 4;
            for (u = x0; u < x1; u++, p += 4) { r += p[0] * p[3]; g += p[1] * p[3]; b += p[2] * p[3]; a += p[3]; }
         }
         if (a) { out[0] = (unsigned char)(r / a); out[1] = (unsigned char)(g / a); out[2] = (unsigned char)(b / a); }
         else out[0] = out[1] = out[2] = 0;
         out[3] = (unsigned char)(a / n);
      }
   }
   return dst;
}

SDL_Surface *image_load_scaled(const char *path, int max_side, int square, int *ow, int *oh)
{
   struct stat st;
   SDL_Surface *raw, *src, *dst;
   int sx = 0, sy = 0, sw, sh, dw, dh;
   if (ow) *ow = 0;
   if (oh) *oh = 0;
   if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > IMAGE_MAX_BYTES) return NULL;
   if (!(raw = IMG_Load(path))) return NULL;
   if (raw->w <= 0 || raw->h <= 0) { SDL_FreeSurface(raw); return NULL; }
   if (raw->format->format == SDL_PIXELFORMAT_ABGR8888) src = raw;
   else
   {
      src = SDL_ConvertSurfaceFormat(raw, SDL_PIXELFORMAT_ABGR8888, 0);
      SDL_FreeSurface(raw);
      if (!src) return NULL;
   }
   if (ow) *ow = src->w;
   if (oh) *oh = src->h;
   sw = src->w; sh = src->h;
   if (square)
   {
      if (sw > sh) { sx = (sw - sh) / 2; sw = sh; }
      else { sy = (sh - sw) / 2; sh = sw; }
   }
   dw = sw; dh = sh;
   if (max_side > 0 && (dw > max_side || dh > max_side))
   {
      if (dw >= dh) { dh = (int)((long long)dh * max_side / dw); dw = max_side; }
      else { dw = (int)((long long)dw * max_side / dh); dh = max_side; }
      if (dw < 1) dw = 1;
      if (dh < 1) dh = 1;
   }
   if (dw == src->w && dh == src->h) return src;   /* nothing to crop or scale */
   if (SDL_MUSTLOCK(src)) SDL_LockSurface(src);
   dst = scale_down(src, sx, sy, sw, sh, dw, dh);
   if (SDL_MUSTLOCK(src)) SDL_UnlockSurface(src);
   SDL_FreeSurface(src);
   return dst;
}

int image_save_png(SDL_Surface *s, const char *path)
{
   char tmp[600], dir[600], *slash;
   int fd, ok;
   if (!s || snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp)) return -1;
   if (IMG_SavePNG(s, tmp) != 0) { logf_("image: cannot write %s: %s", tmp, SDL_GetError()); unlink(tmp); return -1; }
   ok = (fd = open(tmp, O_RDONLY)) >= 0;
   if (ok) { ok = fsync(fd) == 0; close(fd); }
   if (ok) ok = rename(tmp, path) == 0;
   if (!ok) { logf_("image: write %s failed: %s", path, strerror(errno)); unlink(tmp); return -1; }
   snprintf(dir, sizeof(dir), "%s", path);
   if ((slash = strrchr(dir, '/'))) { *slash = 0; settings_fsync_dir(dir); }
   return 0;
}
