/* SPDX-License-Identifier: GPL-3.0-only
 * The granular engine: two parts, each with up to 8 notes, a master filter and two LFOs, sharing a grain pool, a
 * delay, a reverb and the master output. A port of the Granular synth's Engine.h to fixed point.
 * gr_render() runs in the audio interrupt: it never waits, allocates or does flash access. */

/* ---------------------------------------------------------- state --- */
typedef struct {
    uint8_t active, gate, sustained, stealing, finished, note, vel, pendvel;
    int16_t pending;                 /* a note waiting for this voice to fade, or -1 */
    int32_t env, env_prev;           /* Q15 */
    int32_t glide, glide_step;       /* semitones still to travel in a slide, and the step a block (Q8) */
    uint32_t scan;                   /* Q32 of the sample: this note's scan offset */
    int32_t next;                    /* frames to the next grain, Q8 */
    uint32_t order;
} gr_voice_t;

typedef struct {
    uint32_t pos;                    /* Q16 frames */
    int32_t inc;                     /* Q16 frames a sample (negative: reverse) */
    uint32_t phase, phase_inc, remaining;
    int32_t amp_l, amp_r;            /* Q15 */
    uint16_t start;
    int8_t voice;
    uint8_t part, active, shedding;
} gr_grain_t;

typedef struct {
    uint32_t phase;
    int32_t out;                     /* Q15 */
    int32_t from, to;                /* sample and hold, smooth random */
    uint8_t started;
} gr_lfo_t;

enum { SMP_NORMAL, SMP_REQ, SMP_SILENT };

typedef struct {
    gr_voice_t v[NNOTE];
    gr_lfo_t lfo[2];
    gr_filter_t flt;
    int32_t lock[P_COUNT];           /* the sequencer's parameter locks: a norm, or -1 */
    int32_t val[P_COUNT];            /* real values this block (x mul), LFO and locks included */
    uint16_t vnorm[P_COUNT];         /* norms this block */
    int32_t off[P_COUNT];            /* LFO movement, norm units (Q16, signed) */
    int32_t lfo_depth[2];
    int32_t bus_l[BLK], bus_r[BLK];
    int32_t bend_q8;
    int32_t level, dsend, rsend;     /* Q15, smoothed */
    uint8_t sustain, idle, smp_state;
    uint16_t quiet;
    int16_t running, limit, notes;
    gr_sample_t smp;
    uint32_t rr_q16;                 /* sample rate / FS, Q16 */
    uint32_t scan_k;                 /* scan speed x1000 -> sample fraction per block, Q32 */
} gr_part_t;

static gr_part_t gr_pt[NPART];
static uint32_t amb_budget = AMB_BUDGET, amb_raise = 16;
/* the drone and the pad (gr_amb.c) */
static void amb_note_on(uint32_t part, uint32_t note, uint32_t vel);
static void amb_note_off(uint32_t part, uint32_t note);
static void amb_sustain(uint32_t part, uint32_t on);
static void amb_bend(uint32_t part, int32_t bend);
static void amb_all_notes_off(uint32_t part);
static void amb_all_sound_off(uint32_t part);
static void amb_render(uint32_t part);
static uint32_t amb_voices(uint32_t part);
static gr_grain_t gr_gr[NGRAIN];
static gr_stats_t gr_st;
static int32_t gr_cap = NGRAIN, gr_raise = 16, gr_active_grains;
static uint32_t gr_order, gr_view_cnt;
static int32_t gr_vol_q15;           /* smoothed master */
static int32_t gr_master_q12 = 2048; /* the MASTER knob (main.c / master.c) */
static int32_t gr_send_dl[BLK], gr_send_dr[BLK], gr_send_rl[BLK], gr_send_rr[BLK];
static int32_t gr_mix_l[BLK], gr_mix_r[BLK];

/* 128 frames in ms x 16, as a Q15 step per ms x 16 of an envelope time */
#define BLK_ENV_K ((int32_t)(67108864000ull / 44100ull))

static uint64_t udiv64(uint64_t n, uint64_t d)           /* shift and subtract: no library call */
{
    uint64_t q = 0, r = 0;
    int i;
    for (i = 0; i < 64; i++) {                      /* constant shifts only */
        r = (r << 1) | (n >> 63);
        n <<= 1;
        q <<= 1;
        if (r >= d) {
            r -= d;
            q |= 1u;
        }
    }
    return q;
}

/* a part's sample was (re)loaded: the constants that depend on it */
static void gr_part_sample_set(uint32_t part)
{
    gr_part_t *pt = &gr_pt[part];
    uint32_t frames = pt->smp.frames, rate = pt->smp.rate ? pt->smp.rate : GEN_RATE;
    pt->rr_q16 = (rate << 16) / FS;
    /* scan x1000 -> fraction of the sample per block: BLK * rate / (FS * frames) * 2^32 / 1000 */
    pt->scan_k = frames ? (uint32_t)udiv64((uint64_t)BLK * rate * 4294967296ull, (uint64_t)FS * frames * 1000u) : 0u;
}

/* ------------------------------------------------------------ voices --- */
static void gr_wake(gr_part_t *pt)
{
    if (pt->idle) {
        uint32_t i;
        for (i = 0; i < BLK; i++)
            pt->bus_l[i] = pt->bus_r[i] = 0;
    }
    pt->idle = 0;
    pt->quiet = 0;
}

static void gr_voice_start(gr_part_t *pt, gr_voice_t *v, uint32_t note, uint32_t vel)
{
    v->active = 1;
    v->gate = 1;
    v->sustained = v->stealing = v->finished = 0;
    v->note = (uint8_t)note;
    v->vel = (uint8_t)vel;
    v->pending = -1;
    v->env = v->env_prev = 0;
    v->glide = v->glide_step = 0;
    v->scan = 0;
    v->next = 0;
    v->order = ++gr_order;
    gr_wake(pt);
}

static gr_voice_t *gr_note_on(uint32_t part, uint32_t note, uint32_t vel)
{
    gr_part_t *pt = &gr_pt[part];
    gr_voice_t *victim = 0;
    uint32_t i;
    if (gr_ambient(part)) {
        gr_wake(pt);
        amb_note_on(part, note, vel);
        return 0;
    }
    for (i = 0; i < NNOTE; i++) {
        gr_voice_t *v = &pt->v[i];
        if (v->active && v->gate && !v->stealing && v->note == note) {          /* the same key from another input */
            v->vel = (uint8_t)vel;
            v->sustained = 0;
            return 0;
        }
    }
    for (i = 0; i < NNOTE; i++)
        if (!pt->v[i].active) {
            gr_voice_start(pt, &pt->v[i], note, vel);
            return &pt->v[i];
        }
    for (i = 0; i < NNOTE; i++) {                 /* no free voice: fade the oldest, a released note first */
        gr_voice_t *v = &pt->v[i];
        if (v->stealing)
            continue;
        if (!victim || (victim->gate && !v->gate) ||
            (victim->gate == v->gate && (int32_t)(v->order - victim->order) < 0))
            victim = v;
    }
    if (!victim)
        victim = &pt->v[0];
    victim->gate = 0;
    victim->sustained = 0;
    victim->stealing = 1;
    victim->pending = (int16_t)note;
    victim->pendvel = (uint8_t)vel;
    return 0;
}

static void gr_note_off(uint32_t part, uint32_t note)
{
    gr_part_t *pt = &gr_pt[part];
    uint32_t i;
    if (gr_ambient(part)) {
        amb_note_off(part, note);
        return;
    }
    for (i = 0; i < NNOTE; i++) {
        gr_voice_t *v = &pt->v[i];
        if (!v->active)
            continue;
        if (v->pending == (int16_t)note)
            v->pending = -1;
        if (v->gate && v->note == note) {
            if (pt->sustain)
                v->sustained = 1;
            else
                v->gate = 0;
        }
    }
}

static void gr_sustain(uint32_t part, uint32_t on)
{
    gr_part_t *pt = &gr_pt[part];
    uint32_t i;
    if (gr_ambient(part)) {
        amb_sustain(part, on);
        return;
    }
    pt->sustain = (uint8_t)on;
    if (on)
        return;
    for (i = 0; i < NNOTE; i++)
        if (pt->v[i].sustained) {
            pt->v[i].sustained = 0;
            pt->v[i].gate = 0;
        }
}

static void gr_bend(uint32_t part, int32_t bend)          /* -8192 .. 8191, +-2 semitones */
{
    if (gr_ambient(part)) {
        amb_bend(part, bend);
        return;
    }
    gr_pt[part].bend_q8 = (bend * 2 * 256) / 8192;
}

static void gr_all_notes_off(uint32_t part)
{
    gr_part_t *pt = &gr_pt[part];
    uint32_t i;
    if (gr_ambient(part)) {
        amb_all_notes_off(part);
        return;
    }
    pt->sustain = 0;
    for (i = 0; i < NNOTE; i++) {
        pt->v[i].gate = 0;
        pt->v[i].sustained = 0;
        pt->v[i].pending = -1;
    }
}

static void gr_all_sound_off(uint32_t part)
{
    gr_part_t *pt = &gr_pt[part];
    uint32_t i;
    if (gr_ambient(part)) {
        amb_all_sound_off(part);
        return;
    }
    pt->sustain = 0;
    for (i = 0; i < NNOTE; i++) {
        gr_voice_t *v = &pt->v[i];
        if (v->active) {
            v->gate = 0;
            v->sustained = 0;
            v->stealing = 1;
        }
        v->pending = -1;
    }
}

/* -------------------------------------------------------- modulation --- */
/* a value x 1000 -> Q16 (1000 = 65536) */
static inline int32_t k2q16(int32_t v1000) { return (v1000 * 4295) >> 6; }

/* the 24 LFO shapes (the Ambient's list): k, the phase (32 bit) and the two random values of this cycle -> Q15 */
static int32_t gr_shape(uint32_t k, uint32_t ph, int32_t from, int32_t to)
{
    uint32_t x = ph >> 16, n;
    int32_t t = (int32_t)x;
    switch (k) {
    case 0: case 21: return sin32(ph);
    case 1: return x < 32768u ? 32767 : -32767;
    case 2: case 22: return x < 32768u ? (int32_t)(2u * x) - 32768 : 98304 - (int32_t)(2u * x);
    case 3: return t - 32768;
    case 4: return 32767 - t;
    case 5: return from;
    case 6: case 23: return from + (int32_t)(((int64_t)(to - from) * t) >> 16);
    case 7: return (int32_t)isqrt32(x << 16) - 32768;                         /* log: fast, then slow */
    case 8: return (int32_t)isqrt32((65535u - x) << 16) - 32768;
    case 9: return x < 6554u ? 32767 : -32767;
    case 10: return x < 16384u ? 32767 : -32767;
    case 11: return x < 49152u ? 32767 : -32767;
    case 12: return x < 58982u ? 32767 : -32767;
    case 19: return mulq16(mulq16(t, t), 196608 - 2 * t) - 32768;               /* ramp up: a soft rise */
    case 20: return 32767 - mulq16(mulq16(t, t), 196608 - 2 * t);
    default:                                                                  /* steps 2 .. 7 */
        n = k - 11u;
        return -32767 + (int32_t)((x * n >> 16) * 65534u / (n - 1u));
    }
}

static void gr_lfo_advance(gr_lfo_t *lf, uint32_t wave, int32_t rate256)
{
    uint32_t inc = (uint32_t)rate256 * 48694u;                 /* rate x 256 -> phase per block */
    uint32_t prev = lf->phase;
    if (wave >= 21u)
        inc >>= 3;                                              /* the slow shapes */
    lf->phase += inc;
    if (lf->phase < prev || !lf->started) {                  /* wrapped (or first): a new random target */
        lf->from = lf->started ? lf->to : gr_rand_bi();
        lf->to = gr_rand_bi();
        lf->started = 1;
    }
    lf->out = gr_shape(wave, lf->phase, lf->from, lf->to);
}

static void gr_collect(gr_part_t *pt, uint32_t part, const uint8_t *dest)
{
    uint32_t i;
    (void)part;
    for (i = 0; i < P_COUNT; i++)
        pt->off[i] = 0;
    for (i = 0; i < 2u; i++)
        if (dest[i] != P_COUNT)
            pt->off[dest[i]] += (pt->lfo[i].out * pt->lfo_depth[i]) / 500;         /* Q15 x (x1000) -> Q16 */
}

static inline int32_t gr_modv(const gr_pset_t *s, uint32_t p, int32_t off)
{
    if (!off)
        return s->real[p];
    return gr_norm_to_real(p, (uint32_t)clampi((int32_t)s->norm[p] + off, 0, 65535));
}

/* the value of a part's parameter this block: a lock takes the knob's place, the LFO moves what is there */
static inline int32_t gr_modv_l(const gr_part_t *pt, const gr_pset_t *s, uint32_t p)
{
    int32_t off = pt->off[p], lk = pt->lock[p];
    if (!off && lk < 0)
        return s->real[p];
    return gr_norm_to_real(p, (uint32_t)clampi((lk >= 0 ? lk : (int32_t)s->norm[p]) + off, 0, 65535));
}

static void gr_modulate(void)
{
    uint32_t part, p;
    for (part = 0; part < NPART; part++) {
        gr_part_t *pt = &gr_pt[part];
        const gr_pset_t *ps = &gr_ps[part];
        uint8_t dest[2];
        dest[0] = gr_dest_of(part)[gr_index(part, P_L1DEST)];
        dest[1] = gr_dest_of(part)[gr_index(part, P_L2DEST)];
        gr_collect(pt, part, dest);                  /* rates and depths use last block's outputs */
        {
            int32_t r1 = gr_modv(ps, P_L1RATE, pt->off[P_L1RATE]), r2 = gr_modv(ps, P_L2RATE, pt->off[P_L2RATE]);
            int32_t d1 = gr_modv(ps, P_L1DEPTH, pt->off[P_L1DEPTH]), d2 = gr_modv(ps, P_L2DEPTH, pt->off[P_L2DEPTH]);
            gr_lfo_advance(&pt->lfo[0], gr_index(part, P_L1WAVE), r1);
            gr_lfo_advance(&pt->lfo[1], gr_index(part, P_L2WAVE), r2);
            pt->lfo_depth[0] = d1;
            pt->lfo_depth[1] = d2;
        }
        gr_st.lfo[part][0] = (int16_t)((pt->lfo[0].out * pt->lfo_depth[0]) / 1000);
        gr_st.lfo[part][1] = (int16_t)((pt->lfo[1].out * pt->lfo_depth[1]) / 1000);
        gr_collect(pt, part, dest);
        for (p = 0; p < P_COUNT; p++) {
            if (gr_is_global(p))
                continue;
            pt->val[p] = gr_modv_l(pt, ps, p);
            pt->vnorm[p] = (uint16_t)clampi((pt->lock[p] >= 0 ? pt->lock[p] : (int32_t)ps->norm[p]) + pt->off[p], 0, 65535);
        }
    }
    for (p = 0; p < P_COUNT; p++) {                   /* shared: the LFOs of both parts add */
        int32_t off = 0;
        if (!gr_is_global(p))
            continue;
        for (part = 0; part < NPART; part++)
            off += gr_pt[part].off[p];
        for (part = 0; part < NPART; part++) {
            gr_pt[part].val[p] = gr_modv(&gr_ps[0], p, off);
            gr_pt[part].vnorm[p] = (uint16_t)clampi((int32_t)gr_ps[0].norm[p] + off, 0, 65535);
        }
    }
}

/* ------------------------------------------------------------ grains --- */
static void gr_release_grain(gr_grain_t *g)
{
    g->active = 0;
    gr_active_grains--;
    if (!g->shedding)
        gr_pt[g->part].running--;
}

/* ends a grain early without a click: jump to the mirrored window phase (same amplitude), then play its falling half */
static void gr_shed_grain(gr_grain_t *g)
{
    if (g->phase < 0x80000000u)
        g->phase = 0u - g->phase;
    if (g->remaining > 256u)
        g->remaining = 256u;
    g->phase_inc = (0xFFFFFFFFu - g->phase) / g->remaining;
    g->shedding = 1;
    gr_pt[g->part].running--;
}

static int gr_shed_one(uint32_t part)
{
    uint32_t i;
    for (i = 0; i < NGRAIN; i++)
        if (gr_gr[i].active && !gr_gr[i].shedding && gr_gr[i].part == part) {
            gr_shed_grain(&gr_gr[i]);
            return 1;
        }
    return 0;
}

/* Divides the grain cap by what each part needs: a part that needs less than its half gets all it needs and the
 * other can use the rest; if both need more they split it. */
static void gr_grain_limits(void)
{
    int32_t demand[NGPART], share = gr_cap / NGPART;
    uint32_t part;
    for (part = 0; part < NGPART; part++) {
        gr_part_t *pt = &gr_pt[part];
        int32_t overlap_q8 = (int32_t)((uint32_t)pt->val[P_DENSITY] * (uint32_t)pt->val[P_SIZE] / 16000u);
        demand[part] = pt->smp.frames && pt->notes ? (overlap_q8 * pt->notes * 5 / 4 >> 8) + 2 : 0;
    }
    for (part = 0; part < NGPART; part++) {
        int32_t other = demand[part ? 0 : 1];
        gr_pt[part].limit = (int16_t)(gr_cap - (other < share ? other : share));
    }
}

static void gr_spawn_grain(uint32_t part, uint32_t vi, const gr_voice_t *v, uint32_t offset)
{
    gr_part_t *pt = &gr_pt[part];
    gr_grain_t *g = 0;
    int32_t running = 0, semis, ratio, jit, overlap_q8, norm, amp, pan, angle;
    uint32_t i, where, start, length, frames = pt->smp.frames, ph;
    for (i = 0; i < NPART; i++)
        running += gr_pt[i].running;
    if (running < gr_cap && pt->running < pt->limit)
        for (i = 0; i < NGRAIN; i++)
            if (!gr_gr[i].active) {
                g = &gr_gr[i];
                break;
            }
    if (!g) {
        gr_st.skipped++;
        return;
    }
    /* where in the sample */
    jit = (int32_t)(((int64_t)k2q16(pt->val[P_SPREAD]) * gr_rand_bi()) >> 16);                /* spread x random, half each way */
    where = (uint32_t)((int32_t)pt->vnorm[P_POSITION] + (int32_t)(v->scan >> 16) + jit) & 0xFFFFu;
    start = (where * frames) >> 16;
    if (start >= frames)
        start = frames - 1u;
    /* pitch */
    semis = ((int32_t)v->note - ROOT_NOTE) * 256 + v->glide + (((pt->val[P_TRANSPOSE] + 128) >> 8) << 8) + pt->bend_q8;
    if (pt->val[P_PITCHRND] > 0)
        semis += (int32_t)(((int64_t)pt->val[P_PITCHRND] * gr_rand_bi()) >> 15);
    ratio = (int32_t)mulq16((int32_t)semis_ratio(semis), (int32_t)pt->rr_q16);
    ratio = clampi(ratio, 1024, 16 << 16);                              /* 1/64 .. 16 */
    /* size, loudness */
    length = ((uint32_t)pt->val[P_SIZE] * 44100u) / 16000u;
    if (length < 16u)
        length = 16u;
    overlap_q8 = (int32_t)((uint32_t)pt->val[P_DENSITY] * (uint32_t)pt->val[P_SIZE] / 16000u);
    norm = overlap_q8 > 256 ? (int32_t)(8388608u / isqrt32((uint32_t)overlap_q8 << 8)) : 32768;
    amp = (int32_t)(((uint32_t)v->vel * (uint32_t)norm / 127u) * 19661u >> 15);       /* x 0.6 */
    pan = (int32_t)(((int64_t)k2q16(pt->val[P_PAN]) * gr_rand_bi()) >> 15);                  /* +-65536 */
    angle = (pan + 65536) >> 1;                                                        /* 0 .. 65536 */
    ph = (uint32_t)angle << 14;                                                        /* a quarter turn at 65536 */
    g->pos = start << 16;
    g->inc = ((uint32_t)(gr_rand() >> 16) * 1000u >> 16) < (uint32_t)pt->val[P_REVERSE] ? -ratio : ratio;
    g->phase = 0;
    g->phase_inc = 0xFFFFFFFFu / length;
    g->remaining = length;
    g->amp_l = mulq15(amp, sin32(ph + 0x40000000u));                  /* cos */
    g->amp_r = mulq15(amp, sin32(ph));
    g->start = (uint16_t)offset;
    g->voice = (int8_t)vi;
    g->part = (uint8_t)part;
    g->active = 1;
    g->shedding = 0;
    gr_active_grains++;
    pt->running++;
}

static void gr_spawn(uint32_t part)
{
    gr_part_t *pt = &gr_pt[part];
    uint32_t vi;
    uint32_t dens = (uint32_t)pt->val[P_DENSITY] ? (uint32_t)pt->val[P_DENSITY] : 256u;
    uint32_t interval = 2890137600u / dens;                             /* frames between grains, Q8 */
    int32_t jit_q15 = (pt->val[P_JITTER] * 32768) / 1000;
    int32_t scan_step = pt->val[P_SCAN] * (int32_t)pt->scan_k;
    for (vi = 0; vi < NNOTE; vi++) {
        gr_voice_t *v = &pt->v[vi];
        int spawned = 0;
        if (!v->active || v->finished)
            continue;
        v->scan += (uint32_t)scan_step;
        if (v->stealing)
            continue;
        while (v->next < (int32_t)(BLK << 8) && spawned < NGRAIN) {
            int32_t f = 65536 + 2 * mulq15(mulq15(jit_q15, 29491), gr_rand_bi()), nx;
            gr_spawn_grain(part, vi, v, (uint32_t)(v->next < 0 ? 0 : v->next >> 8));
            nx = (int32_t)(((uint64_t)interval * (uint32_t)f) >> 16);
            if (nx < 256)
                nx = 256;
            v->next += nx;
            spawned++;
        }
        v->next -= BLK << 8;
    }
}

static void gr_render_grains(void)
{
    uint32_t gi;
    for (gi = 0; gi < NGRAIN; gi++) {
        gr_grain_t *g = &gr_gr[gi];
        gr_part_t *pt;
        const int16_t *d;
        const gr_voice_t *v;
        int32_t *bl, *br, gl, gr_, dl, dr;
        uint32_t pos, len32, phase, pinc, start, count, i;
        int32_t inc;
        if (!g->active)
            continue;
        pt = &gr_pt[g->part];
        d = pt->smp.data;
        bl = pt->bus_l;
        br = pt->bus_r;
        v = &pt->v[g->voice];
        start = g->start;
        g->start = 0;
        count = BLK - start;
        if (count > g->remaining)
            count = g->remaining;
        {   /* the voice's envelope ramps over the block: both channel gains step with it (Q23) */
            int32_t e0 = v->env_prev, e1 = v->env;
            int32_t l0 = mulq15(g->amp_l, e0), l1 = mulq15(g->amp_l, e1), r0 = mulq15(g->amp_r, e0), r1 = mulq15(g->amp_r, e1);
            dl = (l1 - l0) * 2;
            dr = (r1 - r0) * 2;
            gl = (l0 << 8) + dl * (int32_t)start;
            gr_ = (r0 << 8) + dr * (int32_t)start;
        }
        pos = g->pos;
        inc = g->inc;
        len32 = pt->smp.frames << 16;
        phase = g->phase;
        pinc = g->phase_inc;
        for (i = start; i < start + count; i++) {
            uint32_t idx = pos >> 16;
            int32_t s0 = d[idx], s1 = d[idx + 1u], s, t;
            s = s0 + (((s1 - s0) * (int32_t)((pos >> 1) & 0x7FFFu)) >> 15);
            t = (s * (int32_t)GR_HANN[phase >> 22]) >> 15;
            bl[i] += (t * (gl >> 8)) >> 15;
            br[i] += (t * (gr_ >> 8)) >> 15;
            gl += dl;
            gr_ += dr;
            phase += pinc;
            pos += (uint32_t)inc;
            if (pos >= len32)
                pos = inc < 0 ? pos + len32 : pos - len32;
        }
        g->pos = pos;
        g->phase = phase;
        g->remaining -= count;
        if (g->remaining == 0)
            gr_release_grain(g);
    }
}

/* ------------------------------------------------------------- parts --- */
static void gr_update_voices(uint32_t part)
{
    gr_part_t *pt = &gr_pt[part];
    int32_t a = BLK_ENV_K / (pt->val[P_ATTACK] ? pt->val[P_ATTACK] : 1), r = BLK_ENV_K / (pt->val[P_RELEASE] ? pt->val[P_RELEASE] : 1);
    uint32_t i;
    int32_t count = 0;
    if (a > 32768)
        a = 32768;
    if (r > 32768)
        r = 32768;
    for (i = 0; i < NNOTE; i++) {
        gr_voice_t *v = &pt->v[i];
        if (!v->active)
            continue;
        count++;
        v->env_prev = v->env;
        if (v->glide) {
            int32_t rem = absi(v->glide) - v->glide_step;
            v->glide = rem <= 0 ? 0 : (v->glide > 0 ? rem : -rem);
        }
        if (v->stealing)
            v->env -= 16384;                                              /* about 6 ms */
        else if (v->gate)
            v->env += a;
        else
            v->env -= r;
        if (v->env > 32768)
            v->env = 32768;
        if (v->env <= 0) {
            v->env = 0;
            if (!v->gate)
                v->finished = 1;
        }
    }
    pt->notes = (int16_t)count;
}

static void gr_retire_voices(uint32_t part)
{
    gr_part_t *pt = &gr_pt[part];
    uint32_t vi, gi;
    for (vi = 0; vi < NNOTE; vi++) {
        gr_voice_t *v = &pt->v[vi];
        if (!v->active || !v->finished)
            continue;
        for (gi = 0; gi < NGRAIN; gi++)
            if (gr_gr[gi].active && gr_gr[gi].part == part && gr_gr[gi].voice == (int8_t)vi)
                gr_release_grain(&gr_gr[gi]);
        if (v->pending >= 0)
            gr_voice_start(pt, v, (uint32_t)v->pending, v->pendvel);
        else
            v->active = v->finished = v->stealing = 0;
    }
}

static void gr_smooth(int32_t *cur, int32_t target)
{
    int32_t d = target - *cur;
    *cur = (d > -16 && d < 16) ? target : *cur + d / 10;
}

static void gr_mix_part(gr_part_t *pt, int32_t *outL, int32_t *outR)
{
    int32_t lvl = pt->level, target = (pt->val[P_LEVEL] * 32768) / 1000, step = (target - lvl) / BLK;
    uint32_t i;
    gr_smooth(&pt->dsend, (pt->val[P_DLYSEND] * 32768) / 1000);
    gr_smooth(&pt->rsend, (pt->val[P_REVSEND] * 32768) / 1000);
    for (i = 0; i < BLK; i++) {
        int32_t l, r;
        lvl += step;
        l = mulq15(pt->bus_l[i], lvl);
        r = mulq15(pt->bus_r[i], lvl);
        outL[i] += l;
        outR[i] += r;
        gr_send_dl[i] += mulq15(l, pt->dsend);
        gr_send_dr[i] += mulq15(r, pt->dsend);
        gr_send_rl[i] += mulq15(l, pt->rsend);
        gr_send_rr[i] += mulq15(r, pt->rsend);
    }
    pt->level = target;
}

/* a part with no notes and no output skips its filter and mix. A self-oscillating filter keeps it going. */
static void gr_update_idle(uint32_t part)
{
    gr_part_t *pt = &gr_pt[part];
    uint32_t i;
    int32_t peak = 0;
    if (gr_ambient(part)) {
        if (amb_voices(part)) {
            pt->quiet = 0;
            return;
        }
    } else {
        for (i = 0; i < NNOTE; i++)
            if (pt->v[i].active) {
                pt->quiet = 0;
                return;
            }
    }
    for (i = 0; i < BLK; i++) {
        int32_t a = absi(pt->bus_l[i]) + absi(pt->bus_r[i]);
        if (a > peak)
            peak = a;
    }
    if (peak > 2)
        pt->quiet = 0;
    else if (++pt->quiet >= 172)
        pt->idle = 1;
}

static inline int32_t gr_softclip(int32_t x)             /* Q15, +-1.5 reaches +-1 */
{
    int32_t x2, x3;
    if (x > 49152)
        return 32767;
    if (x < -49152)
        return -32767;
    x2 = mulq15(x, x);
    x3 = mulq15(x2, x);
    return x - mulq15(x3, 4854);
}

/* the CPU load of the last block, 0..255 = 0..100 %: keeps the render in real time by changing the grain cap */
static void gr_report_load(uint32_t q8)
{
    gr_st.load = (uint8_t)(q8 > 255u ? 255u : q8);
    if (q8 > 217u) {                                      /* above 85 % */
        gr_cap = gr_cap > 6 ? gr_cap - 2 : 4;
        gr_raise = 64;
    } else if (q8 < 179u && gr_cap < NGRAIN && --gr_raise <= 0) {     /* below 70 % */
        gr_cap++;
        gr_raise = 16;
    }
    gr_st.cap = (uint8_t)gr_cap;
    if (q8 > 217u) {                                      /* the drone and the pad give up oscillators too */
        amb_budget = amb_budget > 8u ? amb_budget - 2u : 6u;
        amb_raise = 64;
    } else if (q8 < 160u && amb_budget < AMB_BUDGET && --amb_raise == 0u) {
        amb_budget++;
        amb_raise = 16;
    }
}

static void gr_publish(void)
{
    uint32_t part, i;
    gr_st.grains = (uint8_t)gr_active_grains;
    for (part = 0; part < NPART; part++)
        gr_st.notes[part] = (uint8_t)gr_pt[part].notes;
    if (++gr_view_cnt < 8u)
        return;
    gr_view_cnt = 0;
    for (i = 0; i < NGRAIN; i++) {
        const gr_grain_t *g = &gr_gr[i];
        uint16_t val = GVIEW_NONE;
        if (g->active && gr_pt[g->part].smp.frames)
            val = (uint16_t)(((g->pos >> 16) * 65534u) / gr_pt[g->part].smp.frames);
        gr_st.gpart[i] = g->part;
        gr_st.gview[i] = val;
    }
}

/* One block: BLK frames into outL / outR (Q15, soft-limited at +-1) */
static void gr_render(int32_t *outL, int32_t *outR)
{
    uint32_t part, i;
    int32_t volume;
    gr_modulate();
    for (part = 0; part < NPART; part++) {
        gr_part_t *pt = &gr_pt[part];
        if (!gr_ambient(part))
            gr_update_voices(part);
        if (!pt->idle)
            for (i = 0; i < BLK; i++)
                pt->bus_l[i] = pt->bus_r[i] = 0;
    }
    for (part = NGPART; part < NPART; part++) {                          /* the drone and the pad */
        gr_part_t *pt = &gr_pt[part];
        pt->notes = (int16_t)amb_voices(part);
        if (!pt->idle)
            amb_render(part);
    }
    gr_grain_limits();
    for (part = 0; part < NPART; part++) {            /* the sheds that the limits ask for */
        gr_part_t *pt = &gr_pt[part];
        while (pt->running > pt->limit)
            if (!gr_shed_one(part))
                break;
    }
    for (part = 0; part < NPART; part++)
        if (gr_pt[part].smp_state == SMP_NORMAL && gr_pt[part].smp.frames)
            gr_spawn(part);
    gr_render_grains();
    for (i = 0; i < BLK; i++) {
        outL[i] = outR[i] = 0;
        gr_send_dl[i] = gr_send_dr[i] = gr_send_rl[i] = gr_send_rr[i] = 0;
    }
    for (part = 0; part < NPART; part++) {
        gr_part_t *pt = &gr_pt[part];
        if (pt->smp_state == SMP_REQ && !pt->idle) {                  /* a sample is being replaced: fade out and drop */
            int32_t gain = 32768;
            for (i = 0; i < BLK; i++) {
                gain -= 256;
                pt->bus_l[i] = mulq15(pt->bus_l[i], gain);
                pt->bus_r[i] = mulq15(pt->bus_r[i], gain);
            }
        }
        if (!gr_ambient(part))
            gr_retire_voices(part);
        if (pt->smp_state == SMP_REQ) {
            uint32_t k;
            for (k = 0; k < NGRAIN; k++)
                if (gr_gr[k].active && gr_gr[k].part == part)
                    gr_release_grain(&gr_gr[k]);
            pt->smp.frames = 0;
            pt->smp_state = SMP_SILENT;
        }
        if (!pt->idle) {
            gr_filter_run(&pt->flt, pt->bus_l, pt->bus_r, gr_index(part, P_FTYPE), pt->vnorm[P_CUTOFF],
                          pt->vnorm[P_RESO], pt->vnorm[P_DRIVE]);
            gr_mix_part(pt, outL, outR);
            gr_update_idle(part);
        }
    }
    gr_delay_run(gr_send_dl, gr_send_dr, outL, outR, BLK, gr_pt[0].val[P_DLYTIME], gr_pt[0].val[P_DLYFB], gr_pt[0].val[P_DLYDAMP]);
    gr_reverb_run(gr_send_rl, gr_send_rr, outL, outR, BLK, gr_pt[0].val[P_REVDECAY], gr_pt[0].val[P_REVDAMP]);
    volume = (gr_pt[0].val[P_VOLUME] * 32768) / 1000;
    {
        int32_t step = (volume - gr_vol_q15) / BLK, gain = gr_vol_q15;
        for (i = 0; i < BLK; i++) {
            gain += step;
            outL[i] = gr_softclip(mulq15(outL[i], gain));
            outR[i] = gr_softclip(mulq15(outR[i], gain));
        }
        gr_vol_q15 = volume;
    }
    gr_publish();
}

/* -------------------------------------------------------- main loop --- */
static void gr_engine_init(void)
{
    uint32_t part;
    gr_reverb_init();
    for (part = 0; part < NPART; part++) {
        uint32_t p;
        for (p = 0; p < P_COUNT; p++)
            gr_pt[part].lock[p] = -1;
        gr_pt[part].idle = 1;
        gr_pt[part].smp_state = SMP_NORMAL;
    }
    for (part = 0; part < NGRAIN; part++)
        gr_st.gview[part] = GVIEW_NONE;
}

/* The main loop replaces a part's sample: gr_sample_begin waits until the audio side has dropped the old one (the
 * part falls silent), the caller fills smp.data, gr_sample_end starts it. */
static void gr_sample_begin(uint32_t part)
{
    gr_part_t *pt = &gr_pt[part];
    uint32_t t0 = fm1_ms;
    pt->smp_state = SMP_REQ;
    while (pt->smp_state != SMP_SILENT && (uint32_t)(fm1_ms - t0) < 100u)
        fm1_wdt_feed();
    pt->smp.frames = 0;
    pt->smp_state = SMP_SILENT;
}

static void gr_sample_end(uint32_t part, uint32_t frames, uint32_t rate)
{
    gr_part_t *pt = &gr_pt[part];
    gr_sample_t *s = &pt->smp;
    uint32_t x, i;
    if (frames > SMP_FRAMES)
        frames = SMP_FRAMES;
    s->data[frames] = s->data[0];                                   /* the guard sample: a loop */
    for (x = 0; x < 240u; x++) {                                    /* the picture: the peak of ~16 samples a column */
        uint32_t a = x * frames / 240u, b = (x + 1u) * frames / 240u, step = (b - a) / 16u + 1u;
        int32_t peak = 0;
        for (i = a; i < b; i += step) {
            int32_t m = absi(s->data[i]);
            if (m > peak)
                peak = m;
        }
        s->peaks[x] = (uint8_t)(peak >> 7 > 255 ? 255 : peak >> 7);
    }
    s->rate = rate;
    s->frames = frames;
    gr_part_sample_set(part);
    pt->smp_state = SMP_NORMAL;
}

/* ------------------------------------------------ the sequencer's hooks --- */
/* (all called from the audio code, inside gr_render's caller) */
static void gr_seq_note_on(uint32_t part, uint32_t note, uint32_t vel, uint32_t frame_offset)
{
    gr_voice_t *v = gr_note_on(part, note, vel);
    if (v)
        v->next = (int32_t)((frame_offset < BLK ? frame_offset : BLK - 1u) << 8);       /* the first grain at the offset */
}

/* a slide: the voice of `from` takes the new note and glides there over `blocks` blocks; else a new note */
static void gr_seq_slide(uint32_t part, uint32_t from, uint32_t to, uint32_t vel, uint32_t blocks)
{
    gr_part_t *pt = &gr_pt[part];
    uint32_t i;
    if (gr_ambient(part)) {                                   /* the drone and the pad: the next note, the old one released */
        gr_note_on(part, to, vel);
        gr_note_off(part, from);
        return;
    }
    for (i = 0; i < NNOTE; i++) {
        gr_voice_t *v = &pt->v[i];
        if (v->active && v->gate && !v->stealing && v->note == from) {
            v->glide += ((int32_t)from - (int32_t)to) * 256;
            v->glide_step = (absi(v->glide) + (int32_t)blocks - 1) / (int32_t)(blocks ? blocks : 1u);
            if (v->glide_step < 1)
                v->glide_step = 1;
            v->note = (uint8_t)to;
            v->vel = (uint8_t)vel;
            return;
        }
    }
    gr_note_on(part, to, vel);
}

static void gr_seq_set_locks(uint32_t part, const int32_t *norms)           /* P_COUNT norms, -1 = none */
{
    uint32_t p;
    for (p = 0; p < P_COUNT; p++)
        gr_pt[part].lock[p] = norms ? norms[p] : -1;
}
