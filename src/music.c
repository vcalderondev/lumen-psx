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

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lumen.h"
#include "music.h"

#define MAX_FILE   (16 << 20)
#define SBUF_FR    512            /* source frames decoded per refill */
#define NFREE      8
#define NV         3              /* voices: one in, up to two still fading out */

typedef struct
{
   stb_vorbis *ov;                /* .ogg: decoder over mem */
   unsigned char *mem;            /* .ogg: whole file (owned, outlives ov) */
   short *pcm;                    /* .wav: S16 stereo at src rate */
   unsigned frames, ppos;         /* .wav: length, play position */
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
static float req_fade;
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
         if (v->s.ppos >= v->s.frames) v->s.ppos = 0;
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

/* ---- one-shot effects (cartridge clicks) ---- */
#define NCLICK 3
#define NFX    (NCLICK + SFX_COUNT)  /* clicks first, then the interface sounds */
#define NSLOT  4
static float *clk[NFX];           /* mono, dev_rate, S16 scale; written once at init */
static int clk_len[NFX];
static struct { int k, pos; float gain; } slot[NSLOT];   /* playing effects; under audio lock */
static unsigned slot_age[NSLOT], slot_clock;
static float ui_level = 0.6f;     /* main thread: interface sounds gain (0 = off) */

static unsigned rnd_state = 0x1234567u;
static float rnd(void) { rnd_state = rnd_state * 1664525u + 1013904223u; return (float)(rnd_state >> 8) / 8388608.0f - 1.0f; }

/* A plastic latch: a bright transient (filtered noise + a short resonance) and, a few ms
 * later, the body of the console answering (a damped low tone). */
static void click_make(int k)
{
   static const float res_hz[NCLICK] = { 2300, 2900, 4200 }, res_ms[NCLICK] = { 7, 5, 3 };
   static const float thud_hz[NCLICK] = { 120, 165, 320 }, thud_ms[NCLICK] = { 45, 30, 12 };
   static const float thud_a[NCLICK] = { 0.75f, 0.55f, 0.25f }, gap_ms[NCLICK] = { 9, 7, 3 };
   const int n = dev_rate * 12 / 100;   /* 120 ms */
   float *b = calloc(n, sizeof(float)), hp = 0, prev = 0, peak = 0;
   int i;
   if (!b) return;
   for (i = 0; i < n; i++)
   {
      float t = (float)i / dev_rate, x, nz = rnd();
      hp = 0.6f * (hp + nz - prev); prev = nz;               /* high-passed noise */
      x = hp * expf(-t / 0.0012f) * 0.9f;
      x += sinf(6.2832f * res_hz[k] * t) * expf(-t / (res_ms[k] * 0.001f)) * 0.5f;
      if (k == 1)   /* the spring of the front door: a faint ring after the latch */
         x += sinf(6.2832f * 1150.0f * t) * expf(-t / 0.025f) * 0.10f;
      {
         float t2 = t - gap_ms[k] * 0.001f;
         if (t2 >= 0)
            x += (sinf(6.2832f * thud_hz[k] * t2) + 0.3f * rnd() * expf(-t2 / 0.004f))
                 * expf(-t2 / (thud_ms[k] * 0.001f)) * thud_a[k] * (1.0f - expf(-t2 / 0.0008f));
      }
      b[i] = x;
      if (fabsf(x) > peak) peak = fabsf(x);
   }
   for (i = 0; i < n; i++) b[i] *= 0.80f * 32767.0f / (peak > 0 ? peak : 1);
   clk[k] = b; clk_len[k] = n;
}

/* ---- interface sounds (guidelines/06 "Sound"): short glides and air, mono ---- */

/* a sine gliding from f0 to f1 (exponentially), with an octave partial when rich,
 * a 2 ms attack and an exponential decay over dur */
static void fx_glide(float *b, int n, float at, float f0, float f1, float dur, float amp, int rich)
{
   int i, i0 = (int)(at * dev_rate), m = (int)(dur * dev_rate);
   double ph = 0;
   for (i = 0; i < m && i0 + i < n; i++)
   {
      float t = (float)i / dev_rate, f = f0 * powf(f1 / f0, t / dur);
      float e = expf(-t * 6.0f / dur) * (t < 0.002f ? t / 0.002f : 1.0f);
      ph += 6.2831853 * f / dev_rate;
      b[i0 + i] += (sinf((float)ph) + (rich ? 0.25f * sinf(2.0f * (float)ph) : 0.0f)) * e * amp;
   }
}

/* air: noise through a low-pass whose cutoff sweeps 600 Hz -> 4 kHz (or back), minus its
 * lowest part, under a sin^2 envelope */
static void fx_whoosh(float *b, int n, float dur, int up, float amp)
{
   int i, m = (int)(dur * dev_rate);
   float lp1 = 0, lp = 0, lo = 0;
   for (i = 0; i < m && i < n; i++)
   {
      float u = (float)i / m, x = rnd(), fc = up ? 600.0f * powf(6.67f, u) : 4000.0f * powf(0.15f, u);
      float a = 1.0f - expf(-6.2831853f * fc / dev_rate), e = sinf(3.1415927f * u);
      lp1 += a * (x - lp1); lp += a * (lp1 - lp);   /* 12 dB/octave: air, not hiss */
      lo += (1.0f - expf(-6.2831853f * 200.0f / dev_rate)) * (x - lo);
      b[i] += (lp - lo) * e * e * amp;
   }
}

/* a soft mallet note at `at`: the fundamental with a slight pitch fall into tune (15 ms),
 * a bright 4th partial that dies fast (the strike), a warm octave below, 1.5 ms attack */
static void fx_mallet(float *b, int n, float at, float f, float amp)
{
   int i, i0 = (int)(at * dev_rate);
   double ph = 0, ph4 = 0, phl = 0;
   for (i = 0; i0 + i < n; i++)
   {
      float t = (float)i / dev_rate, bend = 1.0f + 0.012f * expf(-t / 0.005f);
      float att = t < 0.0015f ? t / 0.0015f : 1.0f;
      ph += 6.2831853 * f * bend / dev_rate;
      ph4 += 6.2831853 * f * 4.0f / dev_rate;
      phl += 6.2831853 * f * 0.5f / dev_rate;
      b[i0 + i] += (sinf((float)ph) * expf(-t * 13.0f) + 0.16f * sinf((float)ph4) * expf(-t * 70.0f)
                    + 0.12f * sinf((float)phl) * expf(-t * 22.0f)) * att * amp;
   }
}

/* air through a band-pass (two one-poles) whose centre glides f0 -> f1, sin^2 envelope */
static void fx_air(float *b, int n, float at, float dur, float f0, float f1, float amp)
{
   int i, i0 = (int)(at * dev_rate), m = (int)(dur * dev_rate);
   float lo1 = 0, lo2 = 0, hi1 = 0, hi2 = 0;   /* two-pole each side: 12 dB/octave */
   for (i = 0; i < m && i0 + i < n; i++)
   {
      float u = (float)i / m, fc = f0 * powf(f1 / f0, u), x = rnd(), e = sinf(3.1415927f * u);
      float ah = 1.0f - expf(-6.2831853f * fc * 1.5f / dev_rate), al = 1.0f - expf(-6.2831853f * fc * 0.65f / dev_rate);
      hi1 += ah * (x - hi1); hi2 += ah * (hi1 - hi2);
      lo1 += al * (x - lo1); lo2 += al * (lo1 - lo2);
      b[i0 + i] += (hi2 - lo2) * e * e * amp;
   }
}

static void sfx_make(int id)
{
   static const float len_s[SFX_COUNT] = { 0.07f, 0.07f, 0.07f, 0.32f, 0.32f, 0.5f, 0.5f, 0.3f, 0.2f, 0.24f, 0.45f };
   const int n = (int)(len_s[id] * dev_rate);
   float *b = calloc(n, sizeof(float)), peak = 0, want;
   int i;
   if (!b) return;
   switch (id)
   {
   case SFX_MOVE_NEXT: fx_glide(b, n, 0, 1650, 1500, 0.045f, 1, 0); break;
   case SFX_MOVE_PREV: fx_glide(b, n, 0, 1500, 1350, 0.045f, 1, 0); break;
   case SFX_FOCUS:     fx_glide(b, n, 0, 1250, 1150, 0.040f, 1, 0); break;
   case SFX_PAGE_NEXT: fx_whoosh(b, n, 0.22f, 1, 1); break;
   case SFX_PAGE_PREV: fx_whoosh(b, n, 0.22f, 0, 1); break;
   /* the system: two mallet notes a fifth apart (E5, B5), up for the next, down for the
    * previous, over a breath of air gliding the same way */
   case SFX_SYSTEM_NEXT: fx_air(b, n, 0, 0.18f, 1200, 3000, 1.6f); fx_mallet(b, n, 0.02f, 659.3f, 1); fx_mallet(b, n, 0.075f, 987.8f, 0.85f); break;
   case SFX_SYSTEM_PREV: fx_air(b, n, 0, 0.18f, 3000, 1200, 1.6f); fx_mallet(b, n, 0.02f, 987.8f, 0.85f); fx_mallet(b, n, 0.075f, 659.3f, 1); break;
   case SFX_OPEN:      fx_glide(b, n, 0, 880, 880, 0.12f, 1, 1); fx_glide(b, n, 0.06f, 1320, 1320, 0.14f, 1, 1); break;
   case SFX_BACK:      fx_glide(b, n, 0, 990, 600, 0.12f, 1, 1); break;
   case SFX_CONFIRM:   fx_glide(b, n, 0, 660, 990, 0.16f, 1, 1); break;
   case SFX_PLAY:      fx_glide(b, n, 0, 523, 1046, 0.35f, 1, 1); break;
   }
   for (i = 0; i < n; i++)
   {  /* the last 25 ms fade out: an effect never ends on a cut */
      int left = n - i, f = dev_rate / 40;
      if (left < f) b[i] *= (float)left / f;
      if (fabsf(b[i]) > peak) peak = fabsf(b[i]);
   }
   /* peaks: a tick about -18 dBFS, the air a little under, the notes a little over */
   want = id <= SFX_FOCUS ? 0.125f : (id <= SFX_PAGE_PREV ? 0.11f : 0.15f);
   for (i = 0; i < n; i++) b[i] *= want * 32767.0f / (peak > 0 ? peak : 1);
   clk[NCLICK + id] = b; clk_len[NCLICK + id] = n;
}

/* LUMEN_SFX_DUMP=<dir>: every effect as <dir>/<name>.wav (mono S16 at the device rate),
 * to review the sounds or to put the very same sounds on a video */
static void sfx_dump(const char *dir)
{
   static const char *const names[NFX] = { "click-top", "click-front", "click-handheld",
      "move-next", "move-prev", "focus", "page-next", "page-prev", "system-next", "system-prev",
      "open", "back", "confirm", "play" };
   int k, i;
   if (!dir || !*dir) return;
   for (k = 0; k < NFX; k++)
   {
      char path[600];
      FILE *f;
      unsigned hdr[11], bytes = (unsigned)clk_len[k] * 2;
      if (!clk[k]) continue;
      snprintf(path, sizeof(path), "%s/%s.wav", dir, names[k]);
      if (!(f = fopen(path, "wb"))) { logf_("music: cannot write %s", path); continue; }
      hdr[0] = 0x46464952; hdr[1] = 36 + bytes; hdr[2] = 0x45564157; hdr[3] = 0x20746d66; hdr[4] = 16;
      hdr[5] = 1 | (1 << 16); hdr[6] = dev_rate; hdr[7] = dev_rate * 2; hdr[8] = 2 | (16 << 16);
      hdr[9] = 0x61746164; hdr[10] = bytes;   /* little-endian RIFF (both targets are LE) */
      fwrite(hdr, 4, 11, f);
      for (i = 0; i < clk_len[k]; i++)
      {
         float x = clk[k][i];
         short v = x > 32767 ? 32767 : x < -32768 ? -32768 : (short)x;
         fwrite(&v, 2, 1, f);
      }
      fclose(f);
   }
   logf_("music: effects written to %s", dir);
}

/* start effect k (index into clk) at gain g: a free slot, else the oldest one */
static void slot_start(int k, float g)
{
   int i, best = 0;
   SDL_LockAudioDevice(dev);
   for (i = 0; i < NSLOT; i++)
   {
      if (slot[i].k < 0) { best = i; break; }
      if (slot_age[i] < slot_age[best]) best = i;
   }
   slot[best].k = k; slot[best].pos = 0; slot[best].gain = g;
   slot_age[best] = ++slot_clock;
   SDL_UnlockAudioDevice(dev);
}

void music_click(int kind)
{
   if (!dev || kind < 0 || kind >= NCLICK || !clk[kind]) return;
   slot_start(kind, 1.0f);
}

void music_sfx(int id)
{
   if (!dev || id < 0 || id >= SFX_COUNT || !clk[NCLICK + id] || ui_level <= 0) return;
   slot_start(NCLICK + id, ui_level);
}

void music_sfx_level(float v) { ui_level = v < 0 ? 0 : v > 1 ? 1 : v; }

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
      for (k = 0; k < NSLOT; k++)
         if (slot[k].k >= 0)
         {  /* effects on top of the music, centred */
            float e = clk[slot[k].k][slot[k].pos] * slot[k].gain;
            l += e; r += e;
            if (++slot[k].pos >= clk_len[slot[k].k]) slot[k].k = -1;
         }
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
      float fade;
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
      nv.target = 1;
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
   {
      int k;
      for (k = 0; k < NSLOT; k++) slot[k].k = -1;
      for (k = 0; k < NCLICK; k++) click_make(k);
      for (k = 0; k < SFX_COUNT; k++) sfx_make(k);
      sfx_dump(getenv("LUMEN_SFX_DUMP"));
   }
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

void music_play(const char *path, float fade_s)
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
   {
      int k;
      for (k = 0; k < NFX; k++) { free(clk[k]); clk[k] = NULL; }
      for (k = 0; k < NSLOT; k++) slot[k].k = -1;
   }
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
