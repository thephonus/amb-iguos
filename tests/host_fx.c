/* host check of the delay and the reverb: an impulse in, the echoes and the tail out */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#define GR_POOL
#include "gr_core.h"
static uint32_t fm1_ms;
static unsigned str_len(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }
static void str_cpy(char *d, const char *s, unsigned max) { unsigned i = 0; while (s[i] && i + 1 < max) { d[i] = s[i]; i++; } d[i] = 0; }
static void fmt_fix(char *b, int32_t v, int decimals) { (void)b; (void)v; (void)decimals; }
#include "gr_params.c"
#include "gr_fx.c"
int main(void)
{
    static int32_t in[128], z[128], ol[128], orr[128];
    int b, k, fails = 0, echo375 = 0, echo750r = 0; int32_t peak = 0; double e, tail = 1e9;
    printf("delay: 375 ms (=%d half-rate frames), fb 50%%\n", 375 * 22050 / 1000);
    in[0] = 20000;
    for (b = 0; b < 700; b++) {
        memset(ol, 0, sizeof ol); memset(orr, 0, sizeof orr);
        gr_delay_run(b == 0 ? in : z, b == 0 ? in : z, ol, orr, 128, 6000, 500, 300);
        for (k = 0; k < 128; k++) if (abs(ol[k]) > 800 || abs(orr[k]) > 800) {
            int ms = (b * 128 + k) * 1000 / 44100;
            printf("  echo at %d ms: L %d R %d\n", ms, ol[k], orr[k]);
            if (ms >= 373 && ms <= 377 && abs(ol[k]) > 800 && !orr[k]) echo375 = 1;
            if (ms >= 748 && ms <= 752 && abs(orr[k]) > 800 && !ol[k]) echo750r = 1;
        }
    }
    gr_reverb_init();
    printf("reverb: decay .7 damp .35\n");
    for (b = 0; b < 1400; b++) {
        memset(ol, 0, sizeof ol); memset(orr, 0, sizeof orr);
        gr_reverb_run(b == 0 ? in : z, b == 0 ? in : z, ol, orr, 128, 700, 350);
        e = 0; peak = 0;
        for (k = 0; k < 128; k++) { e += (double)ol[k] * ol[k]; if (abs(ol[k]) > peak) peak = abs(ol[k]); }
        if (b % 100 == 0) printf("  t=%4d ms rms %8.1f peak %d\n", b * 128 * 1000 / 44100, sqrt(e / 128), peak);
        if (b == 1300) tail = sqrt(e / 128);
    }
    if (!echo375 || !echo750r) { printf("FAIL: the echoes are not at 375 ms (left) and 750 ms (right)\n"); fails++; }
    if (tail > 2.0) { printf("FAIL: the reverb tail does not end (rms %.1f)\n", tail); fails++; }
    printf("reverb decay .95 (long): ");
    gr_reverb_init(); memset(&rv.lfo, 0, sizeof rv.lfo);
    for (b = 0; b < 3000; b++) {
        memset(ol, 0, sizeof ol); memset(orr, 0, sizeof orr);
        gr_reverb_run(b == 0 ? in : z, b == 0 ? in : z, ol, orr, 128, 950, 100);
        e = 0; for (k = 0; k < 128; k++) e += (double)ol[k] * ol[k];
        if (b == 2999) printf("rms after 8.7 s %.1f\n", sqrt(e / 128));
    }
    {
        double ei = 0, eo = 0; int n = 0;
        gr_reverb_init(); memset(&rv.lfo, 0, sizeof rv.lfo);
        for (b = 0; b < 800; b++) {
            static int32_t nz[128];
            for (k = 0; k < 128; k++) nz[k] = (rand() % 6001) - 3000;
            memset(ol, 0, sizeof ol); memset(orr, 0, sizeof orr);
            gr_reverb_run(nz, nz, ol, orr, 128, 700, 350);
            if (b > 300) for (k = 0; k < 128; k++) { ei += (double)nz[k] * nz[k]; eo += (double)ol[k] * ol[k]; n++; }
        }
        printf("steady state: input rms %.0f, reverb output rms %.0f (gain %.2f)\n", sqrt(ei / n), sqrt(eo / n), sqrt(eo / ei));
        if (sqrt(eo / ei) < 0.5 || sqrt(eo / ei) > 2.5) { printf("FAIL: reverb gain\n"); fails++; }
    }
    printf(fails ? "%d FAILURES\n" : "effects ok\n", fails);
    return fails != 0;
}
