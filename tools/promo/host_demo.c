/* SPDX-License-Identifier: GPL-3.0-only
 * Demo renderer for the presentation video: scripted musical scenes played through the real engine and sequencer.
 *   cc -O1 -Ifirmware/src -Ibuild/gen tools/promo/host_demo.c -o build/host_demo -lm && build/host_demo SCENE [ARG] > scene.raw   (stereo s16le 44.1 kHz) */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#define GR_POOL
static uint32_t fm1_ms;
static void gr_render(int32_t *, int32_t *);
static void fm1_wdt_feed(void) { int32_t l[128], r[128]; fm1_ms++; gr_render(l, r); }
#define GR_YIELD() ((void)0)
static unsigned str_len(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }
static void str_cpy(char *d, const char *s, unsigned max) { unsigned i = 0; while (s[i] && i + 1 < max) { d[i] = s[i]; i++; } d[i] = 0; }
static void fmt_fix(char *b, int32_t v, int decimals) { (void)b; (void)v; (void)decimals; }
#include "gr_core.h"
#include "gr_params.c"
#include "gr_filter.c"
#include "gr_fx.c"
#include "gr_engine.c"
#include "gr_gen.c"
#include "gr_amb.c"


static void midi_out_event(uint32_t pkt) { (void)pkt; }
#include "gr_seq.c"
#include "demo_scenes.inc"

int main(int argc, char **argv)
{
    int a;
    const char *sc = argc > 1 ? argv[1] : "seq";
    gr_params_defaults(); gr_gen_defaults(); gr_engine_init();
    for (a = 0; a < NGPART; a++) gr_pt[a].smp.data = demo_smp[a];
    if (!strcmp(sc, "gen")) scene_gen((uint32_t)atoi(argc > 2 ? argv[2] : "0"));
    else if (!strcmp(sc, "granular")) scene_granular();
    else if (!strcmp(sc, "drone")) scene_drone();
    else if (!strcmp(sc, "pad")) scene_pad();
    else if (!strcmp(sc, "dronedemo")) scene_ambdemo(2);
    else if (!strcmp(sc, "paddemo")) scene_ambdemo(3);
    else if (!strcmp(sc, "song")) scene_song();
    else if (!strcmp(sc, "songva")) { song_va = 1; scene_song(); }
    else if (!strcmp(sc, "gransh")) scene_gransh();
    else scene_seq();
    return 0;
}
