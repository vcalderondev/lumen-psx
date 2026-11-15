/* Image helpers for files the user brings (photos of any size): decode, crop, scale down
 * and save. No GL here, so every function may run on a worker thread. */
#ifndef LUMEN_IMAGE_H
#define LUMEN_IMAGE_H
#include <SDL2/SDL.h>

#define IMAGE_MAX_BYTES (24L * 1024 * 1024)   /* bigger files are refused before decoding */

/* Decodes `path` into a new ABGR8888 surface (R,G,B,A bytes) whose longest side is at
 * most max_side (area-averaged; never scaled up). square = 1 crops the centre square
 * first. ow / oh (may be NULL) get the size of the file's image. NULL on failure. */
SDL_Surface *image_load_scaled(const char *path, int max_side, int square, int *ow, int *oh);

/* Writes a PNG through <path>.tmp + fsync + rename (never a half-written file). 0 / -1. */
int image_save_png(SDL_Surface *s, const char *path);

#endif
