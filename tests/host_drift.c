/* SPDX-License-Identifier: GPL-3.0-only
 * Host test of amb-iguos's own engine parts: the scale, the Deriva, the Freeze, the shimmer and the macros.
 *   tests/run_tests.sh builds it; build/host_drift [out.wav] renders a 70 s scene in D Mixolydian:
 *   layer A a wavetable, layer B a bowed string, the drone on the tonic and fifth, the Deriva playing, shimmer on;
 *   the Freeze from 35 s to 47 s. It checks that every note is in the scale, that nothing clips or runs away and
 *   that the Freeze holds the sound. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define GR_POOL
static uint32_t fm1_ms;
static int32_t dummy_l[128], dummy_r[128];
static void gr_render(int32_t *, int32_t *);
static void fm1_wdt_feed(void) { fm1_ms++; gr_render(dummy_l, dummy_r); }
#define GR_YIELD() ((void)0)

static unsigned str_len(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }
static void str_cpy(char *d, const char *s, unsigned max) { unsigned i = 0; while (s[i] && i + 1 < max) { d[i] = s[i]; i++; } d[i] = 0; }
static void fmt_fix(char *b, int32_t v, int decimals)
{
    char t[12]; int n = 0, neg = v < 0, i; uint32_t u = (uint32_t)(neg ? -v : v);
    do { t[n++] = (char)('0' + u % 10u); u /= 10u; if (n == decimals) t[n++] = '.'; } while (u || n <= decimals);
    if (neg) t[n++] = '-';
    for (i = 0; i < n; i++) b[i] = t[n - 1 - i];
    b[n] = 0;
}

/* every note that reaches a part is logged */
static uint32_t notes_on, notes_out_of_scale;
#define gr_note_on gr_note_on_real
#include "gr_core.h"
#include "gr_params.c"
#include "gr_filter.c"
#include "gr_fx.c"
#include "gr_engine.c"
#include "gr_gen.c"
#include "gr_amb.c"
#undef gr_note_on
static int sc_in(int32_t note);
static gr_voice_t *gr_note_on(uint32_t part, uint32_t note, uint32_t vel)
{
    notes_on++;
    if (!sc_in((int32_t)note)) {
        notes_out_of_scale++;
        printf("note %u on part %u is not in the scale\n", note, part);
    }
    return gr_note_on_real(part, note, vel);
}
#include "gr_drift.c"
static int sc_in(int32_t note) { return sc_snap(note) == note; }

static int16_t smp_mem[NGPART][SMP_FRAMES + 1];
static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } else { printf("ok   " __VA_ARGS__); printf("\n"); } } while (0)

static void make(uint32_t part, uint32_t type)
{
    gr_gen[part].type = (uint8_t)type;
    gr_sample_begin(part);
    gr_sample_end(part, gr_gen_render(part, smp_mem[part]), GEN_RATE);
}

static void setr(uint32_t part, uint32_t p, int32_t real) { gr_set_norm(part, p, gr_norm_of_real(p, real)); }

/* the energy at hz in x (Goertzel) */
static double tone(const int32_t *x, uint32_t n, double hz)
{
    double w = 2 * M_PI * hz / 44100, c = 2 * cos(w), s1 = 0, s2 = 0;
    uint32_t i;
    for (i = 0; i < n; i++) { double s0 = x[i] + c * s1 - s2; s2 = s1; s1 = s0; }
    return s1 * s1 + s2 * s2 - c * s1 * s2;
}

/* a 440 Hz burst into the reverb alone; the tail's 880 Hz against its 440 Hz */
static double shimmer_ratio(int32_t shim)
{
    static int32_t tail[44100 * 3];
    uint32_t b, k, n = 0;
    memset(rv_mem, 0, sizeof rv_mem); memset(&rv, 0, sizeof rv); memset(sh_buf, 0, sizeof sh_buf); memset(&sh, 0, sizeof sh);
    gr_reverb_init();
    for (b = 0; b < 6 * 344; b++) {
        int32_t in[128], l[128], r[128];
        for (k = 0; k < 128; k++) {
            in[k] = b < 30 ? (int32_t)(8000 * sin(2 * M_PI * 440 * (b * 128 + k) / 44100.0)) : 0;
            l[k] = r[k] = 0;
        }
        gr_reverb_run(in, in, l, r, 128, 900, 200, shim, 0);
        if (b >= 3 * 344 && n < 44100 * 3 - 128)
            for (k = 0; k < 128; k++) tail[n++] = l[k];
    }
    return tone(tail, n, 880) / (tone(tail, n, 440) + 1);
}

int main(int argc, char **argv)
{
    FILE *wav = argc > 1 ? fopen(argv[1], "wb") : 0;
    const uint32_t SECONDS = 70, blocks = SECONDS * 44100u / 128u;
    static int16_t out[2u * 128u * (70u * 44100u / 128u + 1u)];
    uint32_t b, nout = 0, k;
    double win[8] = {0};
    uint32_t wcnt[8] = {0};
    int32_t peak = 0;

    gr_params_defaults();
    gr_gen_defaults();
    gr_engine_init();
    for (k = 0; k < NGPART; k++)
        gr_pt[k].smp.data = smp_mem[k];
    /* D Mixolydian */
    gr_set_norm(0, P_KEY, 2 * 65535 / 11);
    gr_set_norm(0, P_MODE, 5 * 65535 / (GR_NMODES - 1));
    printf("scale: %s %s\n", GR_CH_KEY[gr_index(0, P_KEY)], GR_CH_MODE[gr_index(0, P_MODE)]);
    CHECK(sc_note(0, 4) == 62 && sc_note(6, 4) == 72 && sc_note(7, 4) == 74, "D Mixolydian: D4 = 62, the 7th is C5 = 72");
    CHECK(sc_snap(63) == 62 || sc_snap(63) == 64, "Eb snaps to D or E");
    CHECK(sc_snap(66) == 66 && sc_snap(65) == 64, "F# stays, F goes to E");
    /* the sounds */
    make(0, GT_WAVETABLE);
    make(1, GT_BOWWIND);
    setr(0, P_SIZE, 260 * 16);
    setr(0, P_DENSITY, 18 * 256);
    setr(0, P_SPREAD, 150);
    setr(0, P_ATTACK, 1500 * 16);
    setr(0, P_RELEASE, 4000 * 16);
    setr(1, P_ATTACK, 2500 * 16);
    setr(1, P_RELEASE, 5000 * 16);
    setr(0, P_REVSEND, 500);
    setr(1, P_REVSEND, 600);
    setr(2, P_REVSEND, 400);
    setr(0, P_SHIMMER, 450);
    setr(0, P_REVDECAY, 900);
    setr(0, P_DRATE, 250);                           /* an event every ~2.5 s */
    setr(0, P_DLEN, 700);
    gr_set_norm(0, P_DDRONE, 65535);                 /* tonic and fifth */
    dr_on = 1;
    for (b = 0; b < blocks; b++) {
        int32_t l[128], r[128];
        uint32_t sec = b * 128u / 44100u;
        if (b == 35u * 44100u / 128u)
            dr_freeze_set(1);
        if (b == 47u * 44100u / 128u)
            dr_freeze_set(0);
        if (b == 55u * 44100u / 128u)
            gr_set_norm(0, P_MACRO3, 65535);         /* ESPACIO all the way */
        if (b == 62u * 44100u / 128u)
            dr_on = 0;
        dr_block();
        gr_render(l, r);
        for (k = 0; k < 128u; k++) {
            int32_t a = abs(l[k]) > abs(r[k]) ? abs(l[k]) : abs(r[k]);
            if (a > peak)
                peak = a;
            if (sec / 10u < 8u) {
                win[sec / 10u] += (double)l[k] * l[k];
                wcnt[sec / 10u]++;
            }
            out[nout++] = (int16_t)l[k];
            out[nout++] = (int16_t)r[k];
        }
    }
    for (k = 0; k < 7u; k++)
        printf("rms %2u-%2u s: %7.1f\n", k * 10u, k * 10u + 10u, sqrt(win[k] / (wcnt[k] ? wcnt[k] : 1u)));
    printf("notes played %u, out of the scale %u, peak %d\n", notes_on, notes_out_of_scale, peak);
    CHECK(notes_on > 10, "the Deriva played (%u notes)", notes_on);
    CHECK(notes_out_of_scale == 0, "every note in the scale");
    CHECK(peak < 32767, "no clipping (peak %d)", peak);
    CHECK(sqrt(win[3] / wcnt[3]) > 300 && sqrt(win[4] / wcnt[4]) > 300, "the Freeze holds the sound");
    CHECK(sqrt(win[6] / wcnt[6]) < 3.0 * sqrt(win[2] / wcnt[2]) + 500, "the shimmer does not run away");
    {
        double a = shimmer_ratio(0), c = shimmer_ratio(800);
        printf("tail 880/440 Hz: shimmer off %.3f, on %.3f\n", a, c);
        CHECK(c > 4 * a, "the shimmer climbs an octave");
    }
    if (wav) {
        uint32_t bytes = nout * 2u, hdr[11] = {0x46464952, 36 + bytes, 0x45564157, 0x20746d66, 16, 0x00020001, 44100, 44100 * 4, 0x00100004, 0x61746164, bytes};
        fwrite(hdr, 4, 11, wav);
        fwrite(out, 2, nout, wav);
        fclose(wav);
    }
    printf(fails ? "%d FAILURES\n" : "all good\n", fails);
    return fails != 0;
}
