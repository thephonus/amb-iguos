/* SPDX-License-Identifier: GPL-3.0-only
 * The state of the synth in flash: the current state (restored at power-on, saved a few seconds after the last
 * change while nothing sounds) and 16 presets, one to a storage object. A preset holds the parameters of the four
 * parts (MIDI channels and the sequencer's settings included), the granular parts' generator settings and which
 * stored sample each uses, and the four patterns with their parameter locks (only the steps that are in use are
 * written). A generated sound is remade
 * from its settings with the same result. Flash writes stop the audio for a moment, so nothing is saved while a note
 * sounds. */
#define ST_VER 7u
#define BLOB_MAX ST_PAYLOAD_MAX                  /* a preset's room: an object's payload */
#define PRESETS_PER_OBJ 1u
/* header 2 + per part: params + a pattern (8 + 64 x 8 + 1 + 24 x 4); the granular parts also generator type, length,
 * settings and source */
_Static_assert(2u + NPART * (P_COUNT * 2u + 8u + SEQ_STEPS * 8u + 1u + SEQ_LOCKS * 4u) +
                   NGPART * (1u + 2u + GEN_COUNT * 8u * 2u + 2u) <= BLOB_MAX, "a full state must fit a preset's room");

static uint8_t blob[BLOB_MAX];
static uint8_t pbuf[BLOB_MAX];
static uint8_t gr_flash_ok_state;                /* the state was read at boot */

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static uint32_t get16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }
static uint32_t get32(const uint8_t *p) { return get16(p) | get16(p + 2) << 16; }

/* the state -> b; returns its length */
static uint32_t state_pack(uint8_t *b)
{
    uint32_t part, i, t, k = 0, mask_at;
    b[k++] = (uint8_t)ST_VER;
    b[k++] = 1;
    for (part = 0; part < NPART; part++) {
        for (i = 0; i < P_COUNT; i++, k += 2u)
            put16(b + k, gr_ps[gr_is_global(i) ? 0 : part].norm[i]);
        if (!gr_ambient(part)) {
            b[k++] = gr_gen[part].type;
            put16(b + k, gr_gen[part].length);
            k += 2u;
            for (t = 0; t < GEN_COUNT; t++)
                for (i = 0; i < 8u; i++, k += 2u)
                    put16(b + k, gr_gen[part].norm[t][i]);
            b[k++] = gr_src_kind[part];
            b[k++] = gr_src_slot[part];
        }
        mask_at = k;                              /* the pattern: a bitmap of the steps in use, then those steps */
        for (i = 0; i < 8u; i++)
            b[k++] = 0;
        for (i = 0; i < SEQ_STEPS; i++)
            if (seq_tr[part].notes[i] != NO_NOTES || seq_tr[part].meta[i] != seq_empty_meta()) {
                b[mask_at + i / 8u] |= (uint8_t)(1u << (i % 8u));
                put32(b + k, seq_tr[part].notes[i]);
                put32(b + k + 4u, seq_tr[part].meta[i]);
                k += 8u;
            }
        mask_at = k++;                            /* the locks in use */
        b[mask_at] = 0;
        for (i = 0; i < SEQ_LOCKS; i++)
            if (seq_tr[part].locks[i] != LOCK_FREE) {
                put32(b + k, seq_tr[part].locks[i]);
                k += 4u;
                b[mask_at]++;
            }
    }
    return k;
}

/* b (len bytes) -> the state; 0, or -1 if it is not one of ours or is cut short */
static int state_unpack(const uint8_t *b, uint32_t len)
{
    uint32_t part, i, t, k = 2, n;
    if (len < 2u || b[0] != ST_VER || b[1] != 1)
        return -1;
    for (part = 0; part < NPART; part++) {
        if (k + P_COUNT * 2u + (gr_ambient(part) ? 0u : 1u + 2u + GEN_COUNT * 16u + 2u) + 8u > len)
            return -1;
        for (i = 0; i < P_COUNT; i++, k += 2u)
            gr_set_norm(part, i, (int32_t)get16(b + k));
        if (!gr_ambient(part)) {
            gr_gen[part].type = b[k] < GEN_COUNT ? b[k] : 0;
            k++;
            gr_gen[part].length = (uint16_t)get16(b + k);
            k += 2u;
            for (t = 0; t < GEN_COUNT; t++)
                for (i = 0; i < 8u; i++, k += 2u)
                    gr_gen[part].norm[t][i] = (uint16_t)get16(b + k);
            gr_src_kind[part] = b[k] == 1 ? 1 : 0;
            gr_src_slot[part] = b[k + 1] < NSLOT ? b[k + 1] : 0;
            k += 2u;
        }
        seq_clear_track(part);
        {
            const uint8_t *mask = b + k;
            k += 8u;
            for (i = 0; i < SEQ_STEPS; i++)
                if ((mask[i / 8u] >> (i % 8u)) & 1u) {
                    if (k + 8u > len)
                        return -1;
                    seq_tr[part].notes[i] = get32(b + k);
                    seq_tr[part].meta[i] = get32(b + k + 4u);
                    k += 8u;
                }
        }
        if (k + 1u > len)
            return -1;
        n = b[k++];
        for (i = 0; i < n && i < SEQ_LOCKS; i++, k += 4u) {
            if (k + 4u > len)
                return -1;
            seq_tr[part].locks[i] = get32(b + k);
        }
        seq_count_glides(part);
    }
    return 0;
}

/* renders the part's generator settings into its sample (the part falls silent while it works) */
static void gr_generate(uint32_t part)
{
    gr_part_t *pt = &gr_pt[part];
    uint32_t frames;
    gr_sample_begin(part);
    frames = gr_gen_render(part, pt->smp.data);
    str_cpy(pt->smp.name, GR_GENTYPES[gr_gen[part].type].name, sizeof pt->smp.name);
    pt->smp.kind = 1;
    gr_sample_end(part, frames, GEN_RATE);
    gr_src_kind[part] = 0;
}

/* a part's sound from its settings: the stored sample it names, else the generator's */
static void gr_make_sound(uint32_t part)
{
    if (gr_ambient(part))
        return;
    if (gr_src_kind[part] == 1 && smp_load(gr_src_slot[part], part) == LKE_OK)
        return;
    gr_generate(part);
}

/* ---------------------------------------------------- autosave, boot --- */
static uint32_t as_hash, as_dirty_ms, as_check_ms;

static uint32_t blob_hash(const uint8_t *b, uint32_t n)
{
    uint32_t h = 2166136261u, i;
    for (i = 0; i < n; i++)
        h = (h ^ b[i]) * 16777619u;
    return h;
}

static void presets_scan(void)                    /* which slots are in use */
{
    uint32_t o, mask = 0;
    for (o = 0; o < NPRESET_OBJ; o++)
        if (st_load(OBJ_PRESET0 + o, pbuf, sizeof pbuf) >= 2 && pbuf[0] == ST_VER && pbuf[1] == 1)
            mask |= 1u << o;
    ui.saved_mask = (uint16_t)mask;
}

static int preset_load(uint32_t slot)
{
    uint32_t part;
    int n = flash_ok ? st_load(OBJ_PRESET0 + slot, pbuf, sizeof pbuf) : -1;
    if (n < 2)
        return -1;
    seq.stop_req = 1;                             /* the transport stops: the patterns change */
    {   uint32_t t0 = fm1_ms;
        while (seq.stop_req && (uint32_t)(fm1_ms - t0) < 50u)
            fm1_wdt_feed();
    }
    if (state_unpack(pbuf, (uint32_t)n))
        return -1;
    for (part = 0; part < NPART; part++) {
        gr_cmd(CMD_ALL_OFF, part, 0, 0);
        gr_make_sound(part);
    }
    return 0;
}

static int preset_save(uint32_t slot)
{
    uint32_t len;
    if (!flash_ok)
        return -1;
    len = state_pack(blob);
    if (st_save(OBJ_PRESET0 + slot, blob, len))
        return -1;
    ui.saved_mask |= (uint16_t)(1u << slot);
    return 0;
}

static void state_defaults(void)
{
    gr_params_defaults();
    gr_gen_defaults();
    gr_src_kind[0] = gr_src_kind[1] = 0;
    seq.stop_req = 1;
    {   uint32_t part;
        for (part = 0; part < NPART; part++)
            gr_cmd(CMD_ALL_OFF, part, 0, 0);
    }
    seq_init_data();
}

static int16_t smp_mem[NGPART][SMP_FRAMES + 1] __attribute__((section(".pool")));
static void gr_samples_init(void)
{
    uint32_t part;
    for (part = 0; part < NGPART; part++)
        gr_pt[part].smp.data = smp_mem[part];
}

static void state_boot(void)
{
    uint32_t part, len;
    int n = flash_ok ? st_load(OBJ_SETTINGS, blob, sizeof blob) : -1;
    state_defaults();
    if (n > 0 && state_unpack(blob, (uint32_t)n) == 0)
        gr_flash_ok_state = 1;
    else
        state_defaults();                         /* (a half-read state is no state) */
    if (flash_ok)
        smp_scan();
    for (part = 0; part < NPART; part++)
        gr_make_sound(part);
    len = state_pack(blob);
    as_hash = blob_hash(blob, len);
    if (flash_ok)
        presets_scan();
}

/* The save erases a flash sector with the interrupts off: the audio and the LED scan stop for a moment. So it waits
 * until nothing has sounded on any of the four parts (a release tail counts) and no key is down, for a while: the
 * effects' tails are gone too. */
static uint32_t as_note_ms;

static void autosave_poll(void)
{
    uint32_t now = fm1_ms, h, len, part, busy = fm1_in.notes != 0u || seq.playing || seq.recording;
    if ((uint32_t)(now - as_check_ms) < 400u)
        return;
    as_check_ms = now;
    for (part = 0; part < NPART; part++)
        busy |= gr_st.notes[part] != 0u;
    if (busy)
        as_note_ms = now ? now : 1u;
    len = state_pack(blob);
    h = blob_hash(blob, len);
    if (h == as_hash) {
        as_dirty_ms = 0;
        return;
    }
    if (!as_dirty_ms)
        as_dirty_ms = now ? now : 1u;
    if ((uint32_t)(now - as_dirty_ms) < 3000u)
        return;
    if (busy || (uint32_t)(now - as_note_ms) < 3000u)     /* nothing is saved while it sounds, nor just after */
        return;
    if (!flash_ok || st_save(OBJ_SETTINGS, blob, len) == 0)
        as_hash = h;
    as_dirty_ms = 0;
}

static void persist_boot(void)
{
#if FELUCCA_FLASH
    uint32_t f = irq_save();
    flash_ok = FL_JEDEC_OK(FL_FAR(fl_jedec_ram)());           /* a known 1 MiB part, else stay RAM-only */
    irq_restore(f);
    if (flash_ok)
        fl_plain_window_init();                               /* flash above 0x93000 reads as plaintext through XIP */
#endif
}
