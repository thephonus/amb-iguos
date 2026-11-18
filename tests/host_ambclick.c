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
#include "gr_drift.c"


static int16_t smp_mem[NGPART][SMP_FRAMES + 1];
/* ambclick: two notes pressed in the same block, in every mode of the drone and the pad; reports the worst jump
 * (second difference) in the output against the steady sound's, per case */
static int16_t out[44100 * 4];
static void play(uint32_t part, uint32_t from, uint32_t blocks, uint32_t *n)
{
    uint32_t b, k;
    for (b = 0; b < blocks; b++) {
        int32_t l[128], r[128];
        amb_poll(); gr_render(l, r);
        for (k = 0; k < 128; k++) out[(*n)++] = (int16_t)l[k];
    }
    (void)part; (void)from;
}
int main(void)
{
    uint32_t part, mode, st, worst_case = 0;
    int a;
    gr_params_defaults(); gr_gen_defaults(); gr_engine_init();
    for (a = 0; a < NGPART; a++) gr_pt[a].smp.data = smp_mem[a];
    for (part = 2; part < 4; part++)
        for (mode = 0; mode < 4; mode++)
            for (st = 0; st < 4; st += 3) {
                uint32_t n = 0, i, ev, pre;
                int32_t base = 0, worst = 0, at = 0, d2;
                gr_params_defaults();
                gr_set_norm(part, P_DLYSEND, 0); gr_set_norm(part, P_REVSEND, 0); gr_set_norm(0, P_DLYSEND, 0); gr_set_norm(0, P_REVSEND, 0);
                gr_set_norm(part, P_AVOICE, mode * 65535 / 3);
                gr_set_norm(part, P_ASTRUCT, st * 65535 / 3);
                gr_set_norm(part, P_AATK, 0);
                amb_poll();
                play(part, 0, 20, &n);
                gr_note_on(part, 48, 100);
                play(part, 0, 100, &n);
                ev = n; pre = n;
                gr_note_on(part, 55, 100); gr_note_on(part, 64, 100);        /* both in one block */
                play(part, 0, 150, &n);
                for (i = 2; i < pre; i++) { d2 = out[i] - 2 * out[i - 1] + out[i - 2]; if (d2 < 0) d2 = -d2; if (d2 > base) base = d2; }
                for (i = ev + 2; i < n; i++) { d2 = out[i] - 2 * out[i - 1] + out[i - 2]; if (d2 < 0) d2 = -d2; if (d2 > worst) { worst = d2; at = (int32_t)(i - ev); } }
                printf("part %u mode %u struct %u: steady max d2 %d, after two notes %d at %d%s\n", part + 1, mode, st, base, worst, at, worst > base * 2 + 50 ? "  <-- CLICK" : "");
                for (a = 0; a < NPART; a++) gr_all_sound_off(a);
                play(part, 0, 40, &n);
            }
    /* a full chord (four voices) and two more notes in the same block: nothing may jump, no note may be lost */
    for (part = 2; part < 4; part++) {
        uint32_t n = 0, i, ev, pre, k, sounding = 0;
        int32_t base = 0, worst = 0, d2;
        gr_params_defaults();
        gr_set_norm(part, P_DLYSEND, 0); gr_set_norm(part, P_REVSEND, 0); gr_set_norm(0, P_DLYSEND, 0); gr_set_norm(0, P_REVSEND, 0);
        gr_set_norm(part, P_AATK, 0);
        amb_poll();
        gr_note_on(part, 48, 100); gr_note_on(part, 52, 100); gr_note_on(part, 55, 100); gr_note_on(part, 59, 100);
        play(part, 0, 150, &n);
        pre = n;
        gr_note_on(part, 62, 100); gr_note_on(part, 66, 100);
        ev = n;
        play(part, 0, 100, &n);
        for (i = 2; i < pre; i++) { d2 = out[i] - 2 * out[i - 1] + out[i - 2]; if (d2 < 0) d2 = -d2; if (d2 > base) base = d2; }
        for (i = ev + 2; i < n; i++) { d2 = out[i] - 2 * out[i - 1] + out[i - 2]; if (d2 < 0) d2 = -d2; if (d2 > worst) worst = d2; }
        for (k = 0; k < AMB_V; k++) sounding += amb[part - NGPART].v[k].active && !amb[part - NGPART].v[k].stealing;
        printf("full chord, part %u: steady d2 %d, after two more notes %d, voices %u%s\n", part + 1, base, worst, sounding,
               worst > base * 2 + 50 ? "  <-- CLICK" : "");
        for (a = 0; a < NPART; a++) gr_all_sound_off(a);
        play(part, 0, 40, &n);
    }
    /* onsets: the first 600 samples of one note, and of two at once, against the sound a moment later */
    for (part = 2; part < 4; part++)
        for (mode = 0; mode < 2; mode++) {
            uint32_t cnt;
            for (cnt = 1; cnt <= 3; cnt++) {
                uint32_t n = 0, i;
                int32_t on = 0, later = 0, peak = 0, d2;
                gr_params_defaults();
                gr_set_norm(part, P_DLYSEND, 0); gr_set_norm(part, P_REVSEND, 0); gr_set_norm(0, P_DLYSEND, 0); gr_set_norm(0, P_REVSEND, 0);
                gr_set_norm(part, P_AATK, mode ? 0 : 20000);
                amb_poll();
                play(part, 0, 20, &n);
                gr_note_on(part, 48, 100); if (cnt > 1) gr_note_on(part, 55, 100); if (cnt > 2) gr_note_on(part, 64, 100);
                play(part, 0, 200, &n);
                for (i = 20 * 128 + 2; i < 20 * 128 + 600; i++) { d2 = out[i] - 2 * out[i - 1] + out[i - 2]; if (d2 < 0) d2 = -d2; if (d2 > on) on = d2; }
                for (i = 120 * 128; i < n; i++) { d2 = out[i] - 2 * out[i - 1] + out[i - 2]; if (d2 < 0) d2 = -d2; if (d2 > later) later = d2; if (abs(out[i]) > peak) peak = abs(out[i]); }
                printf("onset part %u attack %s, %u note(s): d2 at onset %d, later %d, peak %d%s\n", part + 1, mode ? "min" : "slow", cnt, on, later, peak, on > later * 2 + 50 ? "  <-- TICK" : "");
                for (a = 0; a < NPART; a++) gr_all_sound_off(a);
                play(part, 0, 40, &n);
            }
        }
    /* random settings: two notes together; the worst jump in the first 40 ms against the same sound a moment later */
    {
        uint32_t trial, worstt = 0; int32_t worstr = 0;
        srand(7);
        for (trial = 0; trial < 300; trial++) {
            uint32_t n = 0, i, p, nn = 2 + (uint32_t)rand() % 2, k;
            int32_t on = 0, later = 0, d2;
            uint32_t notes[3];
            part = 2 + (uint32_t)(trial & 1u);
            gr_params_defaults();
            for (p = 0; p < P_COUNT; p++)
                if (GR_PINFO[p].name && (p == P_AWAVE || p == P_AHARM || p == P_ASTRUCT || p == P_APITCH || p == P_ADETUNE || p == P_ABAL || p == P_AMODRATE ||
                    p == P_AMODDEPTH || p == P_AATK || p == P_AREL || p == P_AVOICE || p == P_AGLIDE || p == P_AUNIDET || p == P_AMODSHAPE || p == P_TRANSPOSE ||
                    p == P_CUTOFF || p == P_RESO || p == P_DRIVE || p == P_FTYPE))
                    gr_set_norm(part, p, rand() % 65536);
            gr_set_norm(part, P_DLYSEND, 0); gr_set_norm(part, P_REVSEND, 0); gr_set_norm(0, P_DLYSEND, 0); gr_set_norm(0, P_REVSEND, 0);
            gr_set_norm(part, P_AATK, (uint32_t)rand() % 3 == 0 ? 0 : (uint32_t)rand() % 20000);
            amb_poll(); play(part, 0, 30, &n);
            for (k = 0; k < nn; k++) { notes[k] = 40 + (uint32_t)rand() % 40; gr_note_on(part, notes[k], 60 + (uint32_t)rand() % 67); }
            play(part, 0, 160, &n);
            for (i = 30 * 128 + 2; i < 30 * 128 + 1800; i++) { d2 = out[i] - 2 * out[i - 1] + out[i - 2]; if (d2 < 0) d2 = -d2; if (d2 > on) on = d2; }
            for (i = 100 * 128; i < n; i++) { d2 = out[i] - 2 * out[i - 1] + out[i - 2]; if (d2 < 0) d2 = -d2; if (d2 > later) later = d2; }
            if (on > later * 2 + 100 && on - later > worstr) { worstr = on - later; worstt = trial; { uint32_t q; int32_t m = 0, mi = 0; for (i = 30 * 128 + 2; i < 30 * 128 + 1800; i++) { d2 = out[i] - 2 * out[i - 1] + out[i - 2]; if (d2 < 0) d2 = -d2; if (d2 > m) { m = d2; mi = (int32_t)i - 30 * 128; } }
 printf("  at %d: ", mi); for (q = 0; q < 6; q++) printf("%d ", out[30 * 128 + mi - 3 + (int32_t)q]); printf("\n  params:"); for (q = 0; q < P_COUNT; q++) if (GR_PINFO[q].name && q >= P_AWAVE && q <= P_APAN) printf(" %s=%u", GR_PINFO[q].name, gr_norm(part, q)); printf(" cut=%u reso=%u drive=%u ft=%u\n", gr_norm(part, P_CUTOFF), gr_norm(part, P_RESO), gr_norm(part, P_DRIVE), gr_norm(part, P_FTYPE)); }
 printf("trial %u part %u: onset d2 %d vs later %d (notes %u %u)\n", trial, part + 1, on, later, notes[0], notes[1]); }
            for (a = 0; a < NPART; a++) gr_all_sound_off(a);
            play(part, 0, 60, &n);
        }
        printf("random trials done (worst trial %u, excess %d)\n", worstt, worstr);
    }
    /* passing from one note to the next: press, let go, press another (a gap, an overlap), in every voice mode */
    for (part = 2; part < 4; part++)
        for (mode = 0; mode < 4; mode++) {
            uint32_t n = 0, i, ev, gap;
            for (gap = 0; gap < 3; gap++) {                                   /* 0: legato overlap, 1: straight on, 2: after a pause */
                int32_t before = 0, worst = 0, d2;
                gr_params_defaults();
                gr_set_norm(part, P_DLYSEND, 0); gr_set_norm(part, P_REVSEND, 0); gr_set_norm(0, P_DLYSEND, 0); gr_set_norm(0, P_REVSEND, 0);
                gr_set_norm(part, P_AVOICE, mode * 65535 / 3); gr_set_norm(part, P_AATK, 0); gr_set_norm(part, P_AREL, 20000);
                n = 0; amb_poll(); play(part, 0, 10, &n);
                gr_note_on(part, 48, 100); play(part, 0, 150, &n);
                for (i = 2; i < n; i++) { d2 = out[i] - 2 * out[i - 1] + out[i - 2]; if (d2 < 0) d2 = -d2; if (d2 > before) before = d2; }
                if (gap == 0) { gr_note_on(part, 55, 100); gr_note_off(part, 48); }
                else if (gap == 1) { gr_note_off(part, 48); gr_note_on(part, 55, 100); }
                else { gr_note_off(part, 48); play(part, 0, 6, &n); gr_note_on(part, 55, 100); }
                ev = n; play(part, 0, 100, &n);
                for (i = ev + 2; i < n; i++) { d2 = out[i] - 2 * out[i - 1] + out[i - 2]; if (d2 < 0) d2 = -d2; if (d2 > worst) worst = d2; }
                printf("note change, part %u mode %u %s: steady d2 %d, around the change %d%s\n", part + 1, mode, gap == 0 ? "overlap" : gap == 1 ? "straight" : "pause",
                       before, worst, worst > before * 2 + 60 ? "  <-- CLICK" : "");
                for (a = 0; a < NPART; a++) gr_all_sound_off(a);
                play(part, 0, 40, &n);
            }
        }
    /* releases in poly mode: random release and attack times, released at a random moment, one or two notes */
    {
        uint32_t trial, bad = 0;
        srand(11);
        for (trial = 0; trial < 200; trial++) {
            uint32_t n = 0, i, k, nn = 1 + (uint32_t)rand() % 3, ev;
            int32_t before = 0, worst = 0, d2, at = 0;
            uint32_t notes[3], hold = 2 + (uint32_t)rand() % 120;
            part = 2 + (trial & 1u);
            gr_params_defaults();
            gr_set_norm(part, P_DLYSEND, 0); gr_set_norm(part, P_REVSEND, 0); gr_set_norm(0, P_DLYSEND, 0); gr_set_norm(0, P_REVSEND, 0);
            gr_set_norm(part, P_AREL, (uint32_t)rand() % 3 == 0 ? 0 : (uint32_t)rand() % 30000);
            gr_set_norm(part, P_AATK, (uint32_t)rand() % 3 == 0 ? 0 : (uint32_t)rand() % 30000);
            amb_poll(); play(part, 0, 10, &n);
            for (k = 0; k < nn; k++) { notes[k] = 40 + (uint32_t)rand() % 40; gr_note_on(part, notes[k], 100); }
            play(part, 0, hold, &n);
            for (i = 2; i < n; i++) { d2 = out[i] - 2 * out[i - 1] + out[i - 2]; if (d2 < 0) d2 = -d2; if (d2 > before) before = d2; }
            for (k = 0; k < nn; k++) gr_note_off(part, notes[k]);
            ev = n; play(part, 0, 400, &n);
            for (i = ev + 2; i < n; i++) { d2 = out[i] - 2 * out[i - 1] + out[i - 2]; if (d2 < 0) d2 = -d2; if (d2 > worst) { worst = d2; at = (int32_t)(i - ev); } }
            if (worst > before + 40 && worst > before * 3 / 2) {
                bad++;
                printf("release trial %u part %u: %u notes, held %u blocks, rel %d ms atk %d ms: d2 %d before, %d after at %d (block %d)\n", trial, part + 1, nn, hold,
                       gr_pt[part].val[P_AREL] / 16, gr_pt[part].val[P_AATK] / 16, before, worst, at, at / 128);
            }
            for (a = 0; a < NPART; a++) gr_all_sound_off(a);
            play(part, 0, 40, &n);
        }
        printf("release trials: %u suspicious of 200\n", bad);
    }
    (void)worst_case;
    return 0;
}
