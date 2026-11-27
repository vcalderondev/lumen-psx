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
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#ifdef LUMEN_ALSA   /* build.sh: the console; without it a named output falls back to the default */
#include <alsa/asoundlib.h>
#endif
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
/* The mixer (audio_cb) is called by SDL's audio thread, or by our own thread when the output
 * is an ALSA device written directly (direct_*, below). Its state is guarded by mix_mx. */
static SDL_mutex *mix_mx;
static void mix_lock(void) { if (mix_mx) SDL_LockMutex(mix_mx); }
static void mix_unlock(void) { if (mix_mx) SDL_UnlockMutex(mix_mx); }
enum { DIRECT_NONE, DIRECT_OPENING, DIRECT_RUNNING, DIRECT_FAILED };
static SDL_atomic_t direct_state, direct_quit, direct_gone;
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

/* ---- interface sounds (lumen-design guidelines/06 "Sound"): the console's own, mono ---- */

/* One struck or lit object (lumen-design, proposals/sounds "System"): a few partials that
 * are not harmonic, each with its own decay (the high ones die first), every partial as two
 * components a hair apart so the tone moves, a raised-cosine attack, and with `body` the
 * shell answering (150 Hz, 10 ms). Damped sines only: no noise. mat: {ratio, amp, decay s}. */
typedef struct { float ratio, amp, tau; } partial_t;
static const partial_t k_plastic[] = { { 1.00f, 1.00f, 0.011f }, { 1.83f, 0.55f, 0.007f }, { 2.92f, 0.30f, 0.005f }, { 4.60f, 0.14f, 0.003f } };
static const partial_t k_switch[]  = { { 1.00f, 1.00f, 0.016f }, { 2.21f, 0.50f, 0.010f }, { 3.72f, 0.28f, 0.006f }, { 5.90f, 0.12f, 0.004f } };
static const partial_t k_light[]   = { { 1.00f, 1.00f, 0.30f }, { 2.76f, 0.42f, 0.16f }, { 5.40f, 0.20f, 0.07f }, { 8.93f, 0.08f, 0.035f } };

static void fx_strike(float *b, int n, float at, float f, const partial_t *mat, float amp, float attack, float body)
{
   const int i0 = (int)(at * dev_rate), m = (int)(attack * dev_rate);
   int i, k;
   for (k = 0; k < 4; k++)
   {
      const float fk = f * mat[k].ratio;
      const double w = 6.2831853 * fk / dev_rate;
      int last = (int)(mat[k].tau * 9.0f * dev_rate);   /* under -78 dB after 9 decays */
      if (fk > 0.45f * dev_rate) continue;
      if (last > n - i0) last = n - i0;
      for (i = 0; i < last; i++)
      {  /* sin(1.0006 x) + sin(0.9994 x) = 2 sin(x) cos(0.0006 x) */
         float e = expf(-(float)i / (mat[k].tau * dev_rate)) * (i < m ? 0.5f - 0.5f * cosf(3.14159265f * i / m) : 1.0f);
         b[i0 + i] += amp * mat[k].amp * sinf((float)(w * i)) * cosf((float)(w * i * 0.0006)) * e;
      }
   }
   if (body > 0)
      for (i = 0; i < dev_rate / 10 && i0 + i < n; i++)
      {
         float t = (float)i / dev_rate;
         b[i0 + i] += amp * body * sinf(6.2831853f * 150.0f * t) * expf(-t / 0.010f) * (i < m ? 0.5f - 0.5f * cosf(3.14159265f * i / m) : 1.0f);
      }
}

static void sfx_make(int id)
{
   static const float len_s[SFX_COUNT] = { 0.08f, 0.08f, 0.08f, 0.20f, 0.20f, 0.22f, 0.22f, 0.22f, 0.22f, 0.14f, 1.00f };
   const int n = (int)(len_s[id] * dev_rate);
   float *b = calloc(n, sizeof(float)), peak = 0, want;
   int i;
   if (!b) return;
   switch (id)
   {
   /* a button of the console: the same tock, a little smaller to the left and for the focus */
   case SFX_MOVE_NEXT: fx_strike(b, n, 0, 1250, k_plastic, 1, 0.001f, 0.35f); break;
   case SFX_MOVE_PREV: fx_strike(b, n, 0, 1120, k_plastic, 1, 0.001f, 0.35f); break;
   case SFX_FOCUS:     fx_strike(b, n, 0, 950, k_plastic, 1, 0.001f, 0.40f); break;
   /* a page: the detent of a shoulder button, two tocks, rising for the next page */
   case SFX_PAGE_NEXT: fx_strike(b, n, 0, 820, k_plastic, 0.8f, 0.001f, 0.4f); fx_strike(b, n, 0.055f, 980, k_plastic, 1, 0.001f, 0.4f); break;
   case SFX_PAGE_PREV: fx_strike(b, n, 0, 980, k_plastic, 0.8f, 0.001f, 0.4f); fx_strike(b, n, 0.055f, 820, k_plastic, 1, 0.001f, 0.4f); break;
   /* a system: the big switch, press and release; brighter for the next system */
   case SFX_SYSTEM_NEXT: fx_strike(b, n, 0, 470, k_switch, 1, 0.001f, 0.8f); fx_strike(b, n, 0.070f, 610, k_switch, 0.45f, 0.001f, 0.3f); break;
   case SFX_SYSTEM_PREV: fx_strike(b, n, 0, 410, k_switch, 1, 0.001f, 0.8f); fx_strike(b, n, 0.070f, 530, k_switch, 0.45f, 0.001f, 0.3f); break;
   /* open: the lid's latch gives and the lid lands open, two dry sounds rising (no tone: a
    * tone here read as a notification); back: the lid shuts, one low one */
   case SFX_OPEN:      fx_strike(b, n, 0, 430, k_switch, 0.85f, 0.001f, 0.7f); fx_strike(b, n, 0.060f, 760, k_plastic, 1, 0.001f, 0.35f); break;
   case SFX_BACK:      fx_strike(b, n, 0, 360, k_switch, 1, 0.002f, 0.9f); break;
   /* confirm: a button pressed home, the tock with the switch under it: fuller and lower than
    * a move, and quieter (with a glint of the light on top it shouted) */
   case SFX_CONFIRM:   fx_strike(b, n, 0, 860, k_plastic, 0.8f, 0.0015f, 0.45f); fx_strike(b, n, 0.004f, 520, k_switch, 0.6f, 0.002f, 0.3f); break;
   /* play: the light blooms, partial after partial, on one pitch; then the launch's click */
   case SFX_PLAY:      fx_strike(b, n, 0, 185, k_light, 0.9f, 0.12f, 0); fx_strike(b, n, 0.10f, 370, k_light, 0.8f, 0.14f, 0);
                       fx_strike(b, n, 0.22f, 555, k_light, 0.6f, 0.16f, 0); fx_strike(b, n, 0.34f, 740, k_light, 0.5f, 0.18f, 0); break;
   }
   {  /* two early reflections: a surface near the object (back to front: no feedback) */
      const int d1 = dev_rate * 11 / 1000, d2 = dev_rate * 23 / 1000;
      for (i = n - 1; i >= d1; i--) b[i] += 0.11f * b[i - d1] + (i >= d2 ? 0.06f * b[i - d2] : 0.0f);
   }
   for (i = 0; i < n; i++)
   {  /* the last 25 ms fade out: an effect never ends on a cut */
      int left = n - i, f = dev_rate / 40;
      if (left < f) b[i] *= (float)left / f;
      if (fabsf(b[i]) > peak) peak = fabsf(b[i]);
   }
   /* peaks: a tick about -18 dBFS, the air a little under, the notes a little over */
   want = id <= SFX_FOCUS ? 0.125f : id == SFX_CONFIRM ? 0.11f : 0.15f;   /* confirm repeats on every value: the quietest */
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
   mix_lock();
   for (i = 0; i < NSLOT; i++)
   {
      if (slot[i].k < 0) { best = i; break; }
      if (slot_age[i] < slot_age[best]) best = i;
   }
   slot[best].k = k; slot[best].pos = 0; slot[best].gain = g;
   slot_age[best] = ++slot_clock;
   mix_unlock();
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

static void mix(Uint8 *stream, int len);
static void SDLCALL audio_cb(void *ud, Uint8 *stream, int len)
{  /* the default output stays open for good; while a named output plays the mix it is silent */
   (void)ud;
   if (SDL_AtomicGet(&direct_state) == DIRECT_RUNNING) { memset(stream, 0, (size_t)len); return; }
   mix_lock(); mix(stream, len); mix_unlock();
}
static void mix(Uint8 *stream, int len)
{
   short *out = (short *)stream;
   int n = len / 4, i, k;
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
   mix_lock();
   n = nfree;
   memcpy(tmp, freel, sizeof(src_t) * n);
   nfree = 0;
   for (i = 0; i < NV; i++) d |= vo[i].used && vo[i].dead;
   mix_unlock();
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
         mix_lock();
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
         mix_unlock();
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
   /* always the default output at start: a named device (run.sh may have exported AUDIODEV
    * for a Bluetooth headset) is taken by music_reopen(), which writes it directly */
   unsetenv("AUDIODEV"); unsetenv("SDL_AUDIO_ALSA_DEFAULT_DEVICE");
   if (!mix_mx) mix_mx = SDL_CreateMutex();
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

/* bluez-alsa's ALSA plugin needs libraries the console does not have (GLib and friends);
 * they are in the Bluetooth package, and the plugin's RUNPATH reaches only the ones it names
 * itself, not what those need in turn (libgmodule -> libglib -> libpcre2). Loaded here once,
 * dependencies first: the loader then finds them by name when ALSA opens the plugin. */
int music_bt_libs(void)
{
   static const char *const libs[] = { "libpcre2-8.so.0", "libffi.so.8", "libglib-2.0.so.0", "libgmodule-2.0.so.0",
                                       "libsbc.so.1", "libbluetooth.so.3" };
   static int done;
   char p[96];
   size_t i;
   if (done) return done > 0;
   done = 1;
   for (i = 0; i < sizeof(libs) / sizeof(libs[0]); i++)
   {
      FILE *f;
      snprintf(p, sizeof(p), "/tmp/lumen-bt/lib/%s", libs[i]);
      if (!(f = fopen(p, "rb"))) continue;   /* a package without it: the plugin does not need it either */
      fclose(f);
      if (!SDL_LoadObject(p)) { logf_("music: %s did not load: %s", p, SDL_GetError()); done = -1; }
   }
   return done > 0;
}

/* ---- direct ALSA output: a named device (a Bluetooth headset through bluez-alsa, a USB
 * card) is not opened through SDL, and not in this process at all. On the console SDL's
 * ALSA backend never gets a bluez-alsa device going (an endless underrun), and opening the
 * device inside the running dashboard dead-locks in the open, while a fresh process with
 * plain blocking writes plays. So a child ("lumen --audio-sink", music_sink_main) owns the
 * device and plays what it reads from a pipe; a thread here mixes and feeds the pipe, which
 * also paces it. A device that hangs costs a killed child, never the dashboard. */
#define SINK_FRAMES 1024
static char direct_name[96], direct_conf[128];
static SDL_atomic_t direct_pid;

int music_sink_main(const char *name, const char *conf, int rate)
{
#ifdef LUMEN_ALSA
   static short buf[SINK_FRAMES * 2];
   snd_pcm_t *pcm = NULL;
   int rc, errs = 0;
   if (!strncmp(name, "bluealsa:", 9)) music_bt_libs();
   if (conf && *conf) setenv("ALSA_CONFIG_PATH", conf, 1);
   if ((rc = snd_pcm_open(&pcm, name, SND_PCM_STREAM_PLAYBACK, 0)) < 0 ||
       (rc = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16, SND_PCM_ACCESS_RW_INTERLEAVED, 2, (unsigned)rate, 1, 200000)) < 0)
   {
      fprintf(stderr, "audio-sink: %s: %s\n", name, snd_strerror(rc));
      return 1;
   }
   if (write(3, "R", 1) != 1) return 1;   /* ready: the parent starts to feed */
   for (;;)
   {
      size_t got = 0;
      while (got < sizeof(buf))
      {
         ssize_t n = read(0, (char *)buf + got, sizeof(buf) - got);
         if (n <= 0) { snd_pcm_drop(pcm); snd_pcm_close(pcm); return 0; }   /* the parent closed: done */
         got += (size_t)n;
      }
      rc = (int)snd_pcm_writei(pcm, buf, SINK_FRAMES);
      if (rc < 0 && snd_pcm_recover(pcm, rc, 1) < 0)
      {  /* the device is gone (the headset left) */
         if (++errs > 20) { fprintf(stderr, "audio-sink: %s stopped: %s\n", name, snd_strerror(rc)); return 2; }
         usleep(20000);
      }
      else errs = 0;
   }
#else
   (void)name; (void)conf; (void)rate;
   return 1;
#endif
}

static int direct_main(void *unused)
{
#ifdef LUMEN_ALSA
   static short buf[SINK_FRAMES * 2];
   int data[2] = { -1, -1 }, st[2] = { -1, -1 }, ready = 0, i, wst;
   char rate[16], c;
   pid_t pid = -1;
   (void)unused;
   snprintf(rate, sizeof(rate), "%d", dev_rate);
   if (pipe(data) == 0 && pipe(st) == 0) pid = fork();
   if (pid == 0)
   {
      dup2(data[0], 0); dup2(st[1], 3);
      for (i = 4; i < 256; i++) close(i);
      execl("/proc/self/exe", "lumen", "--audio-sink", direct_name, direct_conf, rate, (char *)NULL);
      _exit(127);
   }
   if (pid < 0)
   {
      if (data[0] >= 0) { close(data[0]); close(data[1]); }
      if (st[0] >= 0) { close(st[0]); close(st[1]); }
      SDL_AtomicSet(&direct_state, DIRECT_FAILED); SDL_AtomicSet(&direct_gone, 1);
      return 0;
   }
   close(data[0]); close(st[1]);
   SDL_AtomicSet(&direct_pid, (int)pid);
#ifdef F_SETPIPE_SZ
   fcntl(data[1], F_SETPIPE_SZ, 4096);   /* one block of delay, not the pipe's 64 KB (0.37 s) */
#endif
   fcntl(st[0], F_SETFL, O_NONBLOCK);
   for (i = 0; i < 400 && !ready && !SDL_AtomicGet(&direct_quit); i++)   /* 8 s for the device to open */
   {
      if (read(st[0], &c, 1) == 1) { ready = 1; break; }
      if (waitpid(pid, &wst, WNOHANG) == pid) { pid = -1; break; }
      SDL_Delay(20);
   }
   close(st[0]);
   if (ready)
   {
      logf_("music: output %s (%d Hz)", direct_name, dev_rate);
      SDL_AtomicSet(&direct_state, DIRECT_RUNNING);
      while (!SDL_AtomicGet(&direct_quit))
      {
         size_t put = 0;
         mix_lock(); mix((Uint8 *)buf, (int)sizeof(buf)); mix_unlock();
         while (put < sizeof(buf))
         {  /* blocks until the child has played the block before: this is the pacing */
            ssize_t n = write(data[1], (char *)buf + put, sizeof(buf) - put);
            if (n <= 0) break;
            put += (size_t)n;
         }
         if (put < sizeof(buf)) { logf_("music: %s stopped", direct_name); break; }   /* the child left */
      }
   }
   else logf_("music: %s did not open", direct_name);
   close(data[1]);   /* the child sees the end and closes the device */
   if (pid > 0)
   {
      for (i = 0; i < 50 && waitpid(pid, &wst, WNOHANG) == 0; i++) SDL_Delay(20);
      if (i == 50) { kill(pid, SIGKILL); waitpid(pid, &wst, 0); }
   }
   SDL_AtomicSet(&direct_pid, 0);
   SDL_AtomicSet(&direct_state, SDL_AtomicGet(&direct_quit) ? DIRECT_NONE : DIRECT_FAILED);
#else
   (void)unused;
   SDL_AtomicSet(&direct_state, DIRECT_FAILED);
#endif
   SDL_AtomicSet(&direct_gone, 1);
   return 0;
}

/* stop the direct output; a child that does not take the last block is killed, which
 * frees the thread from its write */
static void direct_stop(void)
{
   int i, pid;
   if (SDL_AtomicGet(&direct_gone)) { SDL_AtomicSet(&direct_state, DIRECT_NONE); return; }
   SDL_AtomicSet(&direct_quit, 1);
   for (i = 0; i < 25 && !SDL_AtomicGet(&direct_gone); i++) SDL_Delay(20);
   if (!SDL_AtomicGet(&direct_gone) && (pid = SDL_AtomicGet(&direct_pid)) > 0) kill(pid, SIGKILL);
   for (i = 0; i < 100 && !SDL_AtomicGet(&direct_gone); i++) SDL_Delay(20);
   if (!SDL_AtomicGet(&direct_gone)) logf_("music: the direct output did not stop: left behind");
   SDL_AtomicSet(&direct_state, DIRECT_NONE);
}

/* Move the sound to another output while running (Settings > Sound > Audio output, a
 * Bluetooth headset that connects or goes). alsa_dev "" or NULL = the default (HDMI, through
 * SDL, open since start); a name = that ALSA device, through the sink above, with alsa_conf
 * as its ALSA_CONFIG_PATH (NULL = the system's). Returns at once: a named output is opened
 * in the background and the default keeps playing until it is ready (music_output_state).
 * What was playing goes on: the sources are not touched. -1 with no audio at all. */
int music_reopen(const char *alsa_dev, const char *alsa_conf)
{
   SDL_Thread *th;
   if (!dev) return -1;   /* no audio since start */
   logf_("music: moving the output to %s", alsa_dev && *alsa_dev ? alsa_dev : "default");
   if (SDL_AtomicGet(&direct_state) != DIRECT_NONE) direct_stop();
   if (!alsa_dev || !*alsa_dev) return 0;
   snprintf(direct_name, sizeof(direct_name), "%s", alsa_dev);
   snprintf(direct_conf, sizeof(direct_conf), "%s", alsa_conf ? alsa_conf : "");
   SDL_AtomicSet(&direct_quit, 0); SDL_AtomicSet(&direct_gone, 0); SDL_AtomicSet(&direct_state, DIRECT_OPENING);
   if ((th = SDL_CreateThread(direct_main, "audio-out", NULL))) SDL_DetachThread(th);
   else { SDL_AtomicSet(&direct_state, DIRECT_FAILED); SDL_AtomicSet(&direct_gone, 1); }
   return 0;
}

/* the named output: 0 none (the default plays), 1 opening, 2 playing, -1 it failed or its
 * device went away (the default plays again; music_reopen clears it) */
int music_output_state(void)
{
   switch (SDL_AtomicGet(&direct_state))
   {
      case DIRECT_OPENING: return 1;
      case DIRECT_RUNNING: return 2;
      case DIRECT_FAILED:  return -1;
      default:             return 0;
   }
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
   mix_lock();
   id = ++cur_id;
   for (k = 0; k < NV; k++)
      if (vo[k].used)
      {
         vo[k].target = 0;
         vo[k].gstep = fade_s > 0 ? 1.0f / (fade_s * dev_rate) : 1;
      }
   mix_unlock();
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
   mix_lock();
   mvol_t = v;
   mix_unlock();
}

void music_duck(float amount)
{
   if (!dev) return;
   amount = amount < 0 ? 0 : amount > 1 ? 1 : amount;
   if (amount == duck_c) return;
   duck_c = amount;
   mix_lock();
   duck_t = amount;
   mix_unlock();
}

void music_shutdown(void)
{
   int k;
   if (!dev) return;
   mix_lock();
   mvol_t = 0;                                 /* ramp to silence (0.2 s) instead of a click */
   mix_unlock();
   SDL_Delay(220);
   SDL_LockMutex(req_mx);
   quit = 1;
   SDL_CondSignal(req_cv);
   SDL_UnlockMutex(req_mx);
   SDL_WaitThread(loader, NULL);               /* waits out an in-flight file read */
   loader = NULL;
   if (SDL_AtomicGet(&direct_state) != DIRECT_NONE) direct_stop();
   if (dev) SDL_CloseAudioDevice(dev);         /* callback no longer runs */
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
