/* SPDX-License-Identifier: GPL-3.0-only
 * Sample generators: VA, Vowel, Pluck, FM, Additive, Texture and FX, each rendering a short mono sound tuned to C4
 * (the root note) into a part's sample memory at 22.05 kHz. A port of the Granular synth's Generator.h to fixed
 * point; the sounds are the same in character, not bit for bit. The result is normalised at the end, so the
 * algorithms render with headroom (an int16 buffer, saturating sums). */
#ifndef GR_YIELD
#define GR_YIELD() ((void)0)
#endif

#define GG_SR GEN_RATE
#define GG_DT_ROOT 50960245u                     /* 261.6256 Hz at 22.05 kHz, as a 32-bit phase step */
#define GG_TURN_PER_HZ 194780u                   /* 2^32 / 22050 */

typedef struct { uint32_t s; } gg_rng_t;
static void gg_seed(gg_rng_t *r, uint32_t seed)
{
    r->s = seed * 2654435761u + 0x9E3779B9u;
    if (!r->s)
        r->s = 1;
}
static inline uint32_t gg_next(gg_rng_t *r)
{
    r->s ^= r->s << 13;
    r->s ^= r->s >> 17;
    r->s ^= r->s << 5;
    return r->s;
}
static inline int32_t gg_bi(gg_rng_t *r) { return (int32_t)(gg_next(r) >> 16) - 32768; }     /* Q15, -1 .. 1 */
static inline uint32_t gg_u16(gg_rng_t *r) { return gg_next(r) >> 16; }                      /* 0 .. 65535 */

static inline void gg_add(int16_t *o, int32_t v) { *o = (int16_t)sat16(*o + v); }
static inline uint32_t gg_dt(int32_t semis_q8) { return (uint32_t)(((uint64_t)GG_DT_ROOT * semis_ratio(semis_q8)) >> 16); }

/* polyBLEP residual (Q15) around a wrap of a phase accumulator */
static int32_t gg_blep(uint32_t ph, uint32_t inc)
{
    uint32_t d = inc >> 15;
    int32_t x;
    if (!d)
        return 0;
    if (ph < inc) {
        x = (int32_t)(ph / d);
        return x + x - ((x * x) >> 15) - 32768;
    }
    if (ph > 0xFFFFFFFFu - inc) {
        x = -(int32_t)((0xFFFFFFFFu - ph) / d);
        return ((x * x) >> 15) + x + x + 32768;
    }
    return 0;
}

static int32_t gg_osc(uint32_t wave, uint32_t ph, uint32_t dt)           /* Saw, Square, Triangle, Sine: Q15 */
{
    switch (wave) {
    case 0: return ((int32_t)(ph >> 16) - 32768) - gg_blep(ph, dt);
    case 1: return (ph < 0x80000000u ? 32767 : -32767) + gg_blep(ph, dt) - gg_blep(ph + 0x80000000u, dt);
    case 2: {
        int32_t t = (int32_t)(ph >> 15);                                 /* 0 .. 131071 */
        return t < 65536 ? t - 32768 : 98303 - t;
    }
    default: return sin32(ph);
    }
}

/* a low-pass state variable filter (Simper); coefficients Q16, states Q23 */
typedef struct { int32_t ic1, ic2, a1, a2, a3; } gg_svf_t;
static void gg_svf_set(gg_svf_t *f, uint32_t hz, int32_t res_q16)
{
    uint32_t ph, s, c;
    int32_t g, k;
    if (hz > 9900u)
        hz = 9900u;
    if (hz < 20u)
        hz = 20u;
    ph = hz * 97391u;
    s = (uint32_t)sin32(ph);
    c = (uint32_t)sin32(ph + 0x40000000u);
    g = (int32_t)((s << 16) / (c ? c : 1u));
    k = 131072 - (int32_t)(((int64_t)res_q16 * 125829) >> 16);                    /* 2 - 1.92 resonance */
    f->a1 = (int32_t)(0xFFFFFFFFu / (65536u + (uint32_t)mulq16(g, g + k)));
    f->a2 = mulq16(g, f->a1);
    f->a3 = mulq16(g, f->a2);
}
static int32_t gg_svf_run(gg_svf_t *f, int32_t x)                          /* Q15 in and out */
{
    int32_t v3 = (x << 8) - f->ic2, v1 = mulq16(f->a1, f->ic1) + mulq16(f->a2, v3);
    int32_t v2 = f->ic2 + mulq16(f->a2, f->ic1) + mulq16(f->a3, v3);
    f->ic1 = clampi(2 * v1 - f->ic1, -(1 << 28), 1 << 28);
    f->ic2 = clampi(2 * v2 - f->ic2, -(1 << 28), 1 << 28);
    return v2 >> 8;
}

/* a two-pole resonator: b0 x + a1 y1 + a2 y2, coefficients Q15 */
typedef struct { int32_t b0, a1, a2, y1, y2; } gg_res_t;
static void gg_res_set(gg_res_t *f, uint32_t hz, uint32_t bw)
{
    uint32_t r;
    int32_t c;
    if (hz > 9900u)
        hz = 9900u;
    r = exp2s_q16(-(int32_t)((bw * 3446u) >> 8));                                /* exp(-pi bw / sr) */
    c = sin32(hz * GG_TURN_PER_HZ + 0x40000000u);
    f->a1 = (int32_t)(((int64_t)r * c) >> 15);
    f->a2 = -(int32_t)(((uint64_t)r * r) >> 17);
    f->b0 = (int32_t)(65536u - r) >> 1;
}
static int32_t gg_res_run(gg_res_t *f, int32_t x)
{
    int32_t y = (int32_t)(((int64_t)f->b0 * x + (int64_t)f->a1 * f->y1 + (int64_t)f->a2 * f->y2) >> 15);
    y = clampi(y, -(1 << 24), 1 << 24);
    f->y2 = f->y1;
    f->y1 = y;
    return y;
}

static int32_t gg_pattern(uint32_t pattern, uint32_t step)
{
    static const int8_t major[8] = {0, 4, 7, 12, 16, 12, 7, 4}, minor[8] = {0, 3, 7, 12, 15, 12, 7, 3},
                        penta[16] = {0, 2, 4, 7, 9, 12, 14, 16, 12, 9, 7, 4, 2, 7, 9, 12};
    switch (pattern) {
    case 1: return (int32_t)(step % 3u) * 12;
    case 2: return major[step % 8u];
    case 3: return minor[step % 8u];
    case 4: return penta[step % 16u];
    case 5: {                                                     /* amb-iguos: up and down the scale (SCL) */
        uint32_t m = gr_index(0, P_MODE), n = GR_MODE_N[m], cyc = 2u * n, s = step % cyc, d = s <= n ? s : cyc - s;
        return d >= n ? 12 : GR_MODE_DEG[m][d];
    }
    default: return 0;
    }
}

/* -------------------------------------------------------------- VA --- */
static void gg_va(const int32_t *v, int16_t *o, uint32_t n)
{
    uint32_t w1 = (uint32_t)v[0], w2 = (uint32_t)v[1], p1 = 0, p2 = 0, i, tau = n / 3u ? n / 3u : 1u;
    uint32_t dt1 = GG_DT_ROOT, dt2 = gg_dt(v[2] * 256 + (v[3] * 256) / 100);
    int32_t mix = (v[4] * 32768) / 1000, res = k2q16(v[6]);
    gg_svf_t f = {0, 0, 0, 0, 0};
    for (i = 0; i < n; i++) {
        int32_t s;
        if ((i & 31u) == 0) {
            uint32_t env = exp2s_q16(-mulq16((int32_t)((i << 16) / tau), 94548));
            int32_t e = (5 * v[7] * (int32_t)env) / 1000;
            gg_svf_set(&f, (uint32_t)(((uint64_t)(uint32_t)v[5] * exp2s_q16(e)) >> 16), res);
            if ((i & 4095u) == 0)
                GR_YIELD();
        }
        s = mulq15(32768 - mix, gg_osc(w1, p1, dt1)) + mulq15(mix, gg_osc(w2, p2, dt2));
        o[i] = (int16_t)sat16(gg_svf_run(&f, s) >> 2);
        p1 += dt1;
        p2 += dt2;
    }
}

/* ----------------------------------------------------------- Vowel --- */
static void gg_vowel(const int32_t *v, int16_t *o, uint32_t n)
{
    static const uint16_t F[5][3] = {{800, 1150, 2900}, {400, 1700, 2600}, {300, 2300, 3000}, {450, 800, 2830}, {325, 700, 2530}};
    uint32_t a = (uint32_t)v[0], b = (uint32_t)v[1], i, k, phase = 0, vib = 0;
    uint32_t open32;
    gg_res_t f[3];
    gg_rng_t rng;
    int32_t formant = k2q16(v[3]), morphs = v[2];
    gg_seed(&rng, (uint32_t)v[7] + 1u);
    open32 = 2791728742u - (uint32_t)v[6] * 1932735u;                           /* the glottal pulse's open part: .65 - .45 bright */
    for (k = 0; k < 3u; k++)
        f[k].y1 = f[k].y2 = 0;
    for (i = 0; i < n; i++) {
        int32_t vibrato, pulse, x, out;
        if ((i & 31u) == 0) {
            int32_t t = (int32_t)((i << 16) / n), xm = t * morphs / 1000, tri, m, sm;
            tri = ((xm + 65536) & 131071) - 65536;
            tri = tri < 0 ? -tri : tri;
            m = morphs > 0 ? 65536 - tri : 0;
            sm = mulq16(mulq16(m, m), 196608 - 2 * m);                          /* smoothstep */
            for (k = 0; k < 3u; k++) {
                int32_t hz = F[a][k] + mulq16(sm, (int32_t)F[b][k] - (int32_t)F[a][k]);
                gg_res_set(&f[k], (uint32_t)mulq16(hz, formant), 70u + 35u * k);
            }
            if ((i & 4095u) == 0)
                GR_YIELD();
        }
        vib += 1012872u;                                                         /* 5.2 Hz */
        vibrato = 65536 + ((((sin32(vib) * v[4]) >> 6) * 786) >> 19);
        phase += (uint32_t)(((uint64_t)GG_DT_ROOT * (uint32_t)vibrato) >> 16);
        if (phase < open32) {
            uint32_t u = (phase >> 1) / (open32 >> 17);                          /* phase / open, Q16 */
            pulse = sin32(u << 15);
            pulse = mulq15(pulse, pulse);
        } else {
            pulse = 0;
        }
        x = pulse + mulq15(mulq15(gg_bi(&rng), (v[5] * 8192) / 1000), 13107 + pulse);
        out = gg_res_run(&f[0], x) + mulq15(19661, gg_res_run(&f[1], x)) + mulq15(9830, gg_res_run(&f[2], x));
        o[i] = (int16_t)sat16(out >> 2);
    }
}

/* ----------------------------------------------------------- Pluck --- */
#define GG_RING 512u
static int32_t gg_ring[GG_RING] GR_POOL;
static void gg_pluck(const int32_t *v, int16_t *o, uint32_t n)
{
    uint32_t pattern = (uint32_t)v[0], notes = (uint32_t)v[1], k, i, step = n / notes;
    int32_t loop = 32276 + (int32_t)(((uint32_t)isqrt32((uint32_t)k2q16(v[2]) << 16) * 485u) >> 16);        /* .985 + .0148 sqrt(sustain) */
    int32_t excite = 3277 + (v[3] * 29491) / 1000, s = (v[4] * 16384) / 1000;                       /* s = damping / 2, Q15 */
    gg_rng_t rng;
    gg_seed(&rng, (uint32_t)v[5] + 1u);
    for (i = 0; i < n; i++)
        o[i] = 0;
    for (k = 0; k < notes; k++) {
        int32_t st = gg_pattern(pattern, k) * 256;
        uint32_t ratio = semis_ratio(st), whole, frac, w, start = step * k, count, j, d_q8;
        int32_t prev = 0;
        /* sr / hz in Q8: 22050 / 261.6256 = 84.2788 -> 84.2788 * 65536 = 5523308, over the pitch ratio (Q16) */
        d_q8 = (5523308u << 8) / ratio;
        d_q8 -= (uint32_t)(s >> 7);                                              /* the loop filter adds s samples of delay */
        whole = d_q8 >> 8;
        frac = d_q8 & 255u;
        for (j = 0; j < GG_RING; j++)
            gg_ring[j] = 0;
        {
            int32_t x = 0;
            for (j = 0; j < whole + 2u && j < GG_RING; j++) {
                x += mulq15(excite, (gg_bi(&rng) >> 2) - x);
                gg_ring[j] = x;
            }
        }
        w = whole + 2u;
        count = n - start;
        if (count > 3u * GG_SR)
            count = 3u * GG_SR;
        for (i = 0; i < count; i++) {
            int32_t a = gg_ring[(w - whole) & (GG_RING - 1u)], b = gg_ring[(w - whole - 1u) & (GG_RING - 1u)];
            int32_t x = a + (((b - a) * (int32_t)frac) >> 8);
            int32_t y = mulq15(loop, mulq15(32768 - s, x) + mulq15(s, prev));
            prev = x;
            gg_ring[w & (GG_RING - 1u)] = y;
            w++;
            gg_add(&o[start + i], x >> 1);
            if ((i & 8191u) == 0)
                GR_YIELD();
        }
    }
}

/* -------------------------------------------------------------- FM --- */
static void gg_fm(const int32_t *v, int16_t *o, uint32_t n)
{
    uint32_t pattern = (uint32_t)v[0], notes = (uint32_t)v[1], k, i, step = n / notes;
    int32_t ratio = k2q16(v[2]), index0 = k2q16(v[3]), idxenv = k2q16(v[4]) , fbk = k2q16(v[5]);
    uint32_t tau_amp = (step * (uint32_t)v[6] * 5u) / 2000u;                                         /* decay x 2.5 x step */
    uint32_t tau_idx;
    if (tau_amp < 8u)
        tau_amp = 8u;
    tau_idx = (uint32_t)(((uint64_t)tau_amp * (uint32_t)(68813 - idxenv)) >> 17) + 1u;               /* (1.05 - env) / 2 */
    for (i = 0; i < n; i++)
        o[i] = 0;
    for (k = 0; k < notes; k++) {
        uint32_t dtc = gg_dt(gg_pattern(pattern, k) * 256), dtm = (uint32_t)(((uint64_t)dtc * (uint32_t)ratio) >> 16);
        uint32_t pc = 0, pm = 0, start = step * k, count = n - start, cutoff = tau_amp * 7u;
        uint32_t amp = 0x7FFFFFFFu, astep = 0x7FFFFFFFu - 0x7FFFFFFFu / tau_amp;
        int32_t last = 0, floor_ = index0 - mulq16(index0, idxenv), index = index0;
        uint32_t istep = 0x7FFFFFFFu - 0x7FFFFFFFu / tau_idx;
        int32_t dev = index - floor_;
        if (count > cutoff)
            count = cutoff;
        for (i = 0; i < count; i++) {
            int32_t mod, att = i < 64u ? (int32_t)(i * 512u) : 32768, car;
            uint32_t off;
            off = (uint32_t)((mulq15(fbk, last) * 2 * 10430) >> 0);                                   /* fb x 2 rad x last */
            mod = sin32(pm + off);
            last = mod;
            off = (uint32_t)(((int64_t)index * mod >> 15) * 10430);                                  /* index rad -> turns */
            car = sin32(pc + off);
            gg_add(&o[start + i], mulq15(mulq15(car, att), (int32_t)(amp >> 16)) >> 1);
            pc += dtc;
            pm += dtm;
            amp = (uint32_t)(((uint64_t)amp * astep) >> 31);
            dev = (int32_t)(((int64_t)dev * istep) >> 31);
            index = floor_ + dev;
            if ((i & 8191u) == 0)
                GR_YIELD();
        }
    }
}

/* -------------------------------------------------------- Additive --- */
#define GG_PART 32u
static void gg_additive(const int32_t *v, int16_t *o, uint32_t n)
{
    static const uint32_t LOG2H[33] = {0, 0, 65536, 103872, 131072, 152170, 169408, 183983, 196608, 207744, 217706, 226717,
                                       234944, 242512, 249519, 256042, 262144, 267876, 273280, 278392, 283242, 287855,
                                       292253, 296456, 300480, 304340, 308048, 311616, 315055, 318373, 321578, 324678, 327680};
    uint32_t partials = (uint32_t)v[0], used = 0, h, i, p;
    uint32_t ph[GG_PART], dt[GG_PART], lph[GG_PART], linc[GG_PART];
    int32_t base[GG_PART], gain[GG_PART], even = (v[2] * 32768) / 1000, motion = (v[4] * 32768) / 1000;
    int32_t sx = ((v[3] - 1000) * 65536) / 1000, lgs;                      /* stretch - 1, Q16 */
    gg_rng_t rng;
    gg_seed(&rng, (uint32_t)v[5] + 1u);
    lgs = (int32_t)(((int64_t)sx * (94548 - (((int64_t)sx * 47274) >> 16))) >> 16);          /* log2(1 + x) ~ x (1 - x/2) / ln 2 */
    for (h = 1; h <= partials && h <= GG_PART; h++) {
        uint32_t ratio = exp2s_q16((int32_t)(LOG2H[h] + (int32_t)((h - 1u) * (uint32_t)lgs)));          /* h * stretch^(h-1) */
        uint64_t d = (uint64_t)GG_DT_ROOT * ratio >> 16;
        int32_t tilt = (v[1] * (int32_t)LOG2H[h]) / 1000;
        if (d > 1932735282ull)
            break;
        ph[used] = gg_next(&rng);
        dt[used] = (uint32_t)d;
        base[used] = (int32_t)(exp2s_q16(-tilt) >> 1);                           /* h^-tilt, Q15 */
        if ((h & 1u) == 0)
            base[used] = mulq15(base[used], even);
        linc[used] = 1246513u + gg_u16(&rng) * 95u;                              /* .1 .. .6 Hz, per 64 samples */
        lph[used] = gg_next(&rng);
        gain[used] = 0;
        used++;
    }
    for (i = 0; i < n; i++) {
        int32_t sum = 0;
        if ((i & 63u) == 0) {
            for (p = 0; p < used; p++) {
                int32_t lfo = (sin32(lph[p]) + 32768) >> 1;
                lph[p] += linc[p];
                gain[p] = mulq15(base[p], 32768 - motion + mulq15(motion, lfo));
            }
            if ((i & 4095u) == 0)
                GR_YIELD();
        }
        for (p = 0; p < used; p++) {
            ph[p] += dt[p];
            sum += mulq15(gain[p], sin32(ph[p]));
        }
        o[i] = (int16_t)sat16(sum >> 2);
    }
}

/* --------------------------------------------------------- Texture --- */
static void gg_texture(const int32_t *v, int16_t *o, uint32_t n)
{
    gg_rng_t rng;
    gg_res_t a, b;
    uint32_t i, hum[3] = {0, 0, 0}, sw = 0, sw2 = 0, bw = 40u + ((uint32_t)v[1] * (uint32_t)v[0] * 14u) / 10000u;
    int32_t cenv = 0, cpol = 1, chance = (v[5] * v[5]) / 4207, sweep = k2q16(v[2]);
    uint32_t sinc = ((uint32_t)v[3] * GG_TURN_PER_HZ) / 1000u, sinc2 = (sinc / 100u) * 63u;
    int32_t humk = (v[4] * 32768) / 1000;
    gg_seed(&rng, (uint32_t)v[6] + 1u);
    a.y1 = a.y2 = b.y1 = b.y2 = 0;
    for (i = 0; i < n; i++) {
        int32_t s, noise;
        if ((i & 63u) == 0) {
            int32_t s1 = sin32(sw), s2 = sin32(sw2 + 0x10000000u);
            int32_t swing = 65536 + mulq15(mulq15(sweep, s1), 26214);                      /* 1 + sweep sin 0.8 */
            int32_t swing2 = 65536 + mulq15(mulq15(sweep, s2), 19661);                     /* 1 + sweep sin 0.6 */
            gg_res_set(&a, (uint32_t)mulq16(v[0], swing), bw);
            gg_res_set(&b, (uint32_t)(mulq16(v[0], swing2) * 5 / 2), bw * 2u);
            if ((i & 4095u) == 0)
                GR_YIELD();
        }
        sw += sinc;
        sw2 += sinc2;
        s = mulq15(39322, gg_res_run(&a, gg_bi(&rng))) + mulq15(22938, gg_res_run(&b, gg_bi(&rng)));
        {
            uint32_t h;
            hum[0] += GG_DT_ROOT;                                                   /* the root, a fifth, an octave */
            hum[1] += GG_DT_ROOT / 2u * 3u;
            hum[2] += GG_DT_ROOT * 2u;
            for (h = 0; h < 3u; h++)
                s += mulq15(humk, sin32(hum[h])) / (int32_t)(2u * (h + 1u));
        }
        if ((int32_t)gg_u16(&rng) < chance) {
            cenv = 13107 + (int32_t)(gg_u16(&rng) * 19661u >> 16);
            cpol = gg_bi(&rng) > 0 ? 1 : -1;
        }
        noise = mulq15(cenv, gg_bi(&rng));
        s += cpol * noise * 3 / 2;
        cenv = mulq15(cenv, 32506);
        o[i] = (int16_t)sat16(s >> 2);
    }
}

/* -------------------------------------------------------------- FX --- */
static void gg_fx(const int32_t *v, int16_t *o, uint32_t n)
{
    uint32_t kind = (uint32_t)v[0], i, phase = 0, rphase = 0, hold_n = 1u + (uint32_t)((v[5] * v[5] * 31) / 1000000);
    int32_t crush = v[5], levels = crush > 0 ? 4 + ((1000 - crush) * (1000 - crush) * 2000) / 1000000 : 0;
    int32_t held = 0, hold_c = 0, range = v[2], glitch_gate = 1, nmix = (v[4] * 32768) / 1000;
    int32_t glitch_oct;
    uint32_t rate_inc = (uint32_t)(((uint64_t)v[3] * GG_TURN_PER_HZ) / 1000u);
    gg_rng_t rng;
    gg_svf_t nf = {0, 0, 0, 0, 0};
    gg_seed(&rng, (uint32_t)v[6] + 1u);
    glitch_oct = 0;
    for (i = 0; i < n; i++) {
        int32_t u = (int32_t)((i << 16) / n), amp = 32768, namp = nmix, hz_oct = 0, s, tq = (int32_t)((i << 16) / GG_SR);
        uint32_t cut = 4000u, dt, prev = rphase, hz;
        rphase += rate_inc;
        switch (kind) {
        case 0: {                                                       /* riser */
            int32_t u2 = mulq16(u, u);
            hz_oct = mulq16(range * 65, u2);                           /* range octaves (x1000 -> Q16 approx) */
            amp = u2 >> 1;
            cut = 200u + (uint32_t)mulq16(12000, u2);
            namp = mulq15(nmix, amp) * 3 / 2;
            break;
        }
        case 1: {                                                       /* laser: repeating falling sweeps */
            int32_t rp = (int32_t)(rphase >> 16);
            hz_oct = mulq16(range * 65, 65535 - rp);
            amp = (int32_t)(exp2s_q16(-mulq16(rp, 283700)) >> 1);      /* exp(-3 phase) */
            namp = mulq15(nmix, amp);
            break;
        }
        case 2: {                                                       /* impact */
            hz_oct = mulq16(range * 65, (int32_t)exp2s_q16(-mulq16(tq, 2363900)));       /* exp(-25 t) */
            amp = (int32_t)(exp2s_q16(-mulq16(u * 4, 94548)) >> 1);                     /* exp(-4 u) */
            namp = mulq15(nmix + 9830, (int32_t)(exp2s_q16(-mulq16(tq, 1702000)) >> 1));  /* exp(-18 t) */
            cut = 300u + (uint32_t)mulq16(9000, (int32_t)exp2s_q16(-mulq16(tq, 1134600)));   /* exp(-12 t) */
            break;
        }
        case 3:                                                         /* siren */
            hz_oct = mulq16(range * 16, sin32(rphase) * 2);
            break;
        default:                                                        /* glitch */
            if (rphase < prev) {
                glitch_oct = (int32_t)(gg_u16(&rng) * (uint32_t)(range * 65) >> 16);
                glitch_gate = gg_u16(&rng) > 19661u ? 1 : 0;
            }
            hz_oct = glitch_oct;
            amp = glitch_gate ? 32768 - mulq15(19661, (int32_t)(rphase >> 17)) : 0;
            namp = glitch_gate ? mulq15(nmix, 32768 - (int32_t)(rphase >> 17)) : 0;
            break;
        }
        if ((i & 31u) == 0) {
            gg_svf_set(&nf, cut, 13107);
            if ((i & 4095u) == 0)
                GR_YIELD();
        }
        hz = (uint32_t)(((uint64_t)(uint32_t)v[1] * exp2s_q16(hz_oct)) >> 16);
        if (hz > 9900u)
            hz = 9900u;
        dt = hz * GG_TURN_PER_HZ;
        phase += dt;
        s = mulq15(amp, mulq15(22938, sin32(phase)) + mulq15(9830, gg_osc(0, phase, dt)));
        s += mulq15(namp, gg_svf_run(&nf, gg_bi(&rng)));
        if (crush > 0) {
            if (hold_c-- <= 0) {
                held = (((s * levels + 16384) >> 15) * 32768) / levels;
                hold_c = (int32_t)hold_n - 1;
            }
            s = held;
        }
        o[i] = (int16_t)sat16(s >> 1);
    }
}

#include "gr_gen2.c"
#include "gr_gen3.c"                       /* amb-iguos: the cinema generators */

/* ----------------------------------------------------------- driver --- */
/* The Generate page's settings of a part */
typedef struct {
    uint8_t type;
    uint16_t length;                 /* norm */
    uint16_t norm[GEN_COUNT][8];
} gr_gen_t;
static gr_gen_t gr_gen[NPART];

static void gr_gen_defaults(void)
{
    uint32_t part, t, i;
    for (part = 0; part < NPART; part++) {
        gr_gen[part].type = part ? 2 : 0;                                  /* VA and Pluck, as the Granular synth */
        gr_gen[part].length = GR_GENLEN.def;
        for (t = 0; t < GEN_COUNT; t++)
            for (i = 0; i < GR_GENTYPES[t].n; i++)
                gr_gen[part].norm[t][i] = GR_GENTYPES[t].p[i].def;
    }
}

/* frames for a length norm, at GEN_RATE */
static uint32_t gr_gen_frames(uint32_t length_norm)
{
    int32_t ms = gr_info_real(&GR_GENLEN, length_norm);
    return (uint32_t)ms * (GEN_RATE / 50u) / 20u;                           /* ms x 22.05 */
}

/* Removes DC, closes the loop (sustained sounds), normalises. o holds frames + loop values. */
static void gg_finish(int16_t *o, uint32_t frames, uint32_t loop)
{
    int32_t pin = 0, pout = 0, peak = 0;
    uint32_t i;
    for (i = 0; i < frames + loop; i++) {
        int32_t x = o[i], y = x - pin + mulq15(32719, pout);
        pin = x;
        pout = y;
        o[i] = (int16_t)sat16(y);
    }
    if (loop) {
        for (i = 0; i < loop; i++) {
            uint32_t m = ((i + 1u) << 16) / (loop + 1u);
            int32_t sm = (int32_t)isqrt32(m << 16), sc = (int32_t)isqrt32((65536u - m) << 16);
            o[i] = (int16_t)sat16(mulq16(o[i], sm) + mulq16(o[frames + i], sc));
        }
    } else {
        uint32_t fade = GEN_RATE / 100u;
        if (fade > frames / 2u)
            fade = frames / 2u;
        for (i = 0; i < fade; i++)
            o[frames - 1u - i] = (int16_t)(((int32_t)o[frames - 1u - i] * (int32_t)(i * 65536u / fade)) >> 16);
    }
    for (i = 0; i < frames; i++) {
        int32_t a = absi(o[i]);
        if (a > peak)
            peak = a;
    }
    if (peak > 0) {
        uint32_t gain = (29490u << 16) / (uint32_t)peak;                    /* .9 of full scale, Q16 */
        for (i = 0; i < frames; i++)
            o[i] = (int16_t)sat16((int32_t)(((int64_t)o[i] * gain) >> 16));
    }
    o[frames] = o[0];
}

/* renders the part's generator settings into dst (SMP_FRAMES + 1 values); returns the number of frames */
static uint32_t gr_gen_render(uint32_t part, int16_t *dst)
{
    const gr_gen_t *g = &gr_gen[part];
    const gr_gentype_t *t = &GR_GENTYPES[g->type];
    int32_t v[8];
    uint32_t i, frames = gr_gen_frames(g->length), loop = 0, total;
    for (i = 0; i < t->n; i++)
        v[i] = gr_info_real(&t->p[i], g->norm[g->type][i]);
    if (t->sustained) {
        loop = GEN_RATE / 20u;
        if (loop > frames / 4u)
            loop = frames / 4u;
    }
    if (frames + loop > SMP_FRAMES)
        frames = SMP_FRAMES - loop;
    if (frames < 2048u)
        frames = 2048u;
    total = frames + loop;
    switch (g->type) {
    case GT_VA: gg_va(v, dst, total); break;
    case GT_VOWEL: gg_vowel(v, dst, total); break;
    case GT_PLUCK: gg_pluck(v, dst, total); break;
    case GT_FM: gg_fm(v, dst, total); break;
    case GT_ADDITIVE: gg_additive(v, dst, total); break;
    case GT_TEXTURE: gg_texture(v, dst, total); break;
    case GT_WAVETABLE: gg_wavetable(v, dst, total); break;
    case GT_DRUMS: gg_drums(v, dst, total); break;
    case GT_BOWWIND: gg_bowwind(v, dst, total); break;
    case GT_DUST: gg_dust(v, dst, total); break;
    case GT_BRAAM: gg_braam(v, dst, total); break;
    case GT_SWELL: gg_swell(v, dst, total); break;
    case GT_TENSION: gg_tension(v, dst, total); break;
    case GT_IMPACT: gg_impact(v, dst, total); break;
    case GT_SHEPARD: gg_shepard(v, dst, total); break;
    default: gg_fx(v, dst, total); break;
    }
    gg_finish(dst, frames, loop);
    return frames;
}
