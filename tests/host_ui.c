/* SPDX-License-Identifier: GPL-3.0-only
 * Host render of the Mini-Grain screens: every page to a PPM (tests/ui_png.py makes the PNGs).
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
#include "gr_drift.c"
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

static void settle(void)
{
    int i;
    for (i = 0; i < 3; i++) { fm1_ms += 700; ui_draw(); }
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "build/ui";
    uint32_t part, b, pg;
    ui_theme_init();
    dial_init();
    gr_params_defaults();
    gr_gen_defaults();
    gr_engine_init();
    for (part = 0; part < NGPART; part++) {
        gr_pt[part].smp.data = smp_mem[part];
        gr_sample_begin(part);
        str_cpy(gr_pt[part].smp.name, GR_GENTYPES[gr_gen[part].type].name, 16);
        gr_sample_end(part, gr_gen_render(part, smp_mem[part]), GEN_RATE);
    }
    ui.saved_mask = 0x0825;
    seq_init();
    {   static const uint8_t N[16] = {60, 0, 63, 0, 67, 0, 65, 0, 60, 62, 0, 70, 72, 0, 67, 0};
        uint32_t i; seq_step_t st;
        for (i = 0; i < 16; i++) if (N[i]) { step_blank(&st); st.time = STT_NOTE; st.notes = 0xFFFFFF00u | N[i]; st.vel = i % 5 == 0 ? 127 : 90 - i; if (i == 4) st.ratchet = 3; if (i == 8) st.slide = 1; if (i == 12) { st.nudge = 9; st.fill = FILL_ONLY; } seq_set(0, i, &st); }
        step_blank(&st); st.time = STT_TIE; seq_set(0, 5, &st); seq_set(0, 6, &st);
        seq_set_lock(0, 2, P_CUTOFF, 30000);
        ui.cursor[0] = 4; seq.playing = 1; seq.play_step[0] = 7; ui.arm_cell = -1;
    }
    gr_note_on(0, 60, 100); gr_note_on(0, 64, 100); gr_note_on(1, 72, 100);
    amb_poll(); gr_note_on(2, 48, 100); gr_note_on(3, 55, 100); gr_note_on(3, 59, 100);
    gr_set_norm(2, P_L1DEST, 65535 * 2 / 28); gr_set_norm(2, P_AMODDEPTH, 40000); gr_set_norm(3, P_ASTRUCT, 65535);
    gr_set_norm(0, P_L1DEST, 65535 * 4 / 28);
    gr_set_norm(0, P_FTYPE, 65535 * 2 / 4);
    for (b = 0; b < 600; b++) { int32_t l[128], r[128]; if (b % 8 == 0) amb_poll(); gr_render(l, r); }
    for (part = 0; part < NPART; part++) {
        for (pg = 0; pg < PG_COUNT; pg++) {
            uint32_t bank;
            char name[40];
            ui.page = (uint8_t)pg; ui.part = (uint8_t)part; ui.force = 1;
            ui.notice_ms = 0;
            if (pg == PG_GEN) { }
            for (bank = 0; bank < ui_nbanks(pg); bank++) {
                ui.bank = (uint8_t)bank;
                ui.turned[bank & 3] = fm1_ms;
                ui.force = 1;
                { int32_t l[128], r[128]; for (b = 0; b < 20; b++) gr_render(l, r); }
                fm1_ms += 100;
                settle();
                snprintf(name, sizeof name, "p%u_%u_%u", part, pg, bank);
                save(dir, name);
            }
        }
    }
    return 0;
}
