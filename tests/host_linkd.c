/* SPDX-License-Identifier: GPL-3.0-only
 * The firmware's sample link as a process: a line of hex bytes in (a SysEx between F0 and F7), the reply's bytes out.
 * web/test_page.py runs the web page against it, so the page's frames meet the real gr_link.c.
 *   cc -O1 -Ifirmware/src -Ibuild/gen tests/host_linkd.c -o build/host_linkd -lm */
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
#include "gr_core.h"
#include "gr_params.c"
#include "gr_filter.c"
#include "gr_fx.c"
#include "gr_engine.c"
#include "gr_gen.c"
#include "gr_amb.c"
#include "gr_drift.c"
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
static uint8_t frame[700]; static uint32_t frame_n; static int frame_ready;
static int ota_wire_send(const uint8_t *p, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++) printf("%02x%c", p[i], i + 1 < n ? ' ' : '\n');
    fflush(stdout);
    return 0;
}
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
static int16_t smp_mem[NPART][SMP_FRAMES + 1];

int main(void)
{
    char line[4096];
    memset(nor, 0xFF, sizeof nor);
    gr_engine_init();
    gr_pt[0].smp.data = smp_mem[0]; gr_pt[1].smp.data = smp_mem[1];
    smp_scan();
    while (fgets(line, sizeof line, stdin)) {
        char *p = line;
        uint32_t n = 0;
        while (*p && *p != '\n') frame[n++] = (uint8_t)strtol(p, &p, 16);
        frame_n = n; frame_ready = 1;
        lk_service();
        if (frame_ready) { printf("-\n"); fflush(stdout); frame_ready = 0; }
    }
    return 0;
}
