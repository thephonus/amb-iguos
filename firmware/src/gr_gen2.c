/* SPDX-License-Identifier: GPL-3.0-only
 * More sample generators (included by gr_gen.c): Wavetable, Drums, Bow/Wind (a bowed string, a flute and a clarinet
 * as waveguides, after the models in STK) and Dust (sparse random clicks through resonant bodies). Same conventions as
 * the others: 22.05 kHz, an int16 buffer with headroom, integer only, normalised at the end. */

static const uint32_t GG_LOG2H[33] = {0, 0, 65536, 103872, 131072, 152170, 169408, 183983, 196608, 207744, 217706, 226717,
                                      234944, 242512, 249519, 256042, 262144, 267876, 273280, 278392, 283242, 287855,
                                      292253, 296456, 300480, 304340, 308048, 311616, 315055, 318373, 321578, 324678, 327680};

static uint32_t gg_decay(uint32_t tau)               /* the Q31 step of an exponential with a time constant tau (samples) */
{
    if (tau < 2u)
        tau = 2u;
    return 0x7FFFFFFFu - 0x7FFFFFFFu / tau;
}
static inline int32_t gg_mul31(int32_t a, uint32_t k) { return (int32_t)(((int64_t)a * k) >> 31); }

/* ---------------------------------------------------------- Wavetable --- */
#define WT_FR 8u
#define WT_N 256u
static int16_t wt_tab[WT_FR][WT_N] GR_POOL;

/* harmonic h (1..32) of table `table` at morph m (Q16): Q15 */
static int32_t wt_amp(uint32_t table, uint32_t h, int32_t m, gg_rng_t *rng)
{
    int32_t a = 0;
    switch (table) {
    case 0:                                             /* saw to square: the even harmonics fade out */
        a = (int32_t)(32768u / h);
        if (!(h & 1u))
            a = mulq16(a, 65536 - m);
        break;
    case 1: {                                           /* dark to bright: the spectral tilt moves from 1/h^2 to 1/h^0.4 */
        int32_t e = 131072 - mulq16(m, 104858);
        a = (int32_t)(exp2s_q16(-mulq16(e, (int32_t)GG_LOG2H[h])) >> 1);
        break;
    }
    case 2: {                                           /* a formant peak sweeping up the harmonics */
        int32_t c = 65536 + mulq16(m, 917504), d = ((int32_t)h << 16) - c, q = mulq16(mulq16(d, d), 10486);   /* ((h - c) / 2.5)^2 */
        a = (int32_t)(exp2s_q16(-mulq16(q, 94548)) >> 2) + (int32_t)(9830u / h);
        break;
    }
    case 3: {                                           /* drawbars: 1 2 3 4 6 8, two settings and the morph between */
        static const uint8_t A[9] = {32, 6, 0, 0, 0, 0, 0, 0, 0}, B[9] = {16, 20, 26, 22, 0, 16, 0, 12, 0};
        if (h <= 8u && h != 5u && h != 7u)
            a = (int32_t)(A[h - 1u] + mulq16((int32_t)B[h - 1u] - (int32_t)A[h - 1u], m)) * 800;
        break;
    }
    default: {                                          /* digital: random amplitudes, a new set for each frame */
        int32_t r = (int32_t)gg_u16(rng) >> 1;
        a = mulq15(mulq15(r, r), (int32_t)(isqrt32(65536u / h) * 128u));
        break;
    }
    }
    return a;
}

static void wt_build(uint32_t table, gg_rng_t *rng)
{
    uint32_t f, i, h;
    int32_t amp[33];
    for (f = 0; f < WT_FR; f++) {
        int32_t m = (int32_t)(f * 65536u / (WT_FR - 1u)), peak = 1, gain;
        static int32_t acc[WT_N];
        for (h = 1; h <= 32u; h++)
            amp[h] = wt_amp(table, h, m, rng);
        for (i = 0; i < WT_N; i++) {
            int32_t sum = 0;
            for (h = 1; h <= 32u; h++)
                if (amp[h])
                    sum += mulq15(amp[h], sin32((i * h) << 24));
            acc[i] = sum;
            if (absi(sum) > peak)
                peak = absi(sum);
        }
        gain = (int32_t)((26000u << 12) / (uint32_t)peak);               /* to a peak of 26000, Q12 */
        for (i = 0; i < WT_N; i++)
            wt_tab[f][i] = (int16_t)clampi((int32_t)(((int64_t)acc[i] * gain) >> 12), -32767, 32767);
        GR_YIELD();
    }
}

static inline int32_t wt_read(uint32_t phase, uint32_t frame, uint32_t frac16)
{
    uint32_t i = phase >> 24, j = (i + 1u) & (WT_N - 1u), f2 = frame + 1u < WT_FR ? frame + 1u : frame;
    int32_t fr = (int32_t)((phase >> 9) & 0x7FFFu);
    int32_t a = wt_tab[frame][i] + (((wt_tab[frame][j] - wt_tab[frame][i]) * fr) >> 15);
    int32_t b = wt_tab[f2][i] + (((wt_tab[f2][j] - wt_tab[f2][i]) * fr) >> 15);
    return a + (int32_t)(((int64_t)(b - a) * (int32_t)frac16) >> 16);
}

static void gg_wavetable(const int32_t *v, int16_t *o, uint32_t n)
{
    gg_rng_t rng;
    uint32_t i, p1 = 0, p2 = 0x40000000u, ps = 0, lp = 0;
    uint32_t dt1 = GG_DT_ROOT, dt2 = gg_dt(((v[5] * 256) / 100)), dts = GG_DT_ROOT / 2u, linc = (uint32_t)v[4] * GG_TURN_PER_HZ / 1000u;
    int32_t pos0 = k2q16(v[1]), sweep = k2q16(v[2]), motion = k2q16(v[3]), sub = (v[6] * 32768) / 1000;
    gg_seed(&rng, (uint32_t)v[7] + 1u);
    wt_build((uint32_t)v[0], &rng);
    for (i = 0; i < n; i++) {
        int32_t u = (int32_t)((i << 16) / n), pos, s, w1, w2;
        uint32_t fp, frame, frac;
        if ((i & 4095u) == 0)
            GR_YIELD();
        lp += linc;
        pos = clampi(pos0 + mulq16(sweep, u) + mulq15(motion, sin32(lp)), 0, 65535);
        fp = (uint32_t)pos * (WT_FR - 1u);
        frame = fp >> 16;
        frac = fp & 0xFFFFu;
        w1 = wt_read(p1, frame, frac);
        w2 = wt_read(p2, frame, frac);
        s = ((w1 + w2) >> 1) + mulq15(sub, sin32(ps));
        o[i] = (int16_t)sat16(s >> 1);
        p1 += dt1;
        p2 += dt2 + dt1;
        ps += dts;
    }
    (void)p2;
}

/* -------------------------------------------------------------- Drums --- */
/* one hit of `kind` into o[start ..], which has room for `room` samples; lv is its level (Q15), tune in semitones x 256 */
static void gg_hit(uint32_t kind, int16_t *o, uint32_t start, uint32_t room, int32_t tune, int32_t lv, int32_t decay,
                   int32_t tone, int32_t snap, gg_rng_t *rng)
{
    uint32_t r = semis_ratio(tune), i, count = room, ph = 0, ph2 = 0;
    gg_svf_t f1 = {0, 0, 0, 0, 0}, f2 = {0, 0, 0, 0, 0};
    int32_t amp = 32767, amp2 = 32767, pe = 32767, a;
    uint32_t k_amp, k_amp2, k_pe, k_noise = 0, bph[6] = {0, 0, 0, 0, 0, 0}, bdt[6];
    int32_t noise_amp = 32767;
    static const uint16_t METAL[6] = {2053, 3044, 3696, 5227, 5400, 8000};             /* the 808's six squares, Hz x 10 */
    int32_t dec = 21845 + mulq15(decay, 2 * 21845);                                    /* 0.33 .. 1.0 */
    switch (kind) {
    case 0: k_amp = gg_decay((uint32_t)(((60 + (decay * 440 >> 15)) * 22))); k_pe = gg_decay(22 * 14); k_amp2 = 0; break;
    case 1: k_amp = gg_decay((uint32_t)((55 + (decay * 90 >> 15)) * 22)); k_amp2 = gg_decay((uint32_t)((90 + (decay * 330 >> 15)) * 22)); k_pe = gg_decay(22 * 8); break;
    case 2: k_amp = gg_decay((uint32_t)(((14 + (decay * 40 >> 15)) * 22))); k_amp2 = 0; k_pe = 0; break;
    case 3: k_amp = gg_decay((uint32_t)(((80 + (decay * 500 >> 15)) * 22))); k_amp2 = 0; k_pe = 0; break;
    case 4: k_amp = gg_decay((uint32_t)((60 + (decay * 360 >> 15)) * 22)); k_amp2 = gg_decay(22 * 3); k_pe = 0; break;
    case 5: k_amp = gg_decay((uint32_t)((110 + (decay * 400 >> 15)) * 22)); k_pe = gg_decay(22 * 40); k_amp2 = 0; break;
    case 6: k_amp = gg_decay((uint32_t)((90 + (decay * 400 >> 15)) * 22)); k_amp2 = gg_decay(22 * 12); k_pe = 0; break;
    default: k_amp = gg_decay(22 * 7); k_amp2 = gg_decay(22 * 3); k_pe = 0; break;
    }
    (void)dec;
    for (i = 0; i < 6u; i++)
        bdt[i] = (uint32_t)(((uint64_t)((uint32_t)METAL[i] * GG_TURN_PER_HZ / 10u) * r) >> 16);
    if (kind == 2u || kind == 3u)
        gg_svf_set(&f1, 3000u + (uint32_t)(tone * 4000 >> 15), 6554);                  /* the high-pass is a low-pass taken away */
    if (kind == 4u) {
        gg_svf_set(&f1, (uint32_t)(((uint64_t)2400u * r) >> 16), 13107);
        gg_svf_set(&f2, (uint32_t)(((uint64_t)700u * r) >> 16), 13107);
    }
    if (kind == 1u)
        gg_svf_set(&f1, 1800u + (uint32_t)(tone * 2000 >> 15), 6554);
    if (kind == 6u)
        gg_svf_set(&f1, 3200u, 19661);
    for (i = 0; i < count; i++) {
        int32_t s = 0, nz = gg_bi(rng), hp;
        if (amp < 6 && (kind != 4u || noise_amp < 6))
            break;
        switch (kind) {
        case 0:                                                  /* kick: a sine falling from high, a click of noise */
        case 5: {                                                /* tom: the same, higher and with less fall */
            uint32_t base = kind == 0u ? 48u : 118u, depth = kind == 0u ? 2u + (uint32_t)(snap >> 13) : 1u;
            uint32_t hz = (uint32_t)(((uint64_t)base * r) >> 16), hzt = hz + hz * depth * (uint32_t)pe / 32768u;
            ph += hzt * GG_TURN_PER_HZ;
            s = mulq15(sin32(ph), amp);
            s = mulq15(s, 32768 + tone);                         /* tone drives it into saturation */
            s = gr_softclip(s + (s >> 2)) ;
            if (i < 44u)
                s += mulq15(nz, snap) >> 1;
            pe = gg_mul31(pe, k_pe);
            amp = gg_mul31(amp, k_amp);
            break;
        }
        case 1:                                                  /* snare: two tuned bodies and a noise rattle */
            ph += (uint32_t)(((uint64_t)185u * r) >> 16) * GG_TURN_PER_HZ;
            ph2 += (uint32_t)(((uint64_t)330u * r) >> 16) * GG_TURN_PER_HZ;
            s = mulq15(sin32(ph) + (sin32(ph2) >> 1), mulq15(amp, 32768 - (tone >> 1)));
            nz = gg_svf_run(&f1, nz);
            hp = gg_bi(rng) - nz;                                /* the noise, high-passed */
            s += mulq15(hp, mulq15(amp2, 16384 + (tone >> 1)));
            if (i < 66u)
                s += mulq15(gg_bi(rng), snap) >> 1;
            amp = gg_mul31(amp, k_amp);
            amp2 = gg_mul31(amp2, k_amp2);
            break;
        case 2:
        case 3: {                                                /* hats: six squares, high-passed */
            uint32_t j;
            int32_t m = 0;
            for (j = 0; j < 6u; j++) {
                bph[j] += bdt[j];
                m += bph[j] < 0x80000000u ? 4096 : -4096;
            }
            hp = m - gg_svf_run(&f1, m);
            s = mulq15(hp, amp);
            s += mulq15(nz, mulq15(amp, tone)) >> 1;
            if (i < 22u)
                s += mulq15(nz, snap) >> 1;
            amp = gg_mul31(amp, k_amp);
            break;
        }
        case 4: {                                                /* clap: four quick bursts, then a tail */
            int32_t bp = gg_svf_run(&f1, nz) - gg_svf_run(&f2, nz), burst = 0;
            uint32_t at = i / 200u;                              /* 9 ms */
            if (at < 4u && i % 200u < 66u)
                burst = 32767 - (int32_t)(i % 200u) * 480;
            s = mulq15(bp, mulq15(burst, 32767) + (at >= 3u ? amp : 0));
            s = s + s;
            s += mulq15(bp, amp2) >> 2;
            if (at >= 3u)
                amp = gg_mul31(amp, k_amp);
            noise_amp = at >= 3u ? amp : 32767;
            break;
        }
        case 6: {                                                /* cowbell: two squares, band-passed, a double decay */
            uint32_t d1 = (uint32_t)(((uint64_t)562u * r) >> 16) * GG_TURN_PER_HZ, d2 = (uint32_t)(((uint64_t)845u * r) >> 16) * GG_TURN_PER_HZ;
            int32_t sq;
            ph += d1;
            ph2 += d2;
            sq = gg_osc(1, ph, d1) + gg_osc(1, ph2, d2);
            sq = gg_svf_run(&f1, sq >> 1);
            a = mulq15(amp, 22000) + mulq15(amp2, 10000);
            s = mulq15(sq, a);
            amp = gg_mul31(amp, k_amp);
            amp2 = gg_mul31(amp2, k_amp2);
            break;
        }
        default: {                                               /* rim shot: a short high ring and a click */
            uint32_t d1 = (uint32_t)(((uint64_t)1750u * r) >> 16) * GG_TURN_PER_HZ, d2 = (uint32_t)(((uint64_t)480u * r) >> 16) * GG_TURN_PER_HZ;
            ph += d1;
            ph2 += d2;
            s = mulq15(sin32(ph), amp) + mulq15(sin32(ph2), amp) / 2 + mulq15(nz, mulq15(amp2, snap + 8192));
            amp = gg_mul31(amp, k_amp);
            amp2 = gg_mul31(amp2, k_amp2);
            break;
        }
        }
        (void)k_noise;
        gg_add(&o[start + i], mulq15(s, lv) >> 1);
        if ((i & 8191u) == 0)
            GR_YIELD();
    }
}

static void gg_drums(const int32_t *v, int16_t *o, uint32_t n)
{
    uint32_t kind = (uint32_t)v[0], hits = (uint32_t)v[5], k, i, step = n / hits;
    int32_t tune = v[1] * 256, decay = (v[2] * 32768) / 1000, tone = (v[3] * 32768) / 1000, snap = (v[4] * 32768) / 1000, hum = (v[6] * 32768) / 1000;
    gg_rng_t rng;
    gg_seed(&rng, (uint32_t)v[7] + 1u);
    for (i = 0; i < n; i++)
        o[i] = 0;
    for (k = 0; k < hits; k++) {
        int32_t t = tune + mulq15(mulq15(hum, gg_bi(&rng)), 384), lv = 32768 - mulq15(hum, (int32_t)(gg_u16(&rng) >> 1));
        uint32_t start = step * k, room = n - start;
        if (room > 3u * GG_SR / 2u)
            room = 3u * GG_SR / 2u;
        gg_hit(kind, o, start, room, t, mulq15(lv, k == 0u ? 32767 : 28000), decay, tone, snap, &rng);
    }
}

/* ----------------------------------------------------------- Bow/Wind --- */
#define DL_N 512u
static int32_t dl_a[DL_N] GR_POOL, dl_b[DL_N] GR_POOL;
typedef struct { int32_t *b; uint32_t w; int32_t last; } gg_dl_t;
static void dl_clear(gg_dl_t *d, int32_t *buf)
{
    uint32_t i;
    d->b = buf;
    d->w = d->last = 0;
    for (i = 0; i < DL_N; i++)
        buf[i] = 0;
}
/* a value delayed by d_q8 samples (>= 2), linear between the two neighbours */
static inline int32_t dl_rd(const gg_dl_t *d, uint32_t d_q8)
{
    uint32_t whole = d_q8 >> 8, idx = d->w - whole;
    int32_t a = d->b[idx & (DL_N - 1u)], b = d->b[(idx - 1u) & (DL_N - 1u)];
    return a + (((b - a) * (int32_t)(d_q8 & 255u)) >> 8);
}
static inline void dl_wr(gg_dl_t *d, int32_t x)
{
    d->b[d->w & (DL_N - 1u)] = x;
    d->w++;
}
/* a one-pole low-pass: y = (1 - p) x + p y1, p Q15 */
static inline int32_t lp1(int32_t *y, int32_t x, int32_t p)
{
    *y = mulq15(32768 - p, x) + mulq15(p, *y);
    return *y;
}

static void gg_bowwind(const int32_t *v, int16_t *o, uint32_t n)
{
    uint32_t mode = (uint32_t)v[0], i;
    int32_t press = (v[1] * 32768) / 1000, bright = (v[2] * 32768) / 1000, pos = (v[3] * 32768) / 1000, noise = 2000 + (v[4] * 30000) / 1000;   /* some noise always: it starts the tone */
    int32_t vib = (v[5] * 32768) / 1000;
    uint32_t att = (uint32_t)(((uint32_t)v[6] * GG_SR) / 1000u);                    /* attack, samples */
    gg_dl_t d1, d2;
    gg_rng_t rng;
    int32_t y1 = 0, y2 = 0, dc_x = 0, dc_y = 0, last_out = 0;
    uint32_t vph = 0, total_q8 = 21575u;                                            /* sr / 261.63 Hz, Q8 */
    gg_seed(&rng, (uint32_t)v[7] + 1u);
    dl_clear(&d1, dl_a);
    dl_clear(&d2, dl_b);
    if (att < 64u)
        att = 64u;
    for (i = 0; i < n; i++) {
        int32_t env = i < att ? (int32_t)((i << 15) / att) : 32768, vf, out = 0, nz = gg_bi(&rng);
        uint32_t vq8;
        if ((i & 4095u) == 0)
            GR_YIELD();
        vph += 5u * GG_TURN_PER_HZ;                                                 /* 5 Hz */
        vf = mulq15(vib, sin32(vph));                                              /* +-1 .. 0 */
        vq8 = (uint32_t)((int32_t)total_q8 + mulq15(vf, 160));                      /* about +-12 cents at full depth */
        if (mode == 0u) {                                                           /* bowed string: friction at the bow */
            uint32_t beta = 4915u + (uint32_t)mulq15(pos, 3277);                    /* bow position: .15 .. .25 of the string, Q15 */
            uint32_t base = vq8 - 256u, nut_q8, bridge_q8;
            int32_t bridge_refl, nut_refl, string_vel, diff, slope, bowtab, new_vel, bow_vel, p;
            bridge_q8 = (uint32_t)(((uint64_t)base * beta) >> 15);
            nut_q8 = base - bridge_q8;
            if (bridge_q8 < 512u)
                bridge_q8 = 512u;
            if (nut_q8 < 512u)
                nut_q8 = 512u;
            p = 29491 - mulq15(bright, 20000);                                      /* the string's loss: brighter, less */
            bridge_refl = -mulq15(lp1(&y1, dl_rd(&d1, bridge_q8), p), 31130);       /* x 0.95 */
            nut_refl = -dl_rd(&d2, nut_q8);
            string_vel = bridge_refl + nut_refl;
            bow_vel = mulq15(env, 3600 + mulq15(press, 5000));
            diff = bow_vel - string_vel;
            slope = (5 << 12) - mulq15(press, 4 << 12);                             /* 5 - 4 pressure, Q12 */
            {
                int32_t t = (int32_t)(((int64_t)absi(diff) * slope) >> 12) + 24576, t2 = mulq15(t, t), t4 = mulq15(t2, t2);
                bowtab = (int32_t)(1073741824u / (uint32_t)(t4 > 1 ? t4 : 1));
                bowtab = clampi(bowtab, 328, 32768);
            }
            new_vel = mulq15(diff, bowtab);
            dl_wr(&d2, bridge_refl + new_vel);
            dl_wr(&d1, nut_refl + new_vel);
            out = bridge_refl + nut_refl;
        } else if (mode == 1u) {                                                    /* flute: a jet across a bore */
            uint32_t bore = vq8 * 1512u / 1000u;                                    /* a flute's bore is about 1.5 periods long */
            uint32_t jet = (uint32_t)(((uint64_t)bore * (8520u + (uint32_t)mulq15(pos, 3900))) >> 15);      /* the jet: .26 .. .38 of the bore */
            int32_t breath = mulq15(env, 22000 + mulq15(press, 18000)), t, pd;
            breath += mulq15(breath, mulq15(noise, nz) >> 1) + mulq15(breath, mulq15(vib, sin32(vph)) >> 3);
            t = lp1(&y1, d1.last, 22937 - mulq15(bright, 14000));                   /* the bore's loss */
            dc_y = t - dc_x + mulq15(32440, dc_y);                                  /* DC blocker */
            dc_x = t;
            t = -dc_y;
            pd = breath - mulq15(16384, t);                                         /* reflection .5 */
            dl_wr(&d2, pd);
            pd = dl_rd(&d2, jet < 512u ? 512u : jet);
            {
                int32_t jt = clampi(pd, -65535, 65535), jt3 = mulq15(mulq15(jt, jt), jt);
                pd = clampi(jt3 - jt, -32768, 32768);                              /* the jet table: x (x^2 - 1) */
            }
            pd += mulq15(16384, t);
            dl_wr(&d1, pd);
            d1.last = dl_rd(&d1, bore < 512u ? 512u : bore);
            out = d1.last;
        } else {                                                                    /* clarinet: a reed on a closed pipe */
            uint32_t pipe = vq8 / 2u - 330u;
            int32_t breath = mulq15(env, 18022 + mulq15(press, 9830)), pd, reed, last = dl_rd(&d1, pipe < 512u ? 512u : pipe);
            int32_t stiff = 9800 + mulq15(pos, 7000);                               /* the reed's slope .3 .. .51 */
            breath += mulq15(breath, mulq15(noise, nz) >> 2) + mulq15(breath, mulq15(vib, sin32(vph)) >> 3);
            pd = -mulq15(31130, lp1(&y1, last, 16384 + mulq15(bright, -10000) + 8192));
            pd -= breath;
            reed = clampi(19661 - mulq15(stiff, pd), -32768, 32768);              /* offset .6 */
            dl_wr(&d1, clampi(breath + mulq15(pd, reed), -60000, 60000));
            out = last;
        }
        last_out = lp1(&y2, out, 8192);
        o[i] = (int16_t)sat16(last_out);
    }
    {   /* the onset is a loud DC step in the wind models: level the sound by its sustained part, the start may clip */
        int32_t peak = 1, gain;
        for (i = n / 4u; i < n; i++)
            if (absi(o[i]) > peak)
                peak = absi(o[i]);
        gain = (int32_t)((26000u << 10) / (uint32_t)peak);
        for (i = 0; i < n; i++)
            o[i] = (int16_t)sat16((int32_t)(((int64_t)o[i] * gain) >> 10));
    }
}

/* --------------------------------------------------------------- Dust --- */
#define DU_V 12u
static void gg_dust(const int32_t *v, int16_t *o, uint32_t n)
{
    struct { uint32_t ph, dt, k; int32_t amp; uint8_t on; } vc[DU_V];
    gg_rng_t rng;
    gg_svf_t bed = {0, 0, 0, 0, 0};
    uint32_t i, j, rr = 0, burst = 0, gap = 0;
    uint32_t thr = ((uint32_t)v[0] * 19484u) >> 16;                                 /* events a sample, of 65536 (density x 10) */
    int32_t spread = k2q16(v[3]), dyn = (v[4] * 32768) / 1000, clump = (v[5] * 65535) / 1000, bedlvl = (v[6] * 32768) / 1000;
    uint32_t body = (uint32_t)v[1];
    gg_seed(&rng, (uint32_t)v[7] + 1u);
    for (j = 0; j < DU_V; j++)
        vc[j].on = 0;
    gg_svf_set(&bed, body, 16384);
    for (i = 0; i < n; i++) {
        int32_t sum = 0;
        int trig = 0;
        if ((i & 4095u) == 0)
            GR_YIELD();
        if (burst && --gap == 0u) {                                                 /* a clump: more clicks soon after */
            burst--;
            gap = 110u + (gg_u16(&rng) % 500u);
            trig = 1;
        } else if (gg_u16(&rng) < thr) {
            trig = 1;
            if ((int32_t)gg_u16(&rng) < clump) {
                burst = 1u + gg_u16(&rng) % 4u;
                gap = 110u + (gg_u16(&rng) % 500u);
            }
        }
        if (trig) {
            int32_t u = (int32_t)(gg_u16(&rng) >> 1), a = u, oct = mulq16(spread, gg_bi(&rng) * 2);
            uint32_t hz = (uint32_t)(((uint64_t)body * exp2s_q16(oct)) >> 16), tau, k;
            int32_t e = 1 + (dyn * 3 >> 15);
            while (--e > 0)
                a = mulq15(a, u);
            if (hz > 9900u)
                hz = 9900u;
            if (hz < 60u)
                hz = 60u;
            tau = (((uint32_t)v[2] * GG_SR) / 1000u) * (700u + gg_u16(&rng) % 600u) / 1000u;
            k = gg_decay(tau);
            vc[rr % DU_V].on = 1;
            vc[rr % DU_V].ph = 0x40000000u;
            vc[rr % DU_V].dt = hz * GG_TURN_PER_HZ;
            vc[rr % DU_V].k = k;
            vc[rr % DU_V].amp = 6000 + mulq15(a, 20000);
            rr++;
        }
        for (j = 0; j < DU_V; j++) {
            if (!vc[j].on)
                continue;
            vc[j].ph += vc[j].dt;
            sum += mulq15(sin32(vc[j].ph), vc[j].amp);
            vc[j].amp = gg_mul31(vc[j].amp, vc[j].k);
            if (vc[j].amp < 4)
                vc[j].on = 0;
        }
        if (bedlvl)
            sum += mulq15(gg_svf_run(&bed, gg_bi(&rng)), bedlvl) >> 2;
        o[i] = (int16_t)sat16(sum);
    }
}
