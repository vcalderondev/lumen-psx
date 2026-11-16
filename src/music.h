/* Per-game background music (docs/dashboard-design.md "Per-game music").
 * One SDL audio device (44.1 kHz stereo S16), two voices crossfading. Files are read
 * whole into memory by a loader thread (the USB is exFAT over FUSE: no I/O in the audio
 * callback) and decoded in the callback: .ogg with stb_vorbis, .wav with SDL_LoadWAV
 * (converted once at load). Tracks loop. Every call is main-thread only and cheap.
 * If the audio device cannot be opened, everything is a logged no-op. */
#ifndef LUMEN_MUSIC_H
#define LUMEN_MUSIC_H

int  music_init(void);                          /* 0 ok, -1 no audio (no-ops from then on) */
void music_play(const char *path, float fade_s);/* crossfade to path ("" or NULL = fade to silence);
                                                   same path as the current target = no-op */
void music_set_volume(float v);                 /* 0..1 master, applied with a 0.2 s ramp */
void music_duck(float amount);                  /* 0..1 extra attenuation (panel open: 0.5), ramped */
/* one-shot effect: the click of a cartridge seating in its console. kind: 0 top-loader
 * (a firm latch and a low thud), 1 front-loader (latch with a spring), 2 handheld (a
 * small snap). Synthesised at init; mixed over the music at a fixed level (the music
 * volume setting does not mute it). */
void music_click(int kind);
/* interface sounds (guidelines/06 "Sound"), synthesised at init; up to four play at once
 * (a fifth replaces the oldest, clicks included). Gain: music_sfx_level (0 = off). */
enum { SFX_MOVE_NEXT, SFX_MOVE_PREV, SFX_FOCUS, SFX_PAGE_NEXT, SFX_PAGE_PREV,
       SFX_SYSTEM_NEXT, SFX_SYSTEM_PREV, SFX_OPEN, SFX_BACK, SFX_CONFIRM, SFX_PLAY, SFX_COUNT };
void music_sfx(int id);
void music_sfx_level(float v);                  /* 0..1; the Sound page: switch x volume */
void music_shutdown(void);                      /* stop, close the device, join the loader (before exit) */

#endif
