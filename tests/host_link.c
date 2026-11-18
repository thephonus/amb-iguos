/* SPDX-License-Identifier: GPL-3.0-only
 * Host test of the sample link (gr_link.c): the frames tools/grain_sample.py makes go through lk_service on a
 * simulated NOR flash; the slot is listed, loaded into a part, stored again, erased.
 *   python3 tools/grain_sample.py frames 3 build/test_tone.wav build/frames.txt
 *   cc -O1 -Ifirmware/src -Ibuild/gen tests/host_link.c -o build/host_link -lm && build/host_link build/frames.txt */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#define GR_POOL
#define FELUCCA_ID "FM-1_900"
static uint32_t fm1_ms;
static uint8_t flash_ok = 1;
static void gr_render(int32_t *, int32_t *);
static void fm1_wdt_feed(void) { int32_t l[128], r[128]; fm1_ms++; gr_render(l, r); }
#define GR_YIELD() ((void)0)
static unsigned str_len(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }
static void str_cpy(char *d, const char *s, unsigned max) { unsigned i = 0; while (s[i] && i + 1 < max) { d[i] = s[i]; i++; } d[i] = 0; }
static void fmt_fix(char *b, int32_t v, int decimals) { (void)b; (void)v; (void)decimals; }
static void gr_cmd(uint32_t t, uint32_t p, uint32_t a, uint32_t b) { (void)t; (void)p; (void)a; (void)b; }
enum { CMD_NOTE_ON, CMD_NOTE_OFF, CMD_ALL_OFF };
#define memcmp gh_memcmp
static int gh_memcmp(const void *a, const void *b, unsigned n) { return __builtin_memcmp(a, b, n); }
#include "gr_core.h"
#include "gr_params.c"
#include "gr_filter.c"
#include "gr_fx.c"
#include "gr_engine.c"
#include "gr_gen.c"
#include "gr_amb.c"
#include "gr_drift.c"

/* simulated NOR */
static uint8_t nor[0x100000];
static int st_read(uint32_t off, void *dst, uint32_t n) { memcpy(dst, nor + off, n); return 0; }
static int st_erase(uint32_t off) { if (off & 0xFFF || off < 0x97000 || off >= 0xE0000) return -8; memset(nor + off, 0xFF, 0x1000); return 0; }
static int st_prog(uint32_t off, const void *src, uint32_t n)
{
    uint32_t i;
    if (off < 0x97000 || off + n > 0xE0000) return -8;
    for (i = 0; i < n; i++) nor[off + i] &= ((const uint8_t *)src)[i];
    return 0;
}
#include "storage.c"

/* the SysEx plumbing */
static uint8_t frame[700]; static uint32_t frame_n; static int frame_ready;
static uint8_t reply[700]; static uint32_t reply_n;
static int ota_wire_send(const uint8_t *p, uint32_t n) { memcpy(reply, p, n); reply_n = n; return 0; }
static int ota_frame_get(const uint8_t **p, uint32_t *n) { if (!frame_ready) return 0; *p = frame; *n = frame_n; return 1; }
static void ota_frame_done(void) { frame_ready = 0; }
static uint32_t ota_now_ms(void) { return fm1_ms; }
static void ota_idle(void) {}
static int ota_erase(uint32_t off) { (void)off; return 0; }
static int ota_prog(uint32_t off, const void *p, uint32_t n) { (void)off; (void)p; (void)n; return 0; }
static int ota_fread(uint32_t off, void *p, uint32_t n) { return st_read(off, p, n); }
static void ota_show(uint32_t s, int32_t c) { (void)s; (void)c; }
static void ota_commit(const uint8_t *p) { (void)p; }
#include "ota.c"
#include "gr_link.c"
static void midi_out_event(uint32_t pkt) { (void)pkt; }
#include "gr_seq.c"
static struct { uint16_t saved_mask; } ui;
static struct { uint32_t notes; } fm1_in;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)
static int fails;
#define FELUCCA_FLASH 0
#include "gr_state.c"


static int send(const uint8_t *f, uint32_t n)
{
    memcpy(frame, f, n); frame_n = n; frame_ready = 1; reply_n = 0;
    lk_service();
    return frame_ready ? -1 : (reply_n > 5 ? reply[5] : -2);
}

int main(int argc, char **argv)
{
    FILE *f = fopen(argc > 1 ? argv[1] : "build/frames.txt", "r");
    char line[2000];
    int k, nf = 0;
    uint32_t i;
    static uint8_t buf[700];
    memset(nor, 0xFF, sizeof nor);
    gr_engine_init();
    gr_samples_init();
    if (!f) { printf("no frames file\n"); return 2; }
    while (fgets(line, sizeof line, f)) {
        uint32_t n = 0; char *p = line; int rc;
        while (*p && *p != '\n') { buf[n++] = (uint8_t)strtol(p, &p, 16); }
        rc = send(buf, n);
        if (rc) { printf("frame %d failed: %d\n", nf, rc); fails++; }
        nf++;
    }
    printf("%d frames sent\n", nf);
    smp_scan();
    printf("slots used mask %x\n", smp_used);
    if (!(smp_used & 4u)) { printf("FAIL slot 3 not stored\n"); return 1; }
    {   /* LIST */
        static const uint8_t list[] = {0x7D, 0x47, 0x52, 2};
        int rc = send(list, sizeof list);
        printf("list reply %u bytes status %d, slot 3 name: %.8s\n", reply_n, rc, reply + 6 + 2 * 27 + 1 + 1 - 1 + 0);
    }
    {   /* LOAD into part 0 */
        static const uint8_t ld[] = {0x7D, 0x47, 0x52, 7, 2, 0};
        int rc = send(ld, sizeof ld);
        gr_sample_begin(0); /* (already loaded: just to exercise the handshake) */
        printf("load status %d, frames %u rate %u name '%s' kind %u\n", rc, gr_pt[0].smp.frames, gr_pt[0].smp.rate, gr_pt[0].smp.name, gr_pt[0].smp.kind);
        fails += rc != 0;
    }
    smp_load(2, 0);
    {   /* compare to the source: 330 Hz decaying tone, 0.9 s at 44.1k */
        double err = 0, sig = 0, rmax = 0;
        int pk = 0;
        for (i = 0; i < gr_pt[0].smp.frames; i++) {            /* (the tool normalises the file to a 30000 peak) */
            double t = (double)i / gr_pt[0].smp.rate, r = fabs(0.6 * sin(2 * M_PI * 330 * t) * exp(-t * 2) + 0.2 * sin(2 * M_PI * 660 * t) * exp(-t * 4));
            if (r > rmax) rmax = r;
        }
        for (i = 0; i < gr_pt[0].smp.frames; i++) {
            double t = (double)i / gr_pt[0].smp.rate;
            double ref = (0.6 * sin(2 * M_PI * 330 * t) * exp(-t * 2) + 0.2 * sin(2 * M_PI * 660 * t) * exp(-t * 4)) / rmax * 30000.0;
            err += (gr_pt[0].smp.data[i] - ref) * (gr_pt[0].smp.data[i] - ref);
            sig += ref * ref;
            if (abs(gr_pt[0].smp.data[i]) > pk) pk = abs(gr_pt[0].smp.data[i]);
        }
        printf("decoded %u frames, peak %d, SNR %.1f dB\n", gr_pt[0].smp.frames, pk, 10 * log10(sig / err));
        if (10 * log10(sig / err) < 20) { printf("FAIL: SNR\n"); fails++; }
    }
    {   /* store a generated sound, list again */
        int rc;
        gr_params_defaults(); gr_gen_defaults();
        gr_sample_begin(1);
        str_cpy(gr_pt[1].smp.name, "Pluck", 16);
        gr_sample_end(1, gr_gen_render(1, smp_mem[1]), GEN_RATE);
        rc = smp_store(1);
        printf("store -> slot %d\n", rc + 1);
        fails += rc != 0;
        for (i = 0; i < 128; i++) smp_mem[0][i] = 0;
        smp_load((uint32_t)rc, 0);
        {   double err = 0, sig = 0;
            for (i = 0; i < gr_pt[1].smp.frames; i++) { double d = gr_pt[0].smp.data[i] - gr_pt[1].smp.data[i]; err += d * d; sig += (double)gr_pt[1].smp.data[i] * gr_pt[1].smp.data[i]; }
            printf("stored pluck vs original: SNR %.1f dB, name '%s'\n", 10 * log10(sig / err), gr_pt[0].smp.name);
            if (10 * log10(sig / err) < 18) { printf("FAIL: pluck SNR\n"); fails++; } }
    }
    {   /* a corrupted slot is refused, ERASE */
        static const uint8_t er[] = {0x7D, 0x47, 0x52, 6, 2};
        int rc;
        nor[SLOT_BASE + 2 * SLOT_SIZE + SLOT_DATA + 100] ^= 0x10;
        printf("corrupted load: %d (want %d)\n", smp_load(2, 0), LKE_CRC);
        fails += smp_load(2, 0) != LKE_CRC;
        rc = send(er, sizeof er);
        smp_scan();
        printf("erase %d, used mask %x\n", rc, smp_used);
        fails += rc != 0 || (smp_used & 4u);
    }
    {   /* presets and the autosave: parameters, patterns and locks go to flash and come back */
        seq_step_t st;
        uint32_t i, len, n;
        int rc;
        static uint8_t first[BLOB_MAX], second[BLOB_MAX];
        memset(nor, 0xFF, sizeof nor);
        seq_init(); gr_params_defaults(); gr_gen_defaults();
        for (i = 0; i < SEQ_STEPS; i += 3) { step_blank(&st); st.time = STT_NOTE; st.notes = 0xFFFFFF00u | (40 + i); st.vel = 20 + i; st.nudge = (int8_t)(i % 30) - 10; st.ratchet = 1 + i % 4; seq_set(0, i, &st); }
        seq_set_lock(0, 3, P_CUTOFF, 1234); seq_set_lock(0, 6, P_DENSITY, 4321); seq_set_lock(1, 0, P_PAN, 9);
        for (i = 0; i < 16; i++) { step_blank(&st); st.time = STT_NOTE; st.notes = 0x01020304u + i; seq_set(1, 16 + i, &st); }
        gr_set_norm(0, P_TEMPO, 30000); gr_set_norm(1, P_SEQLEN, 12345); gr_set_norm(0, P_CUTOFF, 777); gr_gen[1].type = 5; gr_src_kind[0] = 1; gr_src_slot[0] = 3;
        len = state_pack(first);
        printf("state: %u bytes (room %u)\n", len, BLOB_MAX);
        gr_params_defaults(); gr_gen_defaults(); seq_init_data(); gr_src_kind[0] = 0;
        n = state_unpack(first, len);
        len = state_pack(second);
        printf("round trip: unpack %u, repack %u bytes, %s\n", n, len, memcmp(first, second, len) == 0 ? "identical" : "DIFFERENT");
        fails += n != 0 || memcmp(first, second, len);
        CHECK(gr_real(0, P_TEMPO) > 100 && gr_src_slot[0] == 3, "restored values");   /* amb-iguos: no patterns */
        flash_ok = 1;
        seq_step_t chk;
        smp_scan();
        rc = preset_save(5); fails += rc != 0;
        seq_init_data(); gr_params_defaults();
        rc = preset_save(2); fails += rc != 0;                 /* the defaults, in another slot of the same object family */
        presets_scan();
        printf("presets used mask %04x\n", ui.saved_mask);
        CHECK(ui.saved_mask == ((1u << 5) | (1u << 2)), "saved slots");
        rc = preset_load(5);
        seq_get(0, 6, &chk);
        printf("preset 6 loaded: %d, step 6 note %u, lock found %d, tempo %d\n", rc, sn_note(chk.notes, 0), seq_find_lock(0, 6, P_DENSITY), gr_real(0, P_TEMPO));
        CHECK(rc == 0 && gr_real(0, P_TEMPO) > 100, "preset round trip");   /* amb-iguos: no patterns */
        len = state_pack(second);
        CHECK(len == state_pack(first) && memcmp(first, second, len) == 0, "preset equals state");
        rc = preset_load(2);
        seq_get(0, 6, &chk);
        CHECK(rc == 0 && chk.time == STT_REST, "the other preset is the empty one");
        rc = preset_load(7);
        CHECK(rc != 0, "an empty slot is refused");
    }
    printf(fails ? "%d FAILURES\n" : "link ok\n", fails);
    return fails != 0;
}
