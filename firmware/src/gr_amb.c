/* SPDX-License-Identifier: GPL-3.0-only
 * The Drone part (3) and the Pad part (4): a wavetable synth with the layout of the Drone and Pad layers of the
 * Sonicware LIVEN Ambient O, built from its user's manual (LVN-080-UM-03): 32 WAVEs of four HARMONIC tables each, a
 * HARMONIC position that morphs between them, a STRUCTURE that sets the oscillators and what the MOD LFO moves
 *   Drone 1: two oscillators, MOD moves the detune of the second      Drone 2: two oscillators, MOD moves HARMONIC
 *   Pad 1:   three oscillators, MOD moves HARMONIC                      Pad 2:   three oscillators, MOD moves HARMONIC
 * (a "Yang" oscillator's HARMONIC moves up with the MOD, a "Yin" one's down), PITCH and DETUNE of the second
 * oscillators, BALANCE, a MOD LFO of 24 shapes that restarts with each note, the voice modes Poly, Mono, Legato and
 * Unison (four detuned copies), an attack up to 10 s and a release up to 20 s, then the filter, two LFOs, level, pan
 * and the sends to the reverb and the delay like any part.
 * The 32 WAVEs here are not Sonicware's tables (those are not published): each is made from the manual's one-line
 * description of it, so it behaves in the same way (a wavefolded sine, a pulse whose width moves, an even-to-odd
 * morph, formants, noise ...), not note for note. Oscillators run at half the sample rate (22.05 kHz) and are
 * interpolated up: drones and pads are low and soft, and it halves the cost. */
#define AMB_N 256u                       /* samples of a cycle */
#define AMB_HALF (BLK / 2u)
#define DT_A4 85703200u                  /* 440 Hz at 22.05 kHz, as a 32-bit phase step */

typedef struct {
    uint8_t active, gate, held, note, vel, stealing;
    uint8_t pend, pend_note, pend_vel;   /* a note waiting for this fading voice to finish */
    int32_t env, env_prev;               /* Q24 */
    int32_t pitch_q8;                    /* the note in semitones x 256, gliding */
    uint32_t order, mod_ph;
    int32_t mod_from, mod_to;
    uint8_t mod_started;
    uint32_t ph[4][3];
    int32_t gl[4][3], gr[4][3];          /* the gains of the last block, Q15 */
} amb_voice_t;

typedef struct {
    amb_voice_t v[AMB_V];
    int16_t tab[2][4][AMB_N];            /* the wave: four tables; built into the idle copy, then switched to */
    volatile uint8_t cur, built;
    int32_t bend_q8;
    uint8_t sustain, stack[8], nstack;
    uint32_t order;
    int32_t prev_l, prev_r;              /* the last half-rate sample, for the interpolation */
} amb_part_t;
static amb_part_t amb[NPART - NGPART] GR_POOL;

static const char *const AMB_WAVE_NAMES[32] = {
    "SAW.S", "SQR.D", "SIN.S", "SIN.D", "SQR.S", "TRI.S", "SQ.SN", "EV.OD", "OCTV", "SIN.M", "SIN.F", "TRI.F", "SIN.C",
    "FM.CR", "FM.OD", "SYNC", "HRM.1", "HRM.2", "CL.FM", "FRML", "FRM.1", "FRM.2", "27.PS", "FM.WF", "FM.PF", "AD.MP",
    "BCHL", "SQR.R", "RNDM", "S.RND", "TRI.N", "WH.NS"};

/* ----------------------------------------------------------- the waves --- */
static inline int32_t aw_saw(uint32_t ph) { return (int32_t)(ph >> 16) - 32768; }
static inline int32_t aw_tri(uint32_t ph)
{
    int32_t t = (int32_t)(ph >> 15);
    return t < 65536 ? t - 32768 : 98303 - t;
}
static inline int32_t aw_sqr(uint32_t ph, uint32_t duty16) { return (ph >> 16) < duty16 ? 28000 : -28000; }
/* a triangle fold of x (Q15) times k (Q8) back into +-1 */
static int32_t aw_fold(int32_t x, int32_t k)
{
    int32_t y = (int32_t)(((int64_t)x * k) >> 8) + 32768;
    y &= 131071;
    if (y > 65535)
        y = 131071 - y;
    return y - 32768;
}
static inline int32_t aw_soft(int32_t x) { return gr_softclip(x); }
/* sine of ph plus an FM of `idx` turns (Q16) by a sine at `ratio` times the frequency */
static inline int32_t aw_fm(uint32_t ph, uint32_t ratio, int32_t idx16)
{
    int32_t s = sin32(ph * ratio);
    return sin32(ph + (uint32_t)((int64_t)s * idx16 * 2));
}
/* a sum of sines at harmonic h with amplitude a[h] (Q15), harmonics 1 .. n */
static int32_t aw_add(uint32_t ph, const int16_t *a, uint32_t n)
{
    int32_t sum = 0;
    uint32_t h;
    for (h = 1; h <= n; h++)
        if (a[h])
            sum += mulq15(a[h], sin32(ph * h));
    return sum;
}

/* One cycle of `wave` (1 .. 32) at morph m (0 .. 65536) into out[256], peak 24000. f is the table's number 0 .. 3,
 * which seeds the random waves. */
static void amb_cycle(uint32_t wave, int32_t m, uint32_t f, int16_t *out)
{
    static int16_t amp[40], noise[AMB_N];
    static int32_t buf[AMB_N];
    gg_rng_t rng;
    uint32_t i, h, w = wave;
    int32_t peak = 1, mean = 0, k, q;
    gg_seed(&rng, w * 7919u + f * 104729u + 17u);
    for (h = 0; h < 40u; h++)
        amp[h] = 0;
    for (i = 0; i < AMB_N; i++)
        noise[i] = (int16_t)(gg_bi(&rng));
    if (w == 28u || w == 29u || w == 30u || w == 31u || w == 32u) {      /* random waves: smoothed noise, different in each table */
        int32_t lp = 0, c = w == 29u ? 3 : w == 30u ? 3 : 8;
        for (i = 0; i < AMB_N * 2u; i++) {
            lp += (noise[i & 255u] - lp) * c / 16;
            if (i >= AMB_N)
                noise[i - AMB_N] = (int16_t)lp;                           /* two passes: the end joins the start */
        }
    }
    switch (w) {                                        /* the additive waves set their harmonics here */
    case 5:                                             /* a square that sharpens */
        for (h = 1; h <= 31u; h += 2u)
        {
            int32_t cut = 4 + (int32_t)((60 * (int64_t)m) >> 16), xr = (int32_t)((h << 16) / (uint32_t)cut), qq = mulq16(xr, xr);
            amp[h] = (int16_t)mulq16(32768 / (int32_t)h, (int32_t)exp2s_q16(-mulq16(qq, 94548)));
        }
        break;
    case 8:                                             /* even harmonics to odd */
        for (h = 1; h <= 32u; h++)
            amp[h] = (int16_t)(mulq16(32768 / (int32_t)h, (h & 1u) ? 13107 + mulq16(m, 52429) : 65536 - m));
        break;
    case 10:                                            /* a sine with overtones growing */
        amp[1] = 32767;
        for (h = 2; h <= 8u; h++)
            amp[h] = (int16_t)mulq16(24000 / (int32_t)h, m);
        break;
    case 17: case 18:                                   /* a saw's additive overtones */
        for (h = 1; h <= 12u; h++)
            amp[h] = (int16_t)(26000 / (int32_t)h);
        break;
    case 23:                                            /* one overtone out of 27 stepping up the series */
        amp[1] = 9000;
        q = 1 + (int32_t)(26 * (int64_t)m / 65536);
        for (h = 2; h <= 32u; h++) {
            int32_t d = (int32_t)h - q;
            amp[h] = (int16_t)(d == 0 ? 30000 : (d == 1 || d == -1) ? 9000 : 0);
        }
        break;
    case 26:                                            /* an additive tilt that moves */
        for (h = 1; h <= 20u; h++)
            amp[h] = (int16_t)(exp2s_q16(-mulq16(131072 - mulq16(m, 90000), (int32_t)GG_LOG2H[h])) >> 1);
        break;
    default: break;
    }
    for (i = 0; i < AMB_N; i++) {
        uint32_t ph = i << 24;
        int32_t x = 0, s = sin32(ph), g;
        switch (w) {
        case 1:  x = aw_soft(mulq16(aw_saw(ph), 65536 + 3 * m) * 1); break;                        /* a shaped saw */
        case 2:  x = aw_sqr(ph, 32768u - (uint32_t)mulq16(m, 29491)); break;                       /* PWM */
        case 3:  x = aw_soft(mulq16(s, 65536 + 2 * m)); break;                                    /* a shaped sine */
        case 4: {                                                                                /* a sine of a duty-warped phase */
            uint32_t d = 32768u - (uint32_t)mulq16(m, 26214), pp = ph >> 16, w2;
            w2 = pp < d ? (pp << 15) / d : 32768u + ((pp - d) << 15) / (65536u - d);
            x = sin32(w2 << 16);
            break;
        }
        case 5:  x = aw_add(ph, amp, 31); break;
        case 6:  x = aw_soft(mulq16(aw_tri(ph), 65536 + 3 * m)); break;
        case 7:  x = sin32(ph + (uint32_t)((int64_t)s * mulq16(m, 98304) * 2)); break;             /* sin of a sine: a square-ish */
        case 8: case 10: case 23: case 26: x = aw_add(ph, amp, 32); break;
        case 9: {                                                                                /* octaves of saw */
            int32_t wt = 32768, t = 0;
            for (k = 0; k < 4; k++) {
                t += mulq15(aw_saw(ph << k), wt);
                wt = mulq16(wt, m) ;
            }
            x = t / 2 + aw_saw(ph) / 2;
            break;
        }
        case 11: x = aw_fold(s, 256 + mulq16(m, 1280)); break;
        case 12: x = aw_fold(aw_tri(ph), 256 + mulq16(m, 1280)); break;
        case 13: {                                                                               /* a crushed sine */
            int32_t sq = (int32_t)(((int64_t)(65536 - m) * (65536 - m)) >> 16), lv = 4 + (int32_t)(((int64_t)sq * 252) >> 16);
            x = ((s * lv) / 32768) * 32768 / lv;
            break;
        }
        case 14: g = aw_fm(ph, 1, mulq16(m, 70000)); x = (g * 12 / 32768) * 32768 / 12 + g / 8; break;
        case 15: x = aw_soft(mulq16(aw_fm(ph, 2, 26000), 65536 + 6 * m)); break;
        case 16: x = aw_saw((uint32_t)(((uint64_t)ph * (65536u + (uint32_t)(6 * (int64_t)m))) >> 16)); break;     /* hard sync */
        case 17: x = aw_add(ph, amp, 12) / 3 + aw_saw((uint32_t)(((uint64_t)ph * (65536u + (uint32_t)(3 * (int64_t)m))) >> 16)) / 2; break;
        case 18: x = aw_add(ph, amp, 12) / 3 + aw_fm(ph, 3, mulq16(m, 20000)) / 2; break;
        case 19: x = aw_soft(mulq16(aw_fm(ph, 2, mulq16(m, 32768)), 98304 + 3 * m)); break;
        case 20: x = aw_soft(aw_fold(s, 256 + mulq16(m, 768)) * 3 / 2); break;
        case 21: {                                                                               /* sine + FM + a formant window */
            uint32_t kk = 2u + (uint32_t)(10 * (int64_t)m / 65536);
            int32_t win = (32768 - sin32(ph + 0x40000000u)) / 2;
            x = mulq15(sin32(ph * kk + (uint32_t)((int64_t)s * 13000)), win) + s / 4;
            break;
        }
        case 22: {                                                                               /* two formants: a vowel */
            uint32_t k1 = 2u + (uint32_t)(4 * (int64_t)m / 65536), k2 = 6u + (uint32_t)(10 * (int64_t)m / 65536);
            int32_t win = (32768 - sin32(ph + 0x40000000u)) / 2;
            x = mulq15(sin32(ph * k1) + (sin32(ph * k2 + (uint32_t)((int64_t)s * 6000)) >> 1) * 6 / 10, win) + s / 5;
            break;
        }
        case 24: x = aw_fold(aw_tri(ph + (uint32_t)((int64_t)sin32(ph * 2u) * 5000 * 2)), 256 + mulq16(m, 768)); break;
        case 25: x = aw_fm(ph, 1u + (uint32_t)(3 * (int64_t)m / 65536), 6500 + mulq16(m, 72000)); break;
        case 27: x = aw_fold(s + mulq16(sin32(ph * 3u + (uint32_t)((int64_t)s * 9000)), mulq16(m, 23000)), 256 + mulq16(m, 512)); break;
        case 28: x = mulq15(aw_sqr(ph, 32768u), 32768 - mulq16(m, 32768) + mulq16(m, noise[i] / 2 + 16384)); break;
        case 29: case 30: x = noise[i]; break;
        case 31: x = aw_tri(ph) / 2 + mulq16(m, noise[i] / 2); break;
        default: x = noise[i]; break;                                                            /* 32: white noise */
        }
        buf[i] = x;
        mean += x;
    }
    mean /= (int32_t)AMB_N;
    {   /* the waves are levelled by their RMS (about 9000) so that they sound equally loud, the peak kept under 30000 */
        uint64_t acc = 0;
        uint32_t rms;
        int32_t scale_q12, lim_q12;
        for (i = 0; i < AMB_N; i++) {
            buf[i] -= mean;
            if (absi(buf[i]) > peak)
                peak = absi(buf[i]);
            acc += (uint64_t)((int64_t)buf[i] * buf[i]);
        }
        rms = isqrt32((uint32_t)(acc / AMB_N >> 2)) * 2u + 1u;                    /* sqrt of the mean square */
        scale_q12 = (int32_t)((9000u << 12) / rms);
        lim_q12 = (int32_t)((30000u << 12) / (uint32_t)peak);
        if (scale_q12 > lim_q12)
            scale_q12 = lim_q12;
        for (i = 0; i < AMB_N; i++)
            out[i] = (int16_t)clampi((int32_t)(((int64_t)buf[i] * scale_q12) >> 12), -32767, 32767);
    }
}

static void amb_build(uint32_t part, uint32_t wave, int16_t tab[4][AMB_N])
{
    uint32_t f;
    (void)part;
    for (f = 0; f < 4u; f++) {
        amb_cycle(wave, (int32_t)(f * 65536u / 3u), f, tab[f]);
        GR_YIELD();
    }
}

/* main loop: remake a part's tables when its WAVE has changed (the audio code reads the other copy meanwhile) */
static void amb_poll(void)
{
    uint32_t part;
    for (part = NGPART; part < NPART; part++) {
        amb_part_t *a = &amb[part - NGPART];
        uint32_t wave = gr_index(part, P_AWAVE) + 1u;
        if (a->built != wave) {
            uint8_t other = (uint8_t)(a->cur ^ 1u);
            amb_build(part, wave, a->tab[other]);
            a->cur = other;
            a->built = (uint8_t)wave;
        }
    }
}

/* ------------------------------------------------------------ the notes --- */
static uint32_t amb_cost(uint32_t part)
{
    uint32_t nosc = gr_kind[part] == KIND_DRONE ? 2u : 3u;
    return nosc * (gr_index(part, P_AVOICE) == 3u ? 4u : 1u);
}
static uint32_t amb_used(const amb_part_t *a, uint32_t cost)
{
    uint32_t i, n = 0;
    for (i = 0; i < AMB_V; i++)
        n += (a->v[i].active && !a->v[i].stealing) || a->v[i].pend ? cost : 0u;
    return n;
}

static void amb_start(amb_part_t *a, amb_voice_t *v, uint32_t note, uint32_t vel, int keep_env)
{
    uint32_t u, o;
    v->active = 1;
    v->gate = 1;
    v->held = v->stealing = v->pend = 0;
    v->note = (uint8_t)note;
    v->vel = (uint8_t)vel;
    if (!keep_env) {
        v->env_prev = v->env;
        v->env = 0;
        v->mod_ph = 0;
        v->mod_started = 0;
    }
    v->pitch_q8 = (int32_t)note * 256;
    v->order = ++a->order;
    if (!keep_env)
        for (u = 0; u < 4u; u++)
            for (o = 0; o < 3u; o++) {
                v->ph[u][o] = (uint32_t)(gr_rand() & 0xFFFFu) << 16;             /* the copies start out of step */
                v->gl[u][o] = v->gr[u][o] = 0;
            }
}

/* the one voice of mono and legato takes a new note: not by cutting it (a click) but by fading it out for a few ms and
 * letting the new note start on it */
static void amb_mono_take(amb_part_t *a, amb_voice_t *v, uint32_t note, uint32_t vel)
{
    if (!v->active) {
        amb_start(a, v, note, vel, 0);
        return;
    }
    v->gate = 0;
    v->held = 0;
    v->stealing = 1;
    v->pend = 1;
    v->pend_note = (uint8_t)note;
    v->pend_vel = (uint8_t)vel;
}

static void amb_note_on(uint32_t part, uint32_t note, uint32_t vel)
{
    amb_part_t *a = &amb[part - NGPART];
    uint32_t mode = gr_index(part, P_AVOICE), cost = amb_cost(part), i;
    amb_voice_t *free_v = 0, *victim = 0;
    if (a->nstack < 8u)
        a->stack[a->nstack++] = (uint8_t)note;
    if (mode == 1u || mode == 2u) {                                      /* mono, legato: one voice follows the newest note */
        amb_voice_t *v = &a->v[0];
        if (v->active && v->gate && !v->stealing && mode == 2u) {        /* legato: the same voice, the pitch glides to the note */
            v->note = (uint8_t)note;
            v->vel = (uint8_t)vel;
            return;
        }
        if (v->active && v->stealing && v->pend) {                       /* already handing over: the newest note wins */
            v->pend_note = (uint8_t)note;
            v->pend_vel = (uint8_t)vel;
            return;
        }
        if (v->active && v->gate && !v->stealing && v->note == note)
            return;
        amb_mono_take(a, v, note, vel);
        return;
    }
    for (i = 0; i < AMB_V; i++) {
        amb_voice_t *v = &a->v[i];
        if (v->active && v->gate && !v->stealing && v->note == note) {
            v->vel = (uint8_t)vel;
            return;
        }
    }
    for (i = 0; i < AMB_V; i++)
        if (!a->v[i].active) {
            free_v = &a->v[i];
            break;
        }
    while (!free_v || amb_used(a, cost) + cost > amb_budget) {            /* make room: the oldest, a released one first */
        victim = 0;
        for (i = 0; i < AMB_V; i++) {
            amb_voice_t *v = &a->v[i];
            if (!v->active || v->stealing)
                continue;
            if (!victim || (victim->gate && !v->gate) || (victim->gate == v->gate && (int32_t)(v->order - victim->order) < 0))
                victim = v;
        }
        if (!victim)
            break;
        victim->gate = 0;
        victim->stealing = 1;
        if (!free_v) {
            for (i = 0; i < AMB_V; i++)
                if (!a->v[i].active) {
                    free_v = &a->v[i];
                    break;
                }
            if (!free_v)
                free_v = victim;                                        /* it fades in a few ms, then this one is free */
        }
        if (amb_used(a, cost) + cost <= amb_budget)
            break;
    }
    if (free_v && !free_v->stealing) {
        amb_start(a, free_v, note, vel, 0);
    } else {                                                             /* every voice is busy: the note waits for the one fading out */
        amb_voice_t *w = free_v && !free_v->pend ? free_v : 0;
        for (i = 0; !w && i < AMB_V; i++)
            if (a->v[i].active && a->v[i].stealing && !a->v[i].pend)
                w = &a->v[i];
        if (w) {
            w->pend = 1;
            w->pend_note = (uint8_t)note;
            w->pend_vel = (uint8_t)vel;
        }
    }
}

static void amb_release(amb_part_t *a, amb_voice_t *v)
{
    if (a->sustain)
        v->held = 1;
    else
        v->gate = 0;
}

static void amb_note_off(uint32_t part, uint32_t note)
{
    amb_part_t *a = &amb[part - NGPART];
    uint32_t mode = gr_index(part, P_AVOICE), i, j;
    for (i = 0, j = 0; i < a->nstack; i++)
        if (a->stack[i] != note)
            a->stack[j++] = a->stack[i];
    a->nstack = (uint8_t)j;
    if (mode == 1u || mode == 2u) {
        amb_voice_t *v = &a->v[0];
        if (v->pend) {                                                    /* a note is waiting for the fade: follow the keys */
            if (a->nstack)
                v->pend_note = a->stack[a->nstack - 1u];
            else
                v->pend = 0;
            return;
        }
        if (!v->active || !v->gate)
            return;
        if (a->nstack) {                                                  /* back to the note still held */
            if (v->note == note) {
                if (mode == 1u)
                    amb_mono_take(a, v, a->stack[a->nstack - 1u], v->vel);
                else
                    v->note = a->stack[a->nstack - 1u];
            }
        } else {
            amb_release(a, v);
        }
        return;
    }
    for (i = 0; i < AMB_V; i++) {
        amb_voice_t *v = &a->v[i];
        if (v->pend && v->pend_note == note)
            v->pend = 0;                                              /* let go before it started */
        if (v->active && v->gate && !v->held && v->note == note)
            amb_release(a, v);
    }
}

static void amb_sustain(uint32_t part, uint32_t on)
{
    amb_part_t *a = &amb[part - NGPART];
    uint32_t i;
    a->sustain = (uint8_t)on;
    if (on)
        return;
    for (i = 0; i < AMB_V; i++)
        if (a->v[i].held) {
            a->v[i].held = 0;
            a->v[i].gate = 0;
        }
}
static void amb_bend(uint32_t part, int32_t bend) { amb[part - NGPART].bend_q8 = (bend * 2 * 256) / 8192; }
static void amb_all_notes_off(uint32_t part)
{
    amb_part_t *a = &amb[part - NGPART];
    uint32_t i;
    a->sustain = 0;
    a->nstack = 0;
    for (i = 0; i < AMB_V; i++) {
        a->v[i].gate = 0;
        a->v[i].held = 0;
        a->v[i].pend = 0;
    }
}
static void amb_all_sound_off(uint32_t part)
{
    amb_part_t *a = &amb[part - NGPART];
    uint32_t i;
    amb_all_notes_off(part);
    for (i = 0; i < AMB_V; i++)
        if (a->v[i].active)
            a->v[i].stealing = 1;
}
static uint32_t amb_voices(uint32_t part)
{
    uint32_t i, n = 0;
    for (i = 0; i < AMB_V; i++)
        n += amb[part - NGPART].v[i].active;
    return n;
}

/* ---------------------------------------------------------- the render --- */
/* the oscillators of a structure: the sign of the HARMONIC modulation (+1 Yang, -1 Yin, 0 none), the pan (-64 .. 64),
 * whether PITCH and DETUNE apply, and the balance group the oscillator belongs to */
typedef struct { int8_t sign, pan, pitched, detuned, group; } amb_osc_t;
static const amb_osc_t AMB_STR[4][3] = {
    {{0, -22, 0, 0, 0}, {0, 22, 1, 1, 1}, {0, 0, 0, 0, 0}},                          /* Drone 1: MOD moves the detune */
    {{1, -22, 0, 0, 0}, {-1, 22, 1, 1, 1}, {0, 0, 0, 0, 0}},                         /* Drone 2 */
    {{1, 0, 0, 0, 0}, {-1, -36, 1, 0, 1}, {-1, 36, 0, 1, 1}},                        /* Pad 1: one Yang, two Yin */
    {{1, -24, 0, 0, 0}, {-1, 24, 0, 1, 0}, {1, 0, 1, 0, 1}},                         /* Pad 2: two Yang, one Yin */
};

static inline int32_t amb_rd(const int16_t (*tab)[AMB_N], uint32_t ph, uint32_t f, uint32_t fr)
{
    uint32_t i = ph >> 24, j = (i + 1u) & (AMB_N - 1u);
    int32_t w = (int32_t)((ph >> 9) & 0x7FFFu), a0, a1;
    a0 = tab[f][i] + (((tab[f][j] - tab[f][i]) * w) >> 15);
    a1 = tab[f + 1u][i] + (((tab[f + 1u][j] - tab[f + 1u][i]) * w) >> 15);
    return a0 + (int32_t)(((int64_t)(a1 - a0) * (int32_t)fr) >> 16);
}

static void amb_render(uint32_t part)
{
    amb_part_t *a = &amb[part - NGPART];
    gr_part_t *pt = &gr_pt[part];
    const int16_t (*tab)[AMB_N] = a->tab[a->cur];
    static int32_t acc_l[AMB_HALF], acc_r[AMB_HALF];
    uint32_t vi, i, mode = gr_index(part, P_AVOICE), st = gr_kind[part] == KIND_DRONE ? gr_index(part, P_ASTRUCT) : 2u + gr_index(part, P_ASTRUCT);
    uint32_t nu = mode == 3u ? 4u : 1u, nosc = gr_kind[part] == KIND_DRONE ? 2u : 3u, shape = gr_index(part, P_AMODSHAPE);
    int32_t base_h = (int32_t)pt->vnorm[P_AHARM] * 3;                      /* 0 .. 3 tables, Q16 */
    int32_t pitch_off = (pt->val[P_APITCH] * 256) / 10, det = (pt->val[P_ADETUNE] * 256) / 100;
    int32_t bal = (pt->val[P_ABAL] * 32768) / 63, depth = (pt->val[P_AMODDEPTH] * 32768) / 1000, uni_det = (pt->val[P_AUNIDET] * 256) / 100;
    int32_t transpose = ((pt->val[P_TRANSPOSE] + 128) >> 8) << 8, ga, gb, mrate = pt->val[P_AMODRATE];
    uint32_t atk = (uint32_t)pt->val[P_AATK], rel = (uint32_t)pt->val[P_AREL], a_step, r_step, glide_ms = (uint32_t)pt->val[P_AGLIDE];
    uint32_t mod_inc = (uint32_t)mrate * 12466u;
    int32_t pangain = 0, pan_l, pan_r;
    (void)pangain;
    if (shape >= 21u)
        mod_inc >>= 3;                                                      /* the slow shapes */
    {   /* the envelope's step a block, Q24: 2^24 over the attack's blocks (ms x 16 -> blocks x 256) */
        uint32_t ab = atk * 551u / 100u, rb = rel * 551u / 100u;
        a_step = 0xFFFFFFFFu / (ab < 768u ? 768u : ab);                    /* never under 3 blocks (9 ms): a steeper ramp is a click */
        r_step = 0xFFFFFFFFu / (rb < 768u ? 768u : rb);
    }
    {   /* equal-power balance */
        uint32_t phb = (uint32_t)(bal + 32768) << 14;                       /* 0 .. a quarter turn */
        ga = sin32(phb + 0x40000000u);
        gb = sin32(phb);
    }
    {   /* the part's pan (Q15 gains) */
        uint32_t php = (uint32_t)clampi((pt->val[P_APAN] * 65536) / 1000, 0, 65536) << 14;
        pan_l = sin32(php + 0x40000000u);
        pan_r = sin32(php);
    }
    for (i = 0; i < AMB_HALF; i++)
        acc_l[i] = acc_r[i] = 0;
    for (vi = 0; vi < AMB_V; vi++) {
        amb_voice_t *v = &a->v[vi];
        int32_t modv, vamp, target_pitch;
        uint32_t u, o;
        int ending = 0, restart = 0;          /* the envelope reached zero in this block: it ramps down in it, the voice goes after */
        if (!v->active)
            continue;
        v->env_prev = v->env;
        if (v->stealing)
            v->env -= 1 << 21;                                             /* about 20 ms */
        else if (v->gate || v->held)
            v->env += (int32_t)(a_step >> 0);
        else
            v->env -= (int32_t)r_step;
        if (v->env > (1 << 24))
            v->env = 1 << 24;
        if (v->env <= 0) {
            v->env = 0;
            if (v->stealing && v->pend)
                restart = 1;                                               /* faded out: the waiting note takes the voice */
            else if (!v->gate && !v->held)
                ending = 1;
            if ((restart || ending) && v->env_prev <= 0) {                /* already silent: no block to ramp down */
                if (restart)
                    amb_start(a, v, v->pend_note, v->pend_vel, 0);
                else {
                    v->active = 0;
                    v->stealing = 0;
                }
                continue;
            }
        }
        target_pitch = (int32_t)v->note * 256;
        if (mode == 1u || mode == 2u) {                                    /* glide */
            int32_t d = target_pitch - v->pitch_q8;
            if (!glide_ms || absi(d) < 4)
                v->pitch_q8 = target_pitch;
            else {
                int32_t step = (int32_t)(((uint32_t)absi(d) * 2902u) / glide_ms) + 1;
                v->pitch_q8 += d > 0 ? (step < d ? step : d) : (step < -d ? -step : d);
            }
        } else {
            v->pitch_q8 = target_pitch;
        }
        {   /* the MOD LFO (restarts at each note) */
            uint32_t prev = v->mod_ph;
            v->mod_ph += mod_inc;
            if (v->mod_ph < prev || !v->mod_started) {
                v->mod_from = v->mod_started ? v->mod_to : gr_rand_bi();
                v->mod_to = gr_rand_bi();
                v->mod_started = 1;
            }
            modv = gr_shape(shape, v->mod_ph, v->mod_from, v->mod_to);
        }
        vamp = (int32_t)(((int64_t)(v->env >> 9) * v->vel * (gr_kind[part] == KIND_DRONE ? 22000 : 26000)) / (127 * 32768));     /* Q15 voice level */
        for (u = 0; u < nu; u++)
            for (o = 0; o < nosc; o++) {
                const amb_osc_t *oc = &AMB_STR[st][o];
                int32_t semis = v->pitch_q8 - 69 * 256 + transpose + a->bend_q8, hp, gl, gr_, dgl, dgr, gain, pan, s;
                uint32_t dt, f, fr, ph = v->ph[u][o], k;
                if (oc->pitched)
                    semis += pitch_off;
                if (oc->detuned)
                    semis += det + (st == 0u ? (mulq15(modv, depth) * 128) >> 15 : 0);         /* Drone 1: MOD moves the detune */
                if (nu > 1u)
                    semis += (((int32_t)u * 2 - 3) * uni_det) / 3;
                dt = (uint32_t)(((uint64_t)DT_A4 * semis_ratio(semis)) >> 16);
                hp = base_h + (oc->sign && st != 0u ? oc->sign * mulq15(modv, depth) * 3 : 0);
                hp = clampi(hp, 0, 3 * 65536 - 1);
                f = (uint32_t)hp >> 16;
                fr = (uint32_t)hp & 0xFFFFu;
                gain = mulq15(vamp, oc->group ? gb : ga);
                if (nu > 1u)
                    gain >>= 1;                                                   /* four copies: half each */
                if (gr_kind[part] == KIND_PAD && st == 3u)                       /* Pad 2: two oscillators share a group */
                    gain = mulq15(gain, oc->group ? 32768 : 23170);
                if (gr_kind[part] == KIND_PAD && st == 2u)
                    gain = mulq15(gain, oc->group ? 23170 : 32768);
                pan = oc->pan * 512 + (nu > 1u ? ((int32_t)u * 2 - 3) * 6000 / 3 : 0);       /* Q15 -1 .. 1 */
                pan = clampi(pan, -32768, 32768);
                {
                    uint32_t pp = (uint32_t)(pan + 32768) << 14;                     /* a quarter turn over -1 .. 1 */
                    gl = mulq15(gain, sin32(pp + 0x40000000u));
                    gr_ = mulq15(gain, sin32(pp));
                }
                gl = mulq15(gl, pan_l);
                gr_ = mulq15(gr_, pan_r);
                gl = mulq15(gl, 46341);                                            /* the two channels' equal-power law centres at .707 */
                gr_ = mulq15(gr_, 46341);
                dgl = ((gl - v->gl[u][o]) << 8) / (int32_t)AMB_HALF;
                dgr = ((gr_ - v->gr[u][o]) << 8) / (int32_t)AMB_HALF;
                {
                    int32_t cl = v->gl[u][o] << 8, cr = v->gr[u][o] << 8;
                    for (k = 0; k < AMB_HALF; k++) {
                        s = amb_rd(tab, ph, f, fr);
                        acc_l[k] += (s * (cl >> 8)) >> 15;
                        acc_r[k] += (s * (cr >> 8)) >> 15;
                        cl += dgl;
                        cr += dgr;
                        ph += dt;
                    }
                }
                v->ph[u][o] = ph;
                v->gl[u][o] = gl;
                v->gr[u][o] = gr_;
            }
        if (restart)
            amb_start(a, v, v->pend_note, v->pend_vel, 0);
        else if (ending) {
            v->active = 0;
            v->stealing = 0;
        }
    }
    for (i = 0; i < AMB_HALF; i++) {                                        /* up to the full rate: a straight line between */
        pt->bus_l[2u * i] += (a->prev_l + acc_l[i]) >> 1;
        pt->bus_r[2u * i] += (a->prev_r + acc_r[i]) >> 1;
        pt->bus_l[2u * i + 1u] += acc_l[i];
        pt->bus_r[2u * i + 1u] += acc_r[i];
        a->prev_l = acc_l[i];
        a->prev_r = acc_r[i];
    }
}
