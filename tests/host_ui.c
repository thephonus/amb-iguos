/* SPDX-License-Identifier: GPL-3.0-only
 * Host render of the amb-iguos screens: every section, page and layer to a PPM (tests/ui_png.py makes the PNGs).
 *   cc -O1 -Wall -Ifirmware/src -Ibuild/gen tests/host_ui.c -o build/host_ui -lm && build/host_ui build/ui */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GR_POOL
#define memset gh_memset
#define memcpy gh_memcpy
#define memcmp gh_memcmp
static uint32_t fm1_ms = 5000;
static uint16_t fb[240 * 240];
static uint32_t gr_midi_activity;
#define FELUCCA_VERSION "amb-iguos 0.1"
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
#include "gr_drift.c"
static void midi_out_event(uint32_t pkt) { (void)pkt; }
#include "gr_seq.c"
#define NSLOT 7u
enum { LKE_OK, LKE_ARG };
typedef struct { uint32_t magic, frames, rate, bytes, crc; char name[16]; uint32_t rsv, hcrc; } smp_hdr_t;
static uint32_t smp_used = 0x05;
static uint8_t gr_src_kind[NPART], gr_src_slot[NPART];
static int smp_hdr_read(uint32_t s, smp_hdr_t *h)
{
    static const char *const N[7] = {"lluvia_patio", "", "piano_viejo", "", "", "", ""};
    h->frames = 20000u + s * 4000u; h->rate = 22050 + s * 1000u;
    strcpy(h->name, N[s]);
    return 0;
}
static struct { uint8_t config, suspended; } usb = {0, 0};
#include "gr_ui.c"
static int32_t batt_level(void) { return 2; }
static int32_t batt_pct(void) { return 64; }

static int16_t smp_mem[NPART][SMP_FRAMES + 1];

static void save(const char *dir, const char *name)
{
    char path[256];
    FILE *f;
    int i;
    snprintf(path, sizeof path, "%s/%s.ppm", dir, name);
    f = fopen(path, "wb");
    fprintf(f, "P6\n240 240\n255\n");
    for (i = 0; i < 240 * 240; i++) {
        uint16_t v = fb[i];
        unsigned char rgb[3] = {(unsigned char)(((v >> 11) & 31) * 255 / 31), (unsigned char)(((v >> 5) & 63) * 255 / 63), (unsigned char)((v & 31) * 255 / 31)};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

static void run(uint32_t blocks)
{
    uint32_t b;
    for (b = 0; b < blocks; b++) { int32_t l[128], r[128]; if (b % 8 == 0) amb_poll(); dr_block(); gr_render(l, r); }
}

static void settle(void)
{
    int i;
    for (i = 0; i < 3; i++) { fm1_ms += 700; run(20); ui_draw(); }
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "build/ui";
    uint32_t part, sc;
    ui_theme_init();
    dial_init();
    gr_params_defaults();
    gr_gen_defaults();
    gr_engine_init();
    gr_gen[0].type = GT_WAVETABLE;
    gr_gen[1].type = GT_BOWWIND;
    for (part = 0; part < NGPART; part++) {
        gr_pt[part].smp.data = smp_mem[part];
        gr_sample_begin(part);
        str_cpy(gr_pt[part].smp.name, GR_GENTYPES[gr_gen[part].type].name, 16);
        gr_sample_end(part, gr_gen_render(part, smp_mem[part]), GEN_RATE);
    }
    ui.saved_mask = 0x0825;
    gr_set_norm(0, P_SIZE, 50000); gr_set_norm(0, P_DENSITY, 40000); gr_set_norm(0, P_SPREAD, 20000);
    gr_set_norm(0, P_SHIMMER, 40000); gr_set_norm(0, P_MACRO3, 45000); gr_set_norm(0, P_MACRO1, 26000);
    gr_set_norm(0, P_L1DEST, 65535 * 4 / 28); gr_set_norm(0, P_FTYPE, 65535 * 2 / 4);
    gr_set_norm(0, P_DRATE, 4000); gr_set_norm(0, P_DDRONE, 65535);
    amb_poll();
    dr_on = 1;
    run(6000);
    for (part = 0; part < NPART; part++) {
        for (sc = 0; sc < SC_COUNT; sc++) {
            uint32_t sub, n;
            char name[40];
            ui.part = (uint8_t)part;
            ui.page = (uint8_t)sc;
            n = ui_npages(sc);
            for (sub = 0; sub < n; sub++) {
                ui.sub[sc] = (uint8_t)sub;
                ui.force = 1;
                ui.notice_ms = 0;
                ui.turned[sub & 3] = fm1_ms + 2100;            /* a dial being turned */
                settle();
                snprintf(name, sizeof name, "s%u_%u_%u", part, sc, sub);
                save(dir, name);
            }
        }
    }
    /* the Freeze, on the scene */
    dr_freeze_set(1);
    ui.part = 0; ui.page = SC_ESCENA; ui.sub[SC_ESCENA] = 0; ui.force = 1;
    settle();
    save(dir, "freeze");
    return 0;
}
