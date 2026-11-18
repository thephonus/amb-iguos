/* SPDX-License-Identifier: GPL-3.0-only
 * Stored samples and the USB link. A WAV sent over USB-MIDI (tools/grain_sample.py) is kept in flash as 4-bit IMA
 * ADPCM and decoded into a part's sample memory when picked on the Samples page; a sound generated on the device can
 * be stored the same way. Seven slots of 24 KiB at 0xB5000: a 256-byte header (written last, the commit) and up to
 * 24320 bytes of ADPCM (48640 samples; a part holds SMP_FRAMES).
 *
 * The link is SysEx F0 7D 47 52 cmd args.. F7 ("GR"), 7-bit data: numbers are 5 bytes LSB first, bytes are packed
 * 8 to 7 bits (ota_pack7). Every request gets a reply with the same cmd and a status byte (0 = ok).
 *   1 INFO                  -> version, slot count, SMP_FRAMES (5)
 *   2 LIST                  -> per slot: used, name (16), frames (5), rate (5)
 *   3 BEGIN slot name16 frames rate bytes   erases the slot
 *   4 WRITE slot offset(5) data             at most 256 bytes, offsets in order
 *   5 END slot crc(5)       checks the CRC-32 of what was written, then commits
 *   6 ERASE slot            7 LOAD slot part          loads a slot into a part (0 or 1) */
#define NSLOT 7u
#define SLOT_BASE 0xB5000u
#define SLOT_SIZE 0x6000u
#define SLOT_DATA 256u
#define SLOT_MAX_BYTES (SLOT_SIZE - SLOT_DATA)
#define SLOT_MAGIC 0x4D535247u                    /* "GRSM" */
enum { LK_INFO = 1, LK_LIST, LK_BEGIN, LK_WRITE, LK_END, LK_ERASE, LK_LOAD };
enum { LKE_OK, LKE_ARG, LKE_SIZE, LKE_FLASH, LKE_CRC, LKE_EMPTY, LKE_ORDER };

typedef struct {
    uint32_t magic, frames, rate, bytes, crc;
    char name[16];
    uint32_t rsv, hcrc;
} smp_hdr_t;
_Static_assert(sizeof(smp_hdr_t) <= SLOT_DATA, "the header fits its sector's head");

static uint8_t gr_src_kind[NPART], gr_src_slot[NPART];       /* a part's sound: 0 generated, 1 stored in a slot */
static uint32_t smp_used;                                     /* bit per slot */

static uint32_t smp_hcrc(const smp_hdr_t *h) { return st_crc32(h, sizeof *h - 4u); }

static int smp_hdr_read(uint32_t slot, smp_hdr_t *h)
{
    if (!flash_ok || slot >= NSLOT || st_read(SLOT_BASE + slot * SLOT_SIZE, h, sizeof *h))
        return -1;
    if (h->magic != SLOT_MAGIC || h->hcrc != smp_hcrc(h) || h->bytes > SLOT_MAX_BYTES || h->frames < 64u ||
        h->frames > SMP_FRAMES || h->bytes != (h->frames + 1u) / 2u)
        return -1;
    return 0;
}

static void smp_scan(void)
{
    uint32_t i;
    smp_hdr_t h;
    smp_used = 0;
    for (i = 0; i < NSLOT; i++)
        if (smp_hdr_read(i, &h) == 0)
            smp_used |= 1u << i;
}

static int smp_erase(uint32_t slot)
{
    uint32_t k;
    if (!flash_ok || slot >= NSLOT)
        return LKE_ARG;
    for (k = 0; k < SLOT_SIZE / ST_SECTOR; k++)
        if (GR_YIELD(), st_erase(SLOT_BASE + slot * SLOT_SIZE + k * ST_SECTOR))
            return LKE_FLASH;
    smp_used &= ~(1u << slot);
    return LKE_OK;
}

/* the CRC-32 of the stored data, read back */
static int smp_data_crc(uint32_t slot, uint32_t bytes, uint32_t *crc)
{
    uint8_t b[256];
    uint32_t off, c = 0xFFFFFFFFu;
    for (off = 0; off < bytes; off += sizeof b) {
        uint32_t n = bytes - off > sizeof b ? (uint32_t)sizeof b : bytes - off;
        if (st_read(SLOT_BASE + slot * SLOT_SIZE + SLOT_DATA + off, b, n))
            return -1;
        c = st_crc32_add(c, b, n);
        GR_YIELD();
    }
    *crc = ~c;
    return 0;
}

static int smp_commit(uint32_t slot, const char *name, uint32_t frames, uint32_t rate, uint32_t bytes, uint32_t crc)
{
    smp_hdr_t h;
    uint32_t got, i;
    if (smp_data_crc(slot, bytes, &got))
        return LKE_FLASH;
    if (got != crc)
        return LKE_CRC;
    h.magic = SLOT_MAGIC;
    h.frames = frames;
    h.rate = rate;
    h.bytes = bytes;
    h.crc = crc;
    for (i = 0; i < sizeof h.name; i++)
        h.name[i] = name[i] ? name[i] : 0;
    h.rsv = 0xFFFFFFFFu;
    h.hcrc = smp_hcrc(&h);
    if (st_prog(SLOT_BASE + slot * SLOT_SIZE, &h, sizeof h))
        return LKE_FLASH;
    smp_used |= 1u << slot;
    return LKE_OK;
}

/* ---------------------------------------------------- IMA ADPCM --- */
typedef struct { int32_t pred, idx; } ima_t;
static inline int32_t ima_dec(ima_t *s, uint32_t code)
{
    int32_t step = GR_IMA_STEP[s->idx], vd = step >> 3;
    if (code & 4u)
        vd += step;
    if (code & 2u)
        vd += step >> 1;
    if (code & 1u)
        vd += step >> 2;
    s->pred = clampi(code & 8u ? s->pred - vd : s->pred + vd, -32768, 32767);
    s->idx = clampi(s->idx + GR_IMA_IDX[code & 7u], 0, 88);
    return s->pred;
}
static inline uint32_t ima_enc(ima_t *s, int32_t x)
{
    int32_t step = GR_IMA_STEP[s->idx], diff = x - s->pred, vd = step >> 3;
    uint32_t code = 0;
    if (diff < 0) {
        code = 8;
        diff = -diff;
    }
    if (diff >= step) {
        code |= 4;
        diff -= step;
        vd += step;
    }
    if (diff >= step >> 1) {
        code |= 2;
        diff -= step >> 1;
        vd += step >> 1;
    }
    if (diff >= step >> 2) {
        code |= 1;
        vd += step >> 2;
    }
    s->pred = clampi(code & 8u ? s->pred - vd : s->pred + vd, -32768, 32767);
    s->idx = clampi(s->idx + GR_IMA_IDX[code & 7u], 0, 88);
    return code;
}

/* a slot -> a part's sample (the part is silent while it loads) */
static int smp_load(uint32_t slot, uint32_t part)
{
    smp_hdr_t h;
    uint8_t b[256];
    uint32_t off, i, n = 0, crc;
    ima_t st = {0, 0};
    gr_part_t *pt = &gr_pt[part];
    if (smp_hdr_read(slot, &h))
        return LKE_EMPTY;
    if (smp_data_crc(slot, h.bytes, &crc) || crc != h.crc)
        return LKE_CRC;
    gr_sample_begin(part);
    for (off = 0; off < h.bytes; off += sizeof b) {
        uint32_t k = h.bytes - off > sizeof b ? (uint32_t)sizeof b : h.bytes - off;
        if (st_read(SLOT_BASE + slot * SLOT_SIZE + SLOT_DATA + off, b, k)) {
            gr_sample_end(part, n > 64u ? n : 0u, h.rate);
            return LKE_FLASH;
        }
        for (i = 0; i < k && n < h.frames; i++) {
            pt->smp.data[n++] = (int16_t)ima_dec(&st, b[i] & 15u);
            if (n < h.frames)
                pt->smp.data[n++] = (int16_t)ima_dec(&st, b[i] >> 4);
        }
        GR_YIELD();
    }
    {
        char nm[16];
        for (i = 0; i < 16u; i++)
            nm[i] = h.name[i] >= 32 && h.name[i] < 127 ? h.name[i] : 0;
        nm[15] = 0;
        str_cpy(pt->smp.name, nm, sizeof pt->smp.name);
    }
    pt->smp.kind = 2;
    gr_sample_end(part, h.frames, h.rate);
    gr_src_kind[part] = 1;
    gr_src_slot[part] = (uint8_t)slot;
    return LKE_OK;
}

/* a part's sample -> the first free slot (-1: none free; else the slot or a negative error) */
static int smp_store(uint32_t part)
{
    const gr_sample_t *s = &gr_pt[part].smp;
    uint32_t slot, off = 0, i, crc = 0xFFFFFFFFu, bytes = (s->frames + 1u) / 2u;
    uint8_t b[256];
    ima_t st = {0, 0};
    int rc;
    if (!flash_ok || !s->frames)
        return -2;
    for (slot = 0; slot < NSLOT; slot++)
        if (!((smp_used >> slot) & 1u))
            break;
    if (slot == NSLOT)
        return -1;
    if ((rc = smp_erase(slot)) != 0)
        return -2;
    for (i = 0; i < bytes; i += 1u) {
        uint32_t lo = ima_enc(&st, s->data[2u * i]), hi = 2u * i + 1u < s->frames ? ima_enc(&st, s->data[2u * i + 1u]) : 0u;
        b[off++] = (uint8_t)(lo | hi << 4);
        if (off == sizeof b || i + 1u == bytes) {
            uint32_t at = i + 1u - off;
            crc = st_crc32_add(crc, b, off);
            if (st_prog(SLOT_BASE + slot * SLOT_SIZE + SLOT_DATA + at, b, off))
                return -2;
            off = 0;
            GR_YIELD();
        }
    }
    {
        char nm[16];
        str_cpy(nm, s->name, sizeof nm);
        if (smp_commit(slot, nm, s->frames, s->rate, bytes, ~crc))
            return -2;
    }
    return (int)slot;
}

/* --------------------------------------------------------- link --- */
static uint8_t lk_out[320];
static uint32_t lk_n;
static struct { uint8_t on, slot; uint32_t frames, rate, bytes, next; char name[16]; } lk;

static void lk_begin(uint32_t cmd, uint32_t status)
{
    lk_out[0] = 0xF0;
    lk_out[1] = 0x7D;
    lk_out[2] = 0x47;
    lk_out[3] = 0x52;
    lk_out[4] = (uint8_t)cmd;
    lk_out[5] = (uint8_t)status;
    lk_n = 6;
}
static void lk_b(uint32_t v)
{
    if (lk_n < sizeof lk_out - 1u)
        lk_out[lk_n++] = (uint8_t)(v & 0x7Fu);
}
static void lk_u32(uint32_t v)
{
    uint32_t i;
    for (i = 0; i < 5u; i++, v >>= 7)
        lk_b(v);
}
static void lk_send(void)
{
    lk_out[lk_n++] = 0xF7;
    ota_wire_send(lk_out, lk_n);
}
static uint32_t lk_rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 7 | (uint32_t)p[2] << 14 | (uint32_t)p[3] << 21 | (uint32_t)(p[4] & 15u) << 28; }

static void lk_handle(const uint8_t *p, uint32_t n)           /* p: after the three header bytes: cmd, args */
{
    uint32_t cmd = p[0], st = LKE_OK, i, slot = n > 1u ? p[1] : 0u;
    switch (cmd) {
    case LK_INFO:
        lk_begin(cmd, 0);
        lk_b(1);                                              /* protocol version */
        lk_b(NSLOT);
        lk_u32(SMP_FRAMES);
        lk_u32(SLOT_MAX_BYTES);
        break;
    case LK_LIST:
        lk_begin(cmd, 0);
        for (slot = 0; slot < NSLOT; slot++) {
            smp_hdr_t h;
            int ok = smp_hdr_read(slot, &h) == 0;
            lk_b((uint32_t)ok);
            for (i = 0; i < 16u; i++)
                lk_b(ok ? (uint8_t)h.name[i] : 0u);
            lk_u32(ok ? h.frames : 0u);
            lk_u32(ok ? h.rate : 0u);
        }
        break;
    case LK_BEGIN:
        if (n < 33u || slot >= NSLOT) {
            st = LKE_ARG;
        } else {
            lk.slot = (uint8_t)slot;
            for (i = 0; i < 16u; i++)
                lk.name[i] = (char)p[2 + i];
            lk.name[15] = 0;
            lk.frames = lk_rd32(p + 18);
            lk.rate = lk_rd32(p + 23);
            lk.bytes = lk_rd32(p + 28);
            if (lk.frames < 64u || lk.frames > SMP_FRAMES || lk.bytes != (lk.frames + 1u) / 2u || lk.rate < 4000u || lk.rate > 96000u)
                st = LKE_SIZE;
            else if ((st = (uint32_t)smp_erase(slot)) == LKE_OK) {
                lk.on = 1;
                lk.next = 0;
            }
        }
        lk_begin(cmd, st);
        break;
    case LK_WRITE: {
        static uint8_t d[320];
        uint32_t off, len;
        if (!lk.on || n < 8u || slot != lk.slot) {
            st = LKE_ORDER;
        } else {
            off = lk_rd32(p + 2);
            len = ota_unpack7(p + 7, n - 7u, d, sizeof d);
            if (off != lk.next || len == 0u || len > 256u || (off & 255u) || off + len > lk.bytes)
                st = LKE_SIZE;
            else if (st_prog(SLOT_BASE + slot * SLOT_SIZE + SLOT_DATA + off, d, len))
                st = LKE_FLASH;
            else
                lk.next = off + len;
        }
        lk_begin(cmd, st);
        lk_u32(lk.next);
        break;
    }
    case LK_END:
        if (!lk.on || n < 7u || slot != lk.slot || lk.next != lk.bytes)
            st = LKE_ORDER;
        else {
            st = (uint32_t)smp_commit(slot, lk.name, lk.frames, lk.rate, lk.bytes, lk_rd32(p + 2));
            lk.on = 0;
        }
        lk_begin(cmd, st);
        break;
    case LK_ERASE:
        st = (uint32_t)smp_erase(slot);
        lk_begin(cmd, st);
        break;
    case LK_LOAD:
        if (n < 3u || p[2] >= NPART)
            st = LKE_ARG;
        else {
            gr_cmd(CMD_ALL_OFF, p[2], 0, 0);
            st = (uint32_t)smp_load(slot, p[2]);
        }
        lk_begin(cmd, st);
        break;
    default:
        return;
    }
    lk_send();
}

/* main loop: link frames first; anything else stays for ota_service() */
static void lk_service(void)
{
    const uint8_t *p;
    uint32_t n;
    if (!ota_frame_get(&p, &n) || n < 4u || p[0] != 0x7Du || p[1] != 0x47u || p[2] != 0x52u)
        return;
    {
        static uint8_t f[320];
        uint32_t k = n - 3u > sizeof f ? (uint32_t)sizeof f : n - 3u, i;
        for (i = 0; i < k; i++)
            f[i] = p[3u + i];
        ota_frame_done();                                     /* free the frame buffer before replying */
        lk_handle(f, k);
    }
}
