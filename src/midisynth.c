/* MIDI synthesizer (midisynth.h). Mirrors lumen-scenes/player/lumen-synth.mjs line by line:
 * any change there must be made here too (and the other way round). Doubles throughout, like
 * the JavaScript; per-sample pow() calls of the reference are cached (same values). */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "midisynth.h"

#define MAX_VOICES 32
#define TAU 6.283185307179586

int midisynth_capped;

/* ---- Standard MIDI File parser ---- */

enum { EV_TEMPO, EV_END, EV_ON, EV_OFF, EV_CC, EV_BEND, EV_PROG };
typedef struct { unsigned tick, ord; int type, ch, a, b, tempo; } ev_t;

typedef struct { ev_t *e; int n, cap; } evlist_t;

static int ev_push(evlist_t *l, ev_t e)
{
   if (l->n == l->cap)
   {
      int nc = l->cap ? l->cap * 2 : 1024;
      ev_t *ne = realloc(l->e, sizeof(ev_t) * nc);
      if (!ne) return -1;
      l->e = ne; l->cap = nc;
   }
   e.ord = (unsigned)l->n;
   l->e[l->n++] = e;
   return 0;
}

static int ev_cmp(const void *a, const void *b)
{
   const ev_t *x = a, *y = b;
   if (x->tick != y->tick) return x->tick < y->tick ? -1 : 1;
   return x->ord < y->ord ? -1 : x->ord > y->ord;
}

static int parse_midi(const unsigned char *u8, long len, int *division, evlist_t *out, const char **why)
{
   long p = 0, hlen;
   int format, ntracks, t;
#define U32(q) ((unsigned)u8[q] << 24 | (unsigned)u8[(q) + 1] << 16 | (unsigned)u8[(q) + 2] << 8 | (unsigned)u8[(q) + 3])
   if (len < 14 || memcmp(u8, "MThd", 4)) { *why = "not a MIDI file (no MThd)"; return -1; }
   hlen = U32(4);
   format = u8[8] << 8 | u8[9]; ntracks = u8[10] << 8 | u8[11]; *division = u8[12] << 8 | u8[13];
   p = 8 + hlen;
   if (*division & 0x8000) { *why = "SMPTE time division is not supported"; return -1; }
   if (format > 1) { *why = "MIDI format 2 is not supported"; return -1; }
   if (*division == 0) { *why = "zero time division"; return -1; }
   for (t = 0; t < ntracks && p + 8 <= len; t++)
   {
      int is_trk = !memcmp(u8 + p, "MTrk", 4);
      long tl = U32(p + 4), end;
      unsigned tick = 0;
      int status = 0;
      p += 8;
      end = p + tl < len ? p + tl : len;
      if (!is_trk) { p = end; continue; }
      while (p < end)
      {
         unsigned v = 0; int b;
         ev_t e;
         do { b = u8[p++]; v = (v << 7) | (b & 0x7f); } while ((b & 0x80) && p < end);
         tick += v;
         if (p >= end) break;
         b = u8[p];
         if (b & 0x80) { status = b; p++; }
         else if (!status) { *why = "running status without a status byte"; return -1; }
         memset(&e, 0, sizeof e);
         e.tick = tick;
         if (status == 0xff)
         {
            int type = p < end ? u8[p++] : 0; unsigned l = 0;
            do { b = p < end ? u8[p++] : 0; l = (l << 7) | (b & 0x7f); } while ((b & 0x80) && p < end);
            if (type == 0x51 && l == 3 && p + 3 <= len) { e.type = EV_TEMPO; e.tempo = u8[p] << 16 | u8[p + 1] << 8 | u8[p + 2]; if (ev_push(out, e)) goto oom; }
            else if (type == 0x2f) { e.type = EV_END; if (ev_push(out, e)) goto oom; }
            p += l;
            status = 0;
         }
         else if (status == 0xf0 || status == 0xf7)
         {
            unsigned l = 0;
            do { b = p < end ? u8[p++] : 0; l = (l << 7) | (b & 0x7f); } while ((b & 0x80) && p < end);
            p += l;
            status = 0;
         }
         else
         {
            int hi = status & 0xf0, ch = status & 0x0f;
            e.ch = ch;
            if (hi == 0x80 || hi == 0x90 || hi == 0xa0 || hi == 0xb0 || hi == 0xe0)
            {
               int a, c;
               if (p + 2 > len) break;
               a = u8[p++]; c = u8[p++];
               if (hi == 0x90 && c > 0) { e.type = EV_ON; e.a = a; e.b = c; }
               else if (hi == 0x80 || hi == 0x90) { e.type = EV_OFF; e.a = a; }
               else if (hi == 0xb0) { e.type = EV_CC; e.a = a; e.b = c; }
               else if (hi == 0xe0) { e.type = EV_BEND; e.a = ((c << 7) | a) - 8192; }
               else continue;
               if (ev_push(out, e)) goto oom;
            }
            else if (hi == 0xc0 || hi == 0xd0)
            {
               int a;
               if (p + 1 > len) break;
               a = u8[p++];
               if (hi == 0xc0) { e.type = EV_PROG; e.a = a; if (ev_push(out, e)) goto oom; }
            }
            else { *why = "bad MIDI status"; return -1; }
         }
      }
      p = end;
   }
   qsort(out->e, out->n, sizeof(ev_t), ev_cmp);
   return 0;
oom:
   *why = "out of memory";
   return -1;
#undef U32
}

/* ---- instruments (lumen-synth.mjs PATCHES) ---- */
typedef struct { int w; double a, d, s, r, lp, det, h, g; } patch_t;
static const patch_t PATCHES[16] = {
   /* 0 piano      */ { 1, 0.003, 1.6, 0.0, 0.35, 0, 0, 0.25, 0.9 },
   /* 1 chrom perc */ { 0, 0.001, 0.9, 0.0, 0.5, 0, 0, 0.4, 0.7 },
   /* 2 organ      */ { 3, 0.01, 0.1, 0.8, 0.12, 1800, 0, 0, 0.3 },
   /* 3 guitar     */ { 2, 0.002, 0.7, 0.0, 0.25, 2200, 0, 0, 0.45 },
   /* 4 bass       */ { 1, 0.004, 0.5, 0.6, 0.12, 0, 0, 0, 1.0 },
   /* 5 strings    */ { 2, 0.25, 0.5, 0.8, 0.6, 2600, 8, 0, 0.3 },
   /* 6 ensemble   */ { 2, 0.35, 0.5, 0.85, 0.9, 2000, 10, 0, 0.28 },
   /* 7 brass      */ { 2, 0.05, 0.3, 0.7, 0.2, 1600, 4, 0, 0.35 },
   /* 8 reed       */ { 3, 0.03, 0.3, 0.7, 0.15, 1400, 0, 0, 0.3 },
   /* 9 pipe       */ { 0, 0.06, 0.2, 0.85, 0.25, 0, 3, 0, 0.75 },
   /* 10 lead      */ { 3, 0.01, 0.2, 0.7, 0.15, 3000, 6, 0, 0.28 },
   /* 11 pad       */ { 2, 0.8, 1.0, 0.8, 1.5, 1200, 12, 0, 0.3 },
   /* 12 fx        */ { 1, 0.3, 0.8, 0.6, 1.2, 0, 9, 0, 0.5 },
   /* 13 ethnic    */ { 4, 0.002, 0.6, 0.0, 0.3, 2500, 0, 0, 0.4 },
   /* 14 percussive*/ { 0, 0.001, 0.3, 0.0, 0.15, 0, 0, 0, 0.9 },
   /* 15 sound fx  */ { 5, 0.05, 0.5, 0.3, 0.5, 3000, 0, 0, 0.25 },
};

/* drum kit: kind 0 kick, 1 snare, 2 hat, 3 cymbal, 4 tom, 5 other; decay s, gain, base Hz */
static void drum_for(int note, int *kind, double *dec, double *g, double *hz)
{
   int k = 5; double d = 0.08, gg = 0.3, f = 0;
   switch (note)
   {
   case 35: case 36: k = 0; d = 0.3; gg = 1.0; f = 120; break;
   case 37: case 39: k = 5; d = 0.08; gg = 0.35; break;
   case 38: case 40: k = 1; d = 0.15; gg = 0.7; f = 190; break;
   case 42: case 44: k = 2; d = 0.04; gg = 0.3; break;
   case 46: k = 2; d = 0.25; gg = 0.3; break;
   case 49: case 52: case 55: case 57: k = 3; d = 1.0; gg = 0.3; break;
   case 51: case 53: case 59: k = 3; d = 0.45; gg = 0.22; break;
   case 41: k = 4; d = 0.3; gg = 0.8; f = 90; break;
   case 43: k = 4; d = 0.3; gg = 0.8; f = 110; break;
   case 45: k = 4; d = 0.3; gg = 0.8; f = 130; break;
   case 47: k = 4; d = 0.3; gg = 0.8; f = 150; break;
   case 48: k = 4; d = 0.3; gg = 0.8; f = 170; break;
   case 50: k = 4; d = 0.3; gg = 0.8; f = 200; break;
   default: break;
   }
   *kind = k; *dec = d; *g = gg; *hz = f;
}

static unsigned rng_x;
static double noise(void)
{
   rng_x ^= rng_x << 13; rng_x ^= rng_x >> 17; rng_x ^= rng_x << 5;
   return rng_x / 4294967296.0;
}

static double osc(int w, double p)
{
   switch (w)
   {
   case 0: return sin(TAU * p);
   case 1: return 1 - 4 * fabs(p - 0.5);
   case 2: return 2 * p - 1;
   case 3: return p < 0.5 ? 1 : -1;
   case 4: return p < 0.25 ? 1 : -1;
   default: return noise() * 2 - 1;
   }
}

static double softclip(double x) { return x >= 1 ? 1 : x <= -1 ? -1 : 1.5 * x - 0.5 * x * x * x; }

typedef struct
{
   int ch, note, stage, held, drum;      /* drum: -1 = not a drum */
   unsigned age, t;
   double vel, env, ph, ph2, ph3, lp, ka, kA, kD, kR, freq, f0, detmul;
   patch_t pat;
} voice_t;

typedef struct { int prog, sus; double vol, expr, pan, bend, bendmul; } chan_t;

/* ev: the tempo events only, in order */
static unsigned frame_at(const ev_t *ev, int nev, int division, unsigned tick, int rate)
{
   double sec = 0, us = 500000;
   unsigned t0 = 0;
   int i;
   for (i = 0; i < nev; i++)
   {
      if (ev[i].tick >= tick) break;
      sec += ((double)(ev[i].tick - t0) * us) / (division * 1e6);
      t0 = ev[i].tick; us = ev[i].tempo;
   }
   sec += ((double)(tick - t0) * us) / (division * 1e6);
   return (unsigned)floor(sec * rate + 0.5);
}

int midisynth_render(const unsigned char *data, long len, short **pcm_out, unsigned *frames_out,
                     unsigned *loop_out, const char **why)
{
   const int rate = MIDISYNTH_RATE;
   evlist_t L = { 0 };
   ev_t *tempos = NULL;
   int division = 0, i, k, nv = 0, ntempo = 0;
   unsigned last_tick = 0, total, cap, rendered = 0, age = 0;
   long loop_start = -1;
   short *pcm = NULL;
   float *dlL = NULL, *dlR = NULL;
   int nL = (int)floor(0.27 * rate + 0.5), nR = (int)floor(0.36 * rate + 0.5), diL = 0, diR = 0;
   chan_t ch[16];
   static voice_t vo[MAX_VOICES];

   *why = "out of memory";
   if (parse_midi(data, len, &division, &L, why)) { free(L.e); return -1; }
   for (i = 0; i < L.n; i++) if (L.e[i].tick > last_tick) last_tick = L.e[i].tick;
   tempos = malloc(sizeof(ev_t) * (L.n ? L.n : 1));
   if (!tempos) { free(L.e); return -1; }
   for (i = 0; i < L.n; i++) if (L.e[i].type == EV_TEMPO) tempos[ntempo++] = L.e[i];
   total = frame_at(tempos, ntempo, division, last_tick, rate);
   cap = (unsigned)(MIDISYNTH_MAX_SECONDS * rate);
   midisynth_capped = total > cap;
   if (total > cap) total = cap;
   pcm = malloc((size_t)(total ? total : 1) * 4);
   dlL = calloc(nL, sizeof(float)); dlR = calloc(nR, sizeof(float));
   if (!pcm || !dlL || !dlR) { free(L.e); free(tempos); free(pcm); free(dlL); free(dlR); return -1; }
   for (k = 0; k < 16; k++) { ch[k].prog = 0; ch[k].sus = 0; ch[k].vol = 100 / 127.0; ch[k].expr = 1; ch[k].pan = 0.5; ch[k].bend = 0; ch[k].bendmul = 1; }
   rng_x = 0x2545f491u;

   for (i = 0; i <= L.n; i++)
   {
      unsigned at = i < L.n ? frame_at(tempos, ntempo, division, L.e[i].tick, rate) : total;
      unsigned f;
      if (at > total) at = total;
      /* render up to the event (renderTo) */
      for (f = rendered; f < at; f++)
      {
         double Ls = 0, Rs = 0, eL, eR;
         for (k = nv - 1; k >= 0; k--)
         {
            voice_t *v = &vo[k];
            const patch_t *P = &v->pat;
            const chan_t *c = &ch[v->ch];
            double x, amp;
            if (v->stage == 0) { v->env += v->kA; if (v->env >= 1) { v->env = 1; v->stage = 1; } }
            else if (v->stage == 1) v->env = P->s + (v->env - P->s) * v->kD;
            else v->env *= v->kR;
            if ((v->stage == 2 || (v->stage == 1 && P->s == 0)) && v->env < 0.0005)
            {
               memmove(&vo[k], &vo[k + 1], sizeof(voice_t) * (nv - k - 1));
               nv--;
               continue;
            }
            if (v->drum >= 0)
            {
               double t = (double)v->t / rate;
               if (v->drum == 0 || v->drum == 4)
               {
                  double fr = v->drum == 0 ? 45 + (v->f0 - 45) * exp(-t / 0.03) : v->f0 * (0.7 + 0.3 * exp(-t / 0.05));
                  v->ph += fr / rate; v->ph -= floor(v->ph);
                  x = sin(TAU * v->ph);
               }
               else if (v->drum == 1)
               {
                  double n;
                  v->ph += v->f0 / rate; v->ph -= floor(v->ph);
                  n = noise() * 2 - 1; v->lp += v->ka * (n - v->lp);
                  x = 0.6 * v->lp + 0.4 * (1 - 4 * fabs(v->ph - 0.5));
               }
               else
               {
                  double n = noise() * 2 - 1;
                  v->lp += v->ka * (n - v->lp);
                  x = v->drum == 5 ? n * 0.6 : n - v->lp;
               }
               v->t++;
            }
            else
            {
               double fr = v->freq * c->bendmul;
               v->ph += fr / rate; v->ph -= floor(v->ph);
               x = osc(P->w, v->ph);
               if (P->det) { v->ph2 += (fr * v->detmul) / rate; v->ph2 -= floor(v->ph2); x = 0.5 * (x + osc(P->w, v->ph2)); }
               if (P->h) { v->ph3 += (2 * fr) / rate; v->ph3 -= floor(v->ph3); x += P->h * sin(TAU * v->ph3); }
               if (P->lp) { v->lp += v->ka * (x - v->lp); x = v->lp; }
            }
            amp = x * v->env * v->vel * P->g * c->vol * c->expr;
            Ls += amp * (2 * (1 - c->pan) < 1 ? 2 * (1 - c->pan) : 1);
            Rs += amp * (2 * c->pan < 1 ? 2 * c->pan : 1);
         }
         eL = dlL[diL]; eR = dlR[diR];
         dlL[diL] = (float)(Ls + eR * 0.3); dlR[diR] = (float)(Rs + eL * 0.3);
         if (++diL >= nL) diL = 0;
         if (++diR >= nR) diR = 0;
         pcm[f * 2]     = (short)lrint(softclip(0.5 * (Ls + 0.22 * eL)) * 32767.0);
         pcm[f * 2 + 1] = (short)lrint(softclip(0.5 * (Rs + 0.22 * eR)) * 32767.0);
      }
      if (at > rendered) rendered = at;
      if (i == L.n) break;
      {
         const ev_t *e = &L.e[i];
         chan_t *c = &ch[e->ch & 15];
         switch (e->type)
         {
         case EV_ON:
         {
            voice_t *v;
            if (nv >= MAX_VOICES)
            {
               int oldest = 0;
               for (k = 1; k < nv; k++) if (vo[k].age < vo[oldest].age) oldest = k;
               memmove(&vo[oldest], &vo[oldest + 1], sizeof(voice_t) * (nv - oldest - 1));
               nv--;
            }
            v = &vo[nv++];
            memset(v, 0, sizeof *v);
            v->ch = e->ch; v->note = e->a; v->age = age++; v->vel = e->b / 127.0; v->drum = -1;
            if (e->ch == 9)
            {
               int kind; double dec, g, hz;
               drum_for(e->a, &kind, &dec, &g, &hz);
               v->drum = kind;
               v->pat.w = 0; v->pat.a = 0.001; v->pat.d = dec; v->pat.s = 0; v->pat.r = dec; v->pat.g = g;
               v->f0 = hz;
               v->ka = 1 - exp((-TAU * (kind == 1 ? 5000 : 7000)) / rate);
            }
            else
            {
               v->pat = PATCHES[(c->prog >> 3) & 15];
               v->freq = 440 * pow(2, (e->a - 69) / 12.0);
               v->ka = v->pat.lp ? 1 - exp((-TAU * (v->pat.lp < rate * 0.45 ? v->pat.lp : rate * 0.45)) / rate) : 1;
               v->detmul = pow(2, v->pat.det / 1200);
            }
            {
               double a = v->pat.a * rate, d = v->pat.d * rate / 3, r = v->pat.r * rate / 5;
               v->kA = 1 / (a > 1 ? a : 1);
               v->kD = exp(-1 / (d > 1 ? d : 1));
               v->kR = exp(-1 / (r > 1 ? r : 1));
            }
            break;
         }
         case EV_OFF:
            if (e->ch == 9) break;
            for (k = 0; k < nv; k++)
               if (vo[k].ch == e->ch && vo[k].note == e->a && vo[k].stage < 2)
               { if (c->sus) vo[k].held = 1; else vo[k].stage = 2; }
            break;
         case EV_PROG: c->prog = e->a; break;
         case EV_BEND: c->bend = (e->a / 8192.0) * 2; c->bendmul = pow(2, c->bend / 12); break;
         case EV_CC:
            if (e->a == 7) c->vol = e->b / 127.0;
            else if (e->a == 11) c->expr = e->b / 127.0;
            else if (e->a == 10) c->pan = e->b / 127.0;
            else if (e->a == 64)
            {
               c->sus = e->b >= 64;
               if (!c->sus) for (k = 0; k < nv; k++) if (vo[k].ch == e->ch && vo[k].held) { vo[k].held = 0; if (vo[k].stage < 2) vo[k].stage = 2; }
            }
            else if (e->a == 111 && loop_start < 0) loop_start = at;
            else if (e->a == 120 || e->a == 123) { for (k = 0; k < nv; k++) if (vo[k].ch == e->ch && vo[k].stage < 2) vo[k].stage = 2; }
            break;
         default: break;
         }
      }
   }
   free(L.e); free(tempos); free(dlL); free(dlR);
   *pcm_out = pcm;
   *frames_out = total;
   *loop_out = loop_start >= 0 && (unsigned)loop_start < total ? (unsigned)loop_start : 0;
   return 0;
}
