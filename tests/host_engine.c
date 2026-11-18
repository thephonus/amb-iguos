/* SPDX-License-Identifier: GPL-3.0-only
 * Host test of the Mini-Grain engine: parameters, generators, grains, filters, effects.
 *   cc -O1 -Wall -Ifirmware/src -Ibuild/gen tests/host_engine.c -o build/host_engine && build/host_engine [out.wav] */
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

#include "gr_core.h"
#include "gr_params.c"
#include "gr_filter.c"
#include "gr_fx.c"
#include "gr_engine.c"
#include "gr_gen.c"
#include "gr_amb.c"

static int16_t smp_mem[NPART][SMP_FRAMES + 1];
static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static void stats(const int16_t *d, uint32_t n, int32_t *peak, double *rms)
{
    double acc = 0; int32_t pk = 0; uint32_t i;
    for (i = 0; i < n; i++) { int32_t a = abs(d[i]); if (a > pk) pk = a; acc += (double)d[i] * d[i]; }
    *peak = pk; *rms = sqrt(acc / n);
}

int main(int argc, char **argv)
{
    uint32_t t, part, i, b;
    char buf[32];
    FILE *wav = argc > 1 ? fopen(argv[1], "wb") : 0;
    static int16_t out[2 * 128 * 1200];
    uint32_t nout = 0;

    gr_params_defaults();
    gr_gen_defaults();
    for (i = 0; i < P_COUNT; i++) {
        gr_fmt(buf, 0, i);
        printf("%-10s %-14s norm %5u real %d\n", GR_PINFO[i].name, buf, gr_norm(0, i), gr_real(0, i));
    }
    gr_engine_init();
    for (part = 0; part < NPART; part++) gr_pt[part].smp.data = smp_mem[part];

    /* generators */
    for (t = 0; t < GEN_COUNT; t++) {
        int32_t pk; double rms; uint32_t frames;
        gr_gen[0].type = (uint8_t)t;
        frames = gr_gen_render(0, smp_mem[0]);
        stats(smp_mem[0], frames, &pk, &rms);
        printf("gen %-9s frames %5u peak %5d rms %7.1f ends %d,%d\n", GR_GENTYPES[t].name, frames, pk, rms, smp_mem[0][0], smp_mem[0][frames]);
        CHECK(pk > 20000 && pk <= 32767, "%s peak %d", GR_GENTYPES[t].name, pk);
        CHECK(rms > 500, "%s rms %f", GR_GENTYPES[t].name, rms);
    }

    /* an engine run per generator: a held chord, then release */
    for (t = 0; t < GEN_COUNT; t++) {
        uint32_t frames, blocks = 400;
        int32_t pk = 0; double acc = 0; uint32_t cnt = 0;
        gr_gen[0].type = (uint8_t)t;
        gr_sample_begin(0);
        frames = gr_gen_render(0, smp_mem[0]);
        gr_sample_end(0, frames, GEN_RATE);
        gr_note_on(0, 60, 100);
        gr_note_on(0, 64, 100);
        gr_note_on(0, 67, 100);
        for (b = 0; b < blocks; b++) {
            int32_t l[128], r[128]; uint32_t k;
            if (b == 300) { gr_note_off(0, 60); gr_note_off(0, 64); gr_note_off(0, 67); }
            gr_render(l, r);
            for (k = 0; k < 128; k++) {
                int32_t a = abs(l[k]) > abs(r[k]) ? abs(l[k]) : abs(r[k]);
                if (a > pk) pk = a;
                if (b < 300 && b > 20) { acc += (double)l[k] * l[k]; cnt++; }
                if (wav && t == 0 && nout < 128 * 1200 * 2 - 256) { out[nout++] = (int16_t)l[k]; out[nout++] = (int16_t)r[k]; }
            }
        }
        printf("engine %-9s peak %5d rms %7.1f grains %2d cap %d skipped %u\n", GR_GENTYPES[t].name, pk, sqrt(acc / (cnt ? cnt : 1)),
               gr_st.grains, gr_st.cap, gr_st.skipped);
        CHECK(pk < 32768, "%s clipped", GR_GENTYPES[t].name);
        CHECK(sqrt(acc / (cnt ? cnt : 1)) > 200, "%s silent", GR_GENTYPES[t].name);
        for (b = 0; b < 400; b++) { int32_t l[128], r[128]; gr_render(l, r); }
    }

    /* filters */
    {
        uint32_t modes;
        gr_gen[0].type = 0;
        gr_sample_begin(0);
        gr_sample_end(0, gr_gen_render(0, smp_mem[0]), GEN_RATE);
        for (modes = 0; modes < 5; modes++) {
            int32_t pk = 0; double acc = 0; uint32_t cnt = 0;
            gr_set_norm(0, P_FTYPE, (int32_t)(modes * 65535u / 4u));
            gr_set_norm(0, P_RESO, 52000);
            gr_note_on(0, 48, 110);
            for (b = 0; b < 200; b++) {
                int32_t l[128], r[128]; uint32_t k;
                gr_render(l, r);
                for (k = 0; k < 128; k++) { int32_t a = abs(l[k]); if (a > pk) pk = a; if (b > 40) { acc += (double)l[k] * l[k]; cnt++; } }
            }
            gr_note_off(0, 48);
            printf("filter %u peak %5d rms %7.1f\n", modes, pk, sqrt(acc / cnt));
            CHECK(pk <= 32768, "filter %u clipped", modes);
            for (b = 0; b < 600; b++) { int32_t l[128], r[128]; gr_render(l, r); }
        }
        gr_set_norm(0, P_FTYPE, 0);
    }

    /* two parts, LFO to cutoff and density, sends */
    gr_set_norm(0, P_L1DEST, 65535 / 28 * 1);
    gr_set_norm(0, P_L1DEPTH, 60000);
    gr_set_norm(0, P_L1RATE, 40000);
    gr_set_norm(0, P_FTYPE, 65535 / 4 * 2);
    gr_gen[1].type = 3;
    gr_sample_begin(1);
    gr_sample_end(1, gr_gen_render(1, smp_mem[1]), GEN_RATE);
    gr_note_on(0, 55, 100);
    gr_note_on(1, 72, 100);
    {
        int32_t pk = 0;
        for (b = 0; b < 600; b++) {
            int32_t l[128], r[128]; uint32_t k;
            gr_render(l, r);
            for (k = 0; k < 128; k++) { if (abs(l[k]) > pk) pk = abs(l[k]); }
        }
        printf("two parts peak %d grains %u cap %u notes %u,%u lfo %d\n", pk, gr_st.grains, gr_st.cap, gr_st.notes[0], gr_st.notes[1], gr_st.lfo[0][0]);
        CHECK(pk > 1000, "two parts silent");
    }
    gr_all_sound_off(0);
    gr_all_sound_off(1);
    for (b = 0; b < 100; b++) { int32_t l[128], r[128]; gr_render(l, r); }
    printf("after sound off: grains %u notes %u,%u\n", gr_st.grains, gr_st.notes[0], gr_st.notes[1]);
    CHECK(gr_st.grains == 0, "grains left");

    if (wav) {
        uint32_t bytes = nout * 2, hdr[11] = {0x46464952, 36 + bytes, 0x45564157, 0x20746d66, 16, 0x00020001, 44100, 44100 * 4, 0x00100004, 0x61746164, bytes};
        fwrite(hdr, 4, 11, wav); fwrite(out, 2, nout, wav); fclose(wav);
    }
    printf(fails ? "%d FAILURES\n" : "all good\n", fails);
    return fails != 0;
}
