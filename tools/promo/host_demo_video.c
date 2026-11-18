/* SPDX-License-Identifier: GPL-3.0-only
 * The presentation video's scenes: the demo audio (tools/promo/demo_scenes.inc) and the real screens, frame by frame.
 *   host_demo_video SCENE ARG audio.raw frames.rgb   (30 fps, 240x240 RGB24, stereo s16le 44.1 kHz) */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define GR_POOL
#define memset gh_memset
#define memcpy gh_memcpy
#define memcmp gh_memcmp
static uint32_t fm1_ms = 5000;
static FILE *vid_audio, *vid_frames;
#define DEMO_AUDIO vid_audio
static uint16_t fb[240 * 240];
static uint32_t gr_midi_activity;
#define FELUCCA_VERSION "Mini-Grain 0.1"
#define GR_YIELD() ((void)0)
static void gr_render(int32_t *, int32_t *);
static void fm1_wdt_feed(void) { int32_t l[128], r[128]; fm1_ms++; gr_render(l, r); }
static void lcd_sync(void) {}
static void lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *px)
{
    uint32_t i, j;
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++)
            if (x + i < 240 && y + j < 240) {
                uint16_t v = px[j * w + i];
                fb[(y + j) * 240 + x + i] = (uint16_t)((v >> 8) | (v << 8));
            }
}
static void lcd_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t c)
{
    uint32_t i, j;
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++)
            if (x + i < 240 && y + j < 240) fb[(y + j) * 240 + x + i] = c;
}
#include "libc.c"
#include "gfx.c"
#include "gr_core.h"
#include "gr_params.c"
#include "gr_filter.c"
#include "gr_fx.c"
#include "gr_engine.c"
#include "gr_gen.c"
#include "gr_amb.c"
static void midi_out_event(uint32_t pkt) { (void)pkt; }
#include "gr_seq.c"
#define NSLOT 8u
typedef struct { uint32_t magic, frames, rate, bytes, crc; char name[16]; uint32_t rsv, hcrc; } smp_hdr_t;
static uint32_t smp_used = 0x105;
static uint8_t gr_src_kind[2] = {1, 0}, gr_src_slot[2] = {2, 0};
static int smp_hdr_read(uint32_t s, smp_hdr_t *h)
{
    static const char *const N[10] = {"kick_01", "", "Rhodes C4", "", "", "", "", "", "", "long_pad_name_2"};
    h->frames = 20000u + s * 4000u; h->rate = 22050 + s * 1000u;
    strcpy(h->name, N[s]);
    return 0;
}
#include "gr_ui.c"


static const char *vscene = "seq";
static double vt;
static uint32_t vframes;

static void vframe(void)
{
    unsigned char rgb[240 * 3];
    int y, x;
    for (y = 0; y < 240; y++) {
        for (x = 0; x < 240; x++) {
            uint16_t v = fb[y * 240 + x];
            rgb[x * 3] = (unsigned char)(((v >> 11) & 31) * 255 / 31);
            rgb[x * 3 + 1] = (unsigned char)(((v >> 5) & 63) * 255 / 63);
            rgb[x * 3 + 2] = (unsigned char)((v & 31) * 255 / 31);
        }
        fwrite(rgb, 1, sizeof rgb, vid_frames);
    }
}
static void go(uint32_t part, uint32_t page, uint32_t bank)
{
    if (ui.part != part || ui.page != page || ui.bank != bank) { ui.part = (uint8_t)part; ui.page = (uint8_t)page; ui.bank = (uint8_t)bank; ui.force = 1; }
}
static void script(double t)
{
    uint32_t cue = (uint32_t)(t * 1.6);
    ui.turned[cue & 3u] = fm1_ms;                               /* a dial seems to be turned */
    if (!strcmp(vscene, "gen")) {
        go(0, PG_GEN, t < 1.4 ? 0 : 1);
    } else if (!strcmp(vscene, "granular")) {
        go(0, t < 5 ? PG_GRAIN : t < 8 ? PG_GRAIN : t < 11 ? PG_FX : PG_MOD, t < 5 ? 0 : t < 8 ? 1 : 0);
    } else if (!strcmp(vscene, "drone")) {
        go(2, t < 5.5 ? PG_GRAIN : t < 10.5 ? PG_GRAIN : PG_VOICE, t < 5.5 ? 0 : t < 10.5 ? 1 : 0);
    } else if (!strcmp(vscene, "pad")) {
        go(3, t < 5.5 ? PG_GRAIN : t < 10.5 ? PG_GRAIN : PG_VOICE, t < 5.5 ? 0 : t < 10.5 ? 1 : 0);
    } else if (!strcmp(vscene, "dronedemo") || !strcmp(vscene, "paddemo")) {
        uint32_t p = vscene[0] == 'd' ? 2u : 3u;
        if (t < 12) go(p, PG_GRAIN, t < 6 ? 0 : 1);
        else if (t < 18) go(p, PG_VOICE, 1);
        else go(p, PG_VOICE, 2);
    } else if (!strcmp(vscene, "gransh")) {
        if (t < 18) go(0, PG_GRAIN, 0);
        else if (t < 34) go(0, PG_GRAIN, 1);
        else if (t < 38) go(0, PG_VOICE, 0);
        else if (t < 44) go(0, PG_MOD, t < 41 ? 0 : 1);
        else if (t < 50) go(0, PG_GEN, t < 47 ? 0 : 1);
        else go(0, PG_GRAIN, 0);
    } else if (!strcmp(vscene, "song") || !strcmp(vscene, "songva")) {
        if (t < 16) go(2, PG_GRAIN, t < 8 ? 0 : 1);
        else if (t < 32) go(3, PG_GRAIN, t < 24 ? 0 : 1);
        else if (t < 46) go(1, PG_GRAIN, t < 40 ? 0 : 1);
        else if (t < 58) { go(0, PG_STEPS, 0); ui.cursor[0] = (uint8_t)(seq.play_step[0] < 0 ? 0 : seq.play_step[0]); }
        else if (t < 68) go(2, PG_TOOLS, 0);
        else if (t < 76) go(3, PG_VOICE, 0);
        else if (t < 86) go(0, PG_FX, 0);
        else go(3, PG_GRAIN, 0);
    } else {
        if (t < 7) { go(0, PG_STEPS, 0); ui.cursor[0] = (uint8_t)(seq.play_step[0] < 0 ? 0 : seq.play_step[0]); }
        else if (t < 11) { go(0, PG_TOOLS, t < 9 ? 0 : 2); ui.cursor[0] = 0; ui.lockch = 0; }
        else if (t < 15) go(0, PG_SEQ, t < 13 ? 0 : 1);
        else if (t < 20) go(2, PG_GRAIN, t < 17.5 ? 0 : 1);
        else if (t < 25) go(3, PG_GRAIN, t < 22.5 ? 0 : 1);
        else go(0, PG_GRAIN, 0);
    }
}
static void video_hook(uint32_t b, uint32_t blocks)
{
    double t = (double)b * 128.0 / 44100.0;
    (void)blocks;
    while ((double)vframes / 30.0 <= t) {
        vt = (double)vframes / 30.0;
        fm1_ms = 5000u + (uint32_t)(vt * 1000.0);
        script(vt);
        ui_draw();
        vframe();
        vframes++;
    }
}
#define DEMO_HOOK(b, blocks) video_hook(b, blocks)
#include "demo_scenes.inc"

int main(int argc, char **argv)
{
    uint32_t part;
    const char *sc = argc > 1 ? argv[1] : "seq";
    vscene = sc;
    vid_audio = fopen(argv[3], "wb");
    vid_frames = fopen(argv[4], "wb");
    ui_theme_init();
    dial_init();
    gr_params_defaults(); gr_gen_defaults(); gr_engine_init();
    for (part = 0; part < NGPART; part++) {
        gr_pt[part].smp.data = demo_smp[part];
        gr_sample_begin(part);
        str_cpy(gr_pt[part].smp.name, GR_GENTYPES[gr_gen[part].type].name, 16);
        gr_sample_end(part, gr_gen_render(part, demo_smp[part]), GEN_RATE);
    }
    ui.saved_mask = 0x0825;
    ui.force = 1;
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
    fclose(vid_audio); fclose(vid_frames);
    return 0;
}
