/* SPDX-License-Identifier: GPL-3.0-only
 * amb-iguos: the cinema generators. Like the others they render a mono sound into a part's sample memory at
 * 22.05 kHz, tuned to C4 where they have a pitch, and gg_finish normalises them.
 *
 *   Braam    a huge brass-like swell: detuned saws and a sub through a low-pass that opens, then saturates
 *   Swell    a reversed bell-and-noise bloom that rushes up to a sudden end (a reverse cymbal / reverse piano)
 *   Tensión  a cluster of close, slowly drifting string voices with tremolo
 *   Impacto  a trailer hit: a falling sub, a thump, a noise burst and a long dark tail
 *   Shepard  an endless rise (or fall): octave-spaced voices under a fixed bell-shaped window, a seamless loop */

/* a soft clip, Q15 in, about +-1 out */
static inline int32_t g3_soft(int32_t x)
{
    int32_t x2;
    x = clampi(x, -49152, 49152);
    x2 = mulq15(x, x);
    return x - mulq15(mulq15(x2, x), 10923);                         /* x - x^3 / 3 */
}

/* ------------------------------------------------------------- Braam --- */
/* v: Detune (cents), Swell (0..1000), Bright (0..1000), Drive (0..1000), Sub (0..1000), Variation */
static void gg_braam(const int32_t *v, int16_t *o, uint32_t n)
{
    uint32_t ph[5] = {0, 0, 0, 0, 0}, dt[5], sub = 0, i, k, rise = 1u + (uint32_t)(((uint64_t)n * (uint32_t)(50 + v[1] * 7 / 10)) / 1000u);
    int32_t drive = 32768 + v[3] * 98, subg = (v[4] * 32768) / 1000, env = 0;
    static const int8_t SPREAD[5] = {0, -2, 2, -1, 1};
    gg_svf_t f = {0, 0, 0, 0, 0};
    gg_rng_t rng;
    gg_seed(&rng, (uint32_t)v[5] + 7u);
    for (k = 0; k < 5u; k++)
        dt[k] = gg_dt(-12 * 256 + (SPREAD[k] * v[0] * 256) / 200 + (gg_bi(&rng) >> 12));      /* an octave below C4 */
    for (i = 0; i < n; i++) {
        int32_t s = 0, u;
        if ((i & 31u) == 0) {
            u = i < rise ? (int32_t)(((uint64_t)i << 15) / rise) : 32767;                     /* 0 .. 1 over the swell */
            env = mulq15(u, u);
            gg_svf_set(&f, 90u + (uint32_t)mulq15(mulq15(env, 200 + v[2] * 6), 32767) + (uint32_t)(v[2] * 2), 19661);
            if ((i & 4095u) == 0)
                GR_YIELD();
        }
        for (k = 0; k < 5u; k++) {
            ph[k] += dt[k];
            s += gg_osc(0, ph[k], dt[k]) / 5;
        }
        sub += dt[0] >> 1;
        s = gg_svf_run(&f, s) + mulq15(subg, sin32(sub));
        s = g3_soft(mulq15(mulq15(s, env), drive) >> 1);
        o[i] = (int16_t)sat16(s >> 1);
    }
}

/* ------------------------------------------------------------- Swell --- */
/* v: Color (Hz), Noise (0..1000), Curve (0..1000), Partials (2..12), Variation. A bell and noise decay, written
 * backwards: the sound rushes up to its strike and stops there. */
static void gg_swell(const int32_t *v, int16_t *o, uint32_t n)
{
    static const uint16_t RATIO[12] = {256, 512, 708, 768, 1024, 1229, 1382, 1536, 1792, 2048, 2406, 2867};   /* x256 */
    uint32_t parts = (uint32_t)v[3], ph[12], dt[12], i, k, tau = 1u + n / (2u + (uint32_t)v[2] / 150u);
    int32_t amp[12], nmix = (v[1] * 32768) / 1000, a = 32768 << 15, b = 0;
    uint32_t step = gg_decay(tau);
    gg_svf_t nf = {0, 0, 0, 0, 0};
    gg_rng_t rng;
    gg_seed(&rng, (uint32_t)v[4] + 11u);
    if (parts > 12u)
        parts = 12u;
    gg_svf_set(&nf, (uint32_t)v[0], 6554);
    for (k = 0; k < parts; k++) {
        ph[k] = gg_next(&rng);
        dt[k] = (uint32_t)(((uint64_t)GG_DT_ROOT * RATIO[k]) >> 8);
        amp[k] = 32768 / (int32_t)(k + 2u);
    }
    for (i = 0; i < n; i++) {
        int32_t s = 0, env;
        a = (int32_t)(((int64_t)a * step) >> 31);                     /* the strike's decay */
        env = a >> 15;
        for (k = 0; k < parts; k++) {
            ph[k] += dt[k];
            s += mulq15(amp[k], sin32(ph[k]));
        }
        b = gg_svf_run(&nf, gg_bi(&rng));
        s = mulq15(env, s + mulq15(nmix, b * 2));
        o[n - 1u - i] = (int16_t)sat16(s >> 1);                       /* backwards */
        if ((i & 4095u) == 0)
            GR_YIELD();
    }
}

/* ----------------------------------------------------------- Tensión --- */
/* v: Voices (2..6), Width (semitones x100), Tremolo (Hz x1000), Trem depth (0..1000), Bright (Hz), Variation */
static void gg_tension(const int32_t *v, int16_t *o, uint32_t n)
{
    uint32_t nv = (uint32_t)v[0], ph[6], dt[6], drift[6], dd[6], trem = 0, i, k;
    uint32_t tinc = (uint32_t)(((uint64_t)v[2] * GG_TURN_PER_HZ) / 1000u);
    int32_t tdepth = (v[3] * 32768) / 1000;
    gg_svf_t f = {0, 0, 0, 0, 0};
    gg_rng_t rng;
    gg_seed(&rng, (uint32_t)v[5] + 23u);
    if (nv > 6u)
        nv = 6u;
    gg_svf_set(&f, (uint32_t)v[4], 9830);
    for (k = 0; k < nv; k++) {
        int32_t at = nv > 1u ? (int32_t)k * v[1] * 256 / (100 * (int32_t)(nv - 1u)) : 0;        /* C4 .. C4 + width */
        ph[k] = gg_next(&rng);
        dt[k] = gg_dt(at - (v[1] * 128) / 100);                                                    /* centred on C4 */
        drift[k] = gg_next(&rng);
        dd[k] = (GG_TURN_PER_HZ / 8u) + (gg_u16(&rng) >> 2);                                       /* ~0.1 .. 0.3 Hz */
    }
    for (i = 0; i < n; i++) {
        int32_t s = 0, t;
        trem += tinc;
        for (k = 0; k < nv; k++) {
            uint32_t d;
            drift[k] += dd[k];
            d = dt[k] + (uint32_t)((int32_t)(dt[k] >> 9) * sin32(drift[k]) >> 15);               /* +-3 cents of drift */
            ph[k] += d;
            s += mulq15(gg_osc(0, ph[k], d), 32768 / (int32_t)nv);
        }
        t = 32768 - mulq15(tdepth, (sin32(trem) + 32768) >> 1);
        s = mulq15(gg_svf_run(&f, s), t);
        o[i] = (int16_t)sat16(s);
        if ((i & 4095u) == 0)
            GR_YIELD();
    }
}

/* ----------------------------------------------------------- Impacto --- */
/* v: Sub (0..1000), Body (Hz), Noise (0..1000), Tail (0..1000), Drive (0..1000), Variation */
static void gg_impact(const int32_t *v, int16_t *o, uint32_t n)
{
    uint32_t sub = 0, i, tail_tau = 1u + (uint32_t)(((uint64_t)n * (uint32_t)(80 + v[3])) / 3000u);
    uint32_t s_sub = gg_decay(GG_SR / 3u), s_drop = gg_decay(GG_SR / 14u), s_burst = gg_decay(GG_SR / 40u), s_tail = gg_decay(tail_tau);
    int32_t a_sub = 32767 << 15, a_drop = 32767 << 15, a_burst = 32767 << 15, a_tail = 32767 << 15;
    int32_t subg = (v[0] * 32768) / 1000, ng = (v[2] * 32768) / 1000, drive = 32768 + v[4] * 131;
    gg_svf_t body = {0, 0, 0, 0, 0}, dark = {0, 0, 0, 0, 0};
    gg_rng_t rng;
    gg_seed(&rng, (uint32_t)v[5] + 31u);
    gg_svf_set(&body, (uint32_t)v[1], 22938);
    gg_svf_set(&dark, 700u, 6554);
    for (i = 0; i < n; i++) {
        int32_t s, x, hz;
        a_sub = (int32_t)(((int64_t)a_sub * s_sub) >> 31);
        a_drop = (int32_t)(((int64_t)a_drop * s_drop) >> 31);
        a_burst = (int32_t)(((int64_t)a_burst * s_burst) >> 31);
        a_tail = (int32_t)(((int64_t)a_tail * s_tail) >> 31);
        hz = 38 + mulq15(a_drop >> 15, 110);                          /* the sub falls from ~150 Hz to 38 Hz */
        sub += (uint32_t)hz * GG_TURN_PER_HZ;
        x = gg_bi(&rng);
        s = mulq15(subg, mulq15(a_sub >> 15, sin32(sub)));
        s += mulq15(a_burst >> 15, gg_svf_run(&body, x));
        s += mulq15(ng, mulq15(a_burst >> 15, x) >> 1);
        s += mulq15(mulq15(ng, a_tail >> 15), gg_svf_run(&dark, x)) >> 1;
        s = g3_soft(mulq15(s, drive));
        o[i] = (int16_t)sat16(s >> 1);
        if ((i & 4095u) == 0)
            GR_YIELD();
    }
}

/* ----------------------------------------------------------- Shepard --- */
/* v: Direction (0 up, 1 down), Cycles (1..4 over the sound), Bright (0..1000), Voices (4..8), Variation */
static void gg_shepard(const int32_t *v, int16_t *o, uint32_t n)
{
    uint32_t nv = (uint32_t)v[3], ph[8], dt[8], i, k;
    int32_t amp[8], bright = (v[2] * 32768) / 1000;
    gg_rng_t rng;
    gg_seed(&rng, (uint32_t)v[4] + 5u);
    if (nv > 8u)
        nv = 8u;
    for (k = 0; k < nv; k++)
        ph[k] = gg_next(&rng);
    for (i = 0; i < n; i++) {
        int32_t s = 0;
        if ((i & 31u) == 0) {
            uint32_t p = (uint32_t)(((uint64_t)i * (uint32_t)v[1] << 16) / n) & 0xFFFFu;          /* the cycle, 0 .. 65535 */
            if (v[0])
                p = 65535u - p;
            for (k = 0; k < nv; k++) {
                uint32_t x = (p + (k << 16) / nv) & 0xFFFFu;                                       /* the voice's place */
                int32_t oct = (int32_t)((x * nv) - (nv << 15));                                     /* Q16 octaves around C4 */
                dt[k] = (uint32_t)(((uint64_t)GG_DT_ROOT * exp2s_q16(oct)) >> 16);
                if (dt[k] > 0x70000000u)
                    dt[k] = 0x70000000u;
                amp[k] = (32768 - sin32((x << 16) + 0x40000000u)) >> 1;                           /* a raised cosine */
            }
            if ((i & 4095u) == 0)
                GR_YIELD();
        }
        for (k = 0; k < nv; k++) {
            int32_t w;
            ph[k] += dt[k];
            w = sin32(ph[k]) + mulq15(bright, (gg_osc(2, ph[k], dt[k]) - sin32(ph[k])));
            s += mulq15(amp[k], w) / (int32_t)(nv / 2u);
        }
        o[i] = (int16_t)sat16(s >> 1);
    }
}
