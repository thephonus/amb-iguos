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
/* ambdump PART NOTE SECONDS [Name=norm16 ..]: a drone or pad part playing a note, raw int16 left channel on stdout */
int main(int argc, char **argv)
{
    uint32_t part = (uint32_t)atoi(argv[1]), note = (uint32_t)atoi(argv[2]), blocks = (uint32_t)(atof(argv[3]) * 44100 / 128), b, p;
    int a;
    gr_params_defaults(); gr_gen_defaults(); gr_engine_init();
    for (a = 0; a < NGPART; a++) gr_pt[a].smp.data = smp_mem[a];
    for (a = 4; a < argc; a++) {
        char name[40]; int v;
        if (sscanf(argv[a], "%39[^=]=%d", name, &v) != 2) continue;
        for (p = 0; p < P_COUNT; p++) if (!strcmp(GR_PINFO[p].name, name)) gr_set_norm(part, p, v);
    }
    gr_set_norm(0, P_DLYSEND, 0); gr_set_norm(0, P_REVSEND, 0); gr_set_norm(part, P_DLYSEND, 0); gr_set_norm(part, P_REVSEND, 0);
    amb_poll();
    gr_note_on(part, note, 100);
    for (b = 0; b < blocks; b++) {
        int32_t l[128], r[128]; uint32_t k;
        amb_poll();
        gr_render(l, r);
        for (k = 0; k < 128; k++) { int16_t x = (int16_t)l[k]; fwrite(&x, 2, 1, stdout); }
    }
    return 0;
}
