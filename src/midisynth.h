/* MIDI synthesizer for scene music: a C port of lumen-scenes/player/lumen-synth.mjs
 * (the reference implementation; same patches, drum kit, envelopes, echo and soft clip).
 * A Standard MIDI File is rendered whole to S16 stereo PCM at MIDISYNTH_RATE, on the music
 * loader thread; the track then loops back to its first CC 111 (or the start). */
#ifndef LUMEN_MIDISYNTH_H
#define LUMEN_MIDISYNTH_H

#define MIDISYNTH_RATE        32000
#define MIDISYNTH_MAX_SECONDS 300

/* 0 ok: *pcm (malloc'd, interleaved L R), *frames, *loop_start (frame).
 * -1 error: *why says what (static string). */
int midisynth_render(const unsigned char *data, long len, short **pcm, unsigned *frames,
                     unsigned *loop_start, const char **why);
extern int midisynth_capped;   /* the last render was cut at MIDISYNTH_MAX_SECONDS */

#endif
