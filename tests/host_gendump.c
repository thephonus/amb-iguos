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
/* gendump TYPE [PARAM=NORM16 ...]: the generator's output, raw int16 on stdout */
int main(int argc, char **argv)
{
    int t = atoi(argv[1]), a;
    uint32_t frames;
    gr_params_defaults(); gr_gen_defaults(); gr_engine_init();
    gr_pt[0].smp.data = smp_mem[0];
    gr_gen[0].type = (uint8_t)t;
    for (a = 2; a < argc; a++) { int p, v; if (sscanf(argv[a], "%d=%d", &p, &v) == 2) gr_gen[0].norm[t][p] = (uint16_t)v; }
    frames = gr_gen_render(0, smp_mem[0]);
    fwrite(smp_mem[0], 2, frames, stdout);
    return 0;
}
