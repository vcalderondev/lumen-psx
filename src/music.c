/* Per-game background music (music.h). stb_vorbis v1.24, vendored from
 * github.com/nothings/stb commit 9f755b60 (2026-11-12), third_party/stb_vorbis.c.
 *
 * Threads: main (music_*), loader (file I/O + decoder setup), SDL audio callback
 * (decode + mix). Callback state (voices, gains, free list, cur_id) is guarded by
 * SDL_LockAudioDevice; the loader mailbox by req_mx/req_cv. The callback never
 * allocates, frees or does I/O: retired voices go to a free list that the main
 * thread and the loader drain. Sources are resampled linearly to the device rate
 * and mixed to stereo (stb_vorbis downmixes, SDL_AudioCVT converts WAV). */
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-value"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#include "stb_vorbis.c"
#pragma GCC diagnostic pop

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lumen.h"
#include "music.h"
#include "midisynth.h"

#define MAX_FILE   (16 << 20)
#define SBUF_FR    512            /* source frames decoded per refill */
#define NFREE      8
#define NV         3              /* voices: one in, up to two still fading out */

typedef struct
{
   stb_vorbis *ov;                /* .ogg: decoder over mem */
   unsigned char *mem;            /* .ogg: whole file (owned, outlives ov) */
   short *pcm;                    /* .wav / .mid: S16 stereo at src rate */
   unsigned frames, ppos;         /* .wav / .mid: length, play position */
   unsigned loop;                 /* frame the track loops back to (.mid: first CC 111) */
   int rate;                      /* source rate */
} src_t;

typedef struct
{
   int used, dead;
   src_t s;
   unsigned step, frac;           /* 16.16 source frames per output frame */
   float a[2], b[2];              /* interpolation pair */
   short sbuf[SBUF_FR * 2];
   int sn, si;                    /* frames in sbuf, read index */
   float gain, target, gstep;     /* envelope, per output frame */
} voice_t;

static SDL_AudioDeviceID dev;
static int dev_rate = 44100;
static voice_t vo[NV];
static src_t freel[NFREE];
static int nfree, dead_seen;
static unsigned cur_id;           /* latest request; under audio lock */
static float mvol = 1, mvol_t = 1, duck = 0, duck_t = 0, rstep;

static SDL_Thread *loader;
static SDL_mutex *req_mx;
static SDL_cond *req_cv;
static char req_path[512];
static unsigned req_id;
static float req_fade, req_gain = 1;
static int req_pending, quit;

static char target[512];          /* main thread: current target path */
static char failed[512];          /* last path that failed (log once) */
static float vol_c = 1, duck_c = 0;  /* main thread: last values sent to the callback */

static void src_free(src_t *s)
{
   if (s->ov) stb_vorbis_close(s->ov);
   free(s->mem);
   free(s->pcm);
   memset(s, 0, sizeof *s);
}

/* ---- audio thread ---- */

static int refill(voice_t *v)
{
   int tries;
   for (tries = 0; tries < 2; tries++)
   {
      int n = 0;
      if (v->s.ov)
      {
         n = stb_vorbis_get_samples_short_interleaved(v->s.ov, 2, v->sbuf, SBUF_FR * 2);
         if (n <= 0)
         {
            stb_vorbis_seek_start(v->s.ov);
            continue;
         }
      }
      else if (v->s.pcm && v->s.frames)
      {
         unsigned left = v->s.frames - v->s.ppos;
         n = left < SBUF_FR ? (int)left : SBUF_FR;
         memcpy(v->sbuf, v->s.pcm + (size_t)v->s.ppos * 2, (size_t)n * 4);
         v->s.ppos += n;
         if (v->s.ppos >= v->s.frames) v->s.ppos = v->s.loop < v->s.frames ? v->s.loop : 0;
      }
      if (n > 0)
      {
         v->sn = n;
         v->si = 0;
         return 1;
      }
   }
   v->dead = 1;
   return 0;
}

static void next_frame(voice_t *v)
{
   v->a[0] = v->b[0];
   v->a[1] = v->b[1];
   if (v->si >= v->sn && (v->dead || !refill(v)))
   {
      v->b[0] = v->b[1] = 0;
      return;
   }
   v->b[0] = v->sbuf[v->si * 2];
   v->b[1] = v->sbuf[v->si * 2 + 1];
   v->si++;
}

static float ramp(float x, float t, float st)
{
   if (x < t) return x + st < t ? x + st : t;
   if (x > t) return x - st > t ? x - st : t;
   return x;
}

static void SDLCALL audio_cb(void *ud, Uint8 *stream, int len)
{
   short *out = (short *)stream;
   int n = len / 4, i, k;
   (void)ud;
   for (i = 0; i < n; i++)
   {
      float l = 0, r = 0, m;
      for (k = 0; k < NV; k++)
      {
         voice_t *v = &vo[k];
         float f;
         if (!v->used) continue;
         v->gain = ramp(v->gain, v->target, v->gstep);
         if (v->gain <= 0 && v->target <= 0)
         {
            /* faded out: hand the source back; keep it (silent) if the list is full */
            if (nfree < NFREE)
            {
               freel[nfree++] = v->s;
               memset(&v->s, 0, sizeof v->s);
               v->used = 0;
            }
            continue;
         }
         f = (float)v->frac * (1.0f / 65536.0f);
         l += (v->a[0] + (v->b[0] - v->a[0]) * f) * v->gain;
         r += (v->a[1] + (v->b[1] - v->a[1]) * f) * v->gain;
         v->frac += v->step;
         while (v->frac >= 65536)
         {
            v->frac -= 65536;
            next_frame(v);
         }
      }
      mvol = ramp(mvol, mvol_t, rstep);
      duck = ramp(duck, duck_t, rstep);
      m = mvol * (1 - duck);
      l *= m;
      r *= m;
      out[i * 2]     = l > 32767 ? 32767 : l < -32768 ? -32768 : (short)l;
      out[i * 2 + 1] = r > 32767 ? 32767 : r < -32768 ? -32768 : (short)r;
   }
}

/* ---- main / loader ---- */

/* drain the free list (takes the audio lock briefly; frees outside it) */
static void reap(void)
{
   src_t tmp[NFREE];
   int n, i, d = 0;
   SDL_LockAudioDevice(dev);
   n = nfree;
   memcpy(tmp, freel, sizeof(src_t) * n);
   nfree = 0;
   for (i = 0; i < NV; i++) d |= vo[i].used && vo[i].dead;
   SDL_UnlockAudioDevice(dev);
   for (i = 0; i < n; i++) src_free(&tmp[i]);
   if (d && !dead_seen) logf_("music: decode error mid-stream, voice silenced");
   dead_seen = d;
}

static unsigned char *read_file(const char *path, long *len)
{
   FILE *f = fopen(path, "rb");
   unsigned char *b = NULL;
   long n;
   if (!f) return NULL;
   if (fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) > 0 && n <= MAX_FILE && fseek(f, 0, SEEK_SET) == 0)
   {
      b = malloc(n);
      if (b && fread(b, 1, n, f) != (size_t)n)
      {
         free(b);
         b = NULL;
      }
      *len = n;
   }
   fclose(f);
   return b;
}

/* 0 ok; on error logs (once per path) and leaves s zeroed */
static int load(const char *path, src_t *s)
{
   long len = 0;
   const char *why = "read failed (missing, empty or > 16 MB)";
   unsigned char *b = read_file(path, &len);
   memset(s, 0, sizeof *s);
   if (b && len >= 12 && !memcmp(b, "OggS", 4))
   {
      int err = 0;
      s->ov = stb_vorbis_open_memory(b, (int)len, &err, NULL);
      if (s->ov)
      {
         stb_vorbis_info in = stb_vorbis_get_info(s->ov);
         s->mem = b;
         s->rate = in.sample_rate;
         if (in.channels >= 1 && s->rate >= 4000 && s->rate <= 192000) return 0;
         src_free(s);
         b = NULL;
         why = "unsupported vorbis stream";
      }
      else
         why = "stb_vorbis open failed";
   }
   else if (b && len >= 12 && !memcmp(b, "RIFF", 4))
   {
      SDL_AudioSpec sp;
      Uint8 *wb = NULL;
      Uint32 wl = 0;
      why = "SDL_LoadWAV failed";
      if (SDL_LoadWAV_RW(SDL_RWFromConstMem(b, (int)len), 1, &sp, &wb, &wl))
      {
         SDL_AudioCVT cvt;
         why = "WAV conversion failed";
         if (sp.freq >= 4000 && sp.freq <= 192000
             && SDL_BuildAudioCVT(&cvt, sp.format, sp.channels, sp.freq, AUDIO_S16SYS, 2, sp.freq) >= 0
             && (cvt.buf = malloc((size_t)wl * (cvt.len_mult > 0 ? cvt.len_mult : 1))) != NULL)
         {
            memcpy(cvt.buf, wb, wl);
            cvt.len = (int)wl;
            if (SDL_ConvertAudio(&cvt) == 0 && cvt.len_cvt >= 4)
            {
               s->pcm = (short *)cvt.buf;
               s->frames = (unsigned)(cvt.len_cvt / 4);
               s->rate = sp.freq;
            }
            else
               free(cvt.buf);
         }
         SDL_FreeWAV(wb);
      }
      free(b);
      b = NULL;
      if (s->pcm) return 0;
   }
   else if (b && len >= 14 && !memcmp(b, "MThd", 4))
   {
      /* MIDI (scene music): rendered whole by the synth here, on the loader thread */
      double t0 = SDL_GetTicks();
      if (midisynth_render(b, len, &s->pcm, &s->frames, &s->loop, &why) == 0 && s->frames)
      {
         s->rate = MIDISYNTH_RATE;
         if (midisynth_capped) logf_("music: %s: longer than %d s, cut there", path, MIDISYNTH_MAX_SECONDS);
         logf_("music: %s: MIDI rendered, %.1f s, loop at %.1f s (%.0f ms)", path,
               s->frames / (double)MIDISYNTH_RATE, s->loop / (double)MIDISYNTH_RATE, SDL_GetTicks() - t0);
         free(b);
         return 0;
      }
      if (s->pcm && !s->frames) why = "MIDI file has no notes";
      free(s->pcm);
      s->pcm = NULL;
   }
   else if (b)
      why = len >= 4 && !memcmp(b, "fLaC", 4) ? "FLAC not supported" : "unknown format";
   free(b);
   if (strcmp(failed, path))
   {
      snprintf(failed, sizeof failed, "%s", path);
      logf_("music: %s: %s", path, why);
   }
   return -1;
}

static int SDLCALL loader_main(void *ud)
{
   (void)ud;
   for (;;)
   {
      char path[512];
      unsigned id;
      float fade, gain;
      src_t s, old, tmp[NFREE];
      static voice_t nv;                       /* loader-only scratch */
      int rate, ok = 0, k, n = 0, tries;
      SDL_LockMutex(req_mx);
      while (!req_pending && !quit) SDL_CondWait(req_cv, req_mx);
      if (quit)
      {
         SDL_UnlockMutex(req_mx);
         break;
      }
      memcpy(path, req_path, sizeof path);
      id = req_id;
      fade = req_fade;
      gain = req_gain;
      req_pending = 0;
      SDL_UnlockMutex(req_mx);

      if (load(path, &s)) continue;
      memset(&old, 0, sizeof old);
      rate = s.rate;
      memset(&nv, 0, sizeof nv);               /* build and prime outside the lock */
      nv.used = 1;
      nv.s = s;
      nv.step = (unsigned)(((unsigned long long)rate << 16) / dev_rate);
      if (!nv.step) nv.step = 1;
      nv.target = gain;
      nv.gstep = fade > 0 ? 1.0f / (fade * dev_rate) : 1;
      next_frame(&nv);
      for (tries = 0; ; )
      {
         voice_t *v = NULL, *q = NULL;
         int stop;
         SDL_LockMutex(req_mx); stop = quit; SDL_UnlockMutex(req_mx);
         SDL_LockAudioDevice(dev);
         if (id == cur_id && !stop && ++tries <= 100)   /* ~0.5 s at most: then the track is dropped */
         {
            for (k = 0; k < NV && !v; k++)
               if (!vo[k].used || (vo[k].gain <= 0 && vo[k].target <= 0))
                  v = &vo[k];                  /* free, or silent (free list was full) */
               else if (!q || vo[k].gain < q->gain)
                  q = &vo[k];
            if (v)
            {
               if (v->used) old = v->s;
               *v = nv;
               memset(&s, 0, sizeof s);
               ok = 1;
               n = nfree;                      /* drain the free list while here */
               memcpy(tmp, freel, sizeof(src_t) * n);
               nfree = 0;
            }
            else
            {
               /* all audible (rapid switching): speed up the quietest's fade-out
                  to ~10 ms instead of cutting it, then retry */
               float fast = 1.0f / (0.01f * dev_rate);
               q->target = 0;
               if (q->gstep < fast) q->gstep = fast;
            }
         }
         else
            v = &nv;                           /* superseded: drop */
         SDL_UnlockAudioDevice(dev);
         if (v) break;
         SDL_Delay(5);
      }
      src_free(&old);
      for (k = 0; k < n; k++) src_free(&tmp[k]);
      src_free(&s);                            /* superseded while loading: dropped */
      logf_("music: %s %s (%d Hz)", ok ? "playing" : "dropped", path, rate);
   }
   return 0;
}

int music_init(void)
{
   SDL_AudioSpec want, got;
   if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0)
   {
      logf_("music: SDL audio init failed: %s", SDL_GetError());
      return -1;
   }
   logf_("music: audio driver %s", SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "(none)");
   memset(&want, 0, sizeof want);
   want.freq = 44100;
   want.format = AUDIO_S16SYS;
   want.channels = 2;
   want.samples = 2048;
   want.callback = audio_cb;
   dev = SDL_OpenAudioDevice(NULL, 0, &want, &got, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
   if (!dev)
   {
      logf_("music: open audio failed: %s", SDL_GetError());
      SDL_QuitSubSystem(SDL_INIT_AUDIO);
      return -1;
   }
   dev_rate = got.freq > 0 ? got.freq : 44100;
   rstep = 1.0f / (0.2f * dev_rate);
   logf_("music: device %d Hz, %d ch, %d samples", got.freq, got.channels, got.samples);
   req_mx = SDL_CreateMutex();
   req_cv = SDL_CreateCond();
   loader = req_mx && req_cv ? SDL_CreateThread(loader_main, "music", NULL) : NULL;
   if (!loader)
   {
      logf_("music: loader thread failed: %s", SDL_GetError());
      SDL_CloseAudioDevice(dev);
      dev = 0;
      if (req_cv) SDL_DestroyCond(req_cv);
      if (req_mx) SDL_DestroyMutex(req_mx);
      req_cv = NULL;
      req_mx = NULL;
      SDL_QuitSubSystem(SDL_INIT_AUDIO);
      return -1;
   }
   SDL_PauseAudioDevice(dev, 0);
   return 0;
}

void music_play(const char *path, float fade_s) { music_play_gain(path, fade_s, 1.0f); }

void music_play_gain(const char *path, float fade_s, float gain)
{
   int k;
   unsigned id;
   if (!dev) return;
   if (!path) path = "";
   if (!strcmp(path, target)) return;        /* before reap: no audio lock per frame */
   reap();
   snprintf(target, sizeof target, "%s", path);
   SDL_LockAudioDevice(dev);
   id = ++cur_id;
   for (k = 0; k < NV; k++)
      if (vo[k].used)
      {
         vo[k].target = 0;
         vo[k].gstep = fade_s > 0 ? 1.0f / (fade_s * dev_rate) : 1;
      }
   SDL_UnlockAudioDevice(dev);
   SDL_LockMutex(req_mx);
   if (*path)
   {
      snprintf(req_path, sizeof req_path, "%s", path);
      req_id = id;
      req_fade = fade_s;
      req_gain = gain < 0 ? 0 : gain > 1 ? 1 : gain;
      req_pending = 1;
      SDL_CondSignal(req_cv);
   }
   else
      req_pending = 0;                         /* silence: drop any queued request */
   SDL_UnlockMutex(req_mx);
}

void music_set_volume(float v)
{
   if (!dev) return;
   v = v < 0 ? 0 : v > 1 ? 1 : v;
   if (v == vol_c) return;
   vol_c = v;
   SDL_LockAudioDevice(dev);
   mvol_t = v;
   SDL_UnlockAudioDevice(dev);
}

void music_duck(float amount)
{
   if (!dev) return;
   amount = amount < 0 ? 0 : amount > 1 ? 1 : amount;
   if (amount == duck_c) return;
   duck_c = amount;
   SDL_LockAudioDevice(dev);
   duck_t = amount;
   SDL_UnlockAudioDevice(dev);
}

void music_shutdown(void)
{
   int k;
   if (!dev) return;
   SDL_LockAudioDevice(dev);
   mvol_t = 0;                                 /* ramp to silence (0.2 s) instead of a click */
   SDL_UnlockAudioDevice(dev);
   SDL_Delay(220);
   SDL_LockMutex(req_mx);
   quit = 1;
   SDL_CondSignal(req_cv);
   SDL_UnlockMutex(req_mx);
   SDL_WaitThread(loader, NULL);               /* waits out an in-flight file read */
   loader = NULL;
   SDL_CloseAudioDevice(dev);                  /* callback no longer runs */
   for (k = 0; k < NV; k++) src_free(&vo[k].s);
   for (k = 0; k < nfree; k++) src_free(&freel[k]);
   memset(vo, 0, sizeof vo);
   nfree = 0;
   SDL_DestroyCond(req_cv);
   SDL_DestroyMutex(req_mx);
   req_cv = NULL;
   req_mx = NULL;
   dev = 0;
   target[0] = 0;
   mvol = mvol_t = vol_c = 1;
   duck = duck_t = duck_c = 0;
   SDL_QuitSubSystem(SDL_INIT_AUDIO);
}
