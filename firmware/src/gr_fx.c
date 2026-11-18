/* SPDX-License-Identifier: GPL-3.0-only
 * The shared effects: a ping-pong delay and a Dattorro plate reverb, as in the Granular synth, in fixed point.
 * The delay runs at half the sample rate (22.05 kHz, a darker tape-like echo) to fit the FM-1's RAM; both keep
 * their lines in 16 bits. */
#ifndef GR_POOL
#define GR_POOL __attribute__((section(".pool")))
#endif

static inline int32_t sat16(int32_t v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : v; }

/* ------------------------------------------------------------ delay --- */
#define DLY_N 16384u                 /* half-rate frames per side: 0.74 s */
static int16_t dly_l[DLY_N] GR_POOL, dly_r[DLY_N] GR_POOL;
static struct {
    uint32_t w;
    int32_t delay;                   /* half-rate frames, Q16 */
    int32_t lpl, lpr;                /* damping filters, Q15 */
    int32_t pl, pr;                  /* the previous wet pair (for the upsampling) */
} dly;

/* adds the wet signal to outL/outR. time16 = ms x 16, fb and damp = x 1000 */
static void gr_delay_run(const int32_t *inL, const int32_t *inR, int32_t *outL, int32_t *outR, uint32_t n,
                         int32_t time16, int32_t fb1000, int32_t damp1000)
{
    int32_t target = (int32_t)((uint32_t)time16 * 90317u);                    /* ms x 16 -> half-rate frames, Q16 */
    int32_t fb = (fb1000 * 32768 + 500) / 1000, lpc = 32768 - (damp1000 * 29491) / 1000;
    uint32_t i;
    if (target > (int32_t)((DLY_N - 3u) << 16))
        target = (int32_t)((DLY_N - 3u) << 16);
    if (target < 65536)
        target = 65536;
    if (dly.delay <= 0)
        dly.delay = target;
    for (i = 0; i + 1u < n; i += 2u) {
        int32_t in = (inL[i] + inR[i] + inL[i + 1u] + inR[i + 1u]) >> 2;       /* mono, half rate */
        uint32_t i0, i1;
        int32_t fr, a, b, pos;
        dly.delay += (target - dly.delay) >> 10;
        pos = (int32_t)(dly.w << 16) - dly.delay;
        if (pos < 0)
            pos += (int32_t)(DLY_N << 16);
        i0 = (uint32_t)pos >> 16;
        fr = (pos >> 1) & 0x7FFF;
        if (i0 >= DLY_N)
            i0 -= DLY_N;
        i1 = i0 + 1u == DLY_N ? 0u : i0 + 1u;
        a = dly_l[i0] + (((dly_l[i1] - dly_l[i0]) * fr) >> 15);
        b = dly_r[i0] + (((dly_r[i1] - dly_r[i0]) * fr) >> 15);
        dly.lpl += mulq15(lpc, a - dly.lpl);
        dly.lpr += mulq15(lpc, b - dly.lpr);
        dly_l[dly.w] = (int16_t)sat16(in + mulq15(fb, dly.lpr));
        dly_r[dly.w] = (int16_t)sat16(mulq15(fb, dly.lpl));
        if (++dly.w == DLY_N)
            dly.w = 0;
        outL[i] += (dly.pl + a) >> 1;
        outR[i] += (dly.pr + b) >> 1;
        outL[i + 1u] += a;
        outR[i + 1u] += b;
        dly.pl = a;
        dly.pr = b;
    }
}

/* ----------------------------------------------------------- reverb --- */
enum { RV_DIF0, RV_DIF1, RV_DIF2, RV_DIF3, RV_D1L, RV_D2L, RV_APL, RV_D3L, RV_D1R, RV_D2R, RV_APR, RV_D3R, RV_N };
#define RV_EXC 18u                                  /* the modulation's excursion (16) + 2 */
static const uint16_t RV_SIZE[RV_N] = {142, 107, 379, 277, 672 + 2 * RV_EXC, 4453, 1800, 3720,
                                       908 + 2 * RV_EXC, 4217, 2656, 3163};
#define RV_TOTAL (142 + 107 + 379 + 277 + 672 + 2 * RV_EXC + 4453 + 1800 + 3720 + 908 + 2 * RV_EXC + 4217 + 2656 + 3163 + RV_N)
static int16_t rv_mem[RV_TOTAL] GR_POOL;
static struct {
    int16_t *buf[RV_N];
    uint32_t size[RV_N], w[RV_N];
    int32_t bw, lpl, lpr, fbl, fbr;
    uint32_t lfo;
    uint8_t ready;
} rv;

static void gr_reverb_init(void)
{
    uint32_t i, off = 0;
    for (i = 0; i < RV_N; i++) {
        rv.buf[i] = rv_mem + off;
        rv.size[i] = RV_SIZE[i] + 1u;
        off += rv.size[i];
        rv.w[i] = 0;
    }
    rv.ready = 1;
}

static inline int32_t rv_tap(uint32_t l, uint32_t d)
{
    uint32_t w = rv.w[l], r = w >= d ? w - d : w + rv.size[l] - d;
    return rv.buf[l][r];
}
static inline void rv_push(uint32_t l, int32_t v)
{
    rv.buf[l][rv.w[l]] = (int16_t)sat16(v);
    if (++rv.w[l] == rv.size[l])
        rv.w[l] = 0;
}
static inline int32_t rv_allpass(uint32_t l, int32_t x, int32_t g, uint32_t d)
{
    int32_t del = rv_tap(l, d), v = x - mulq15(g, del);
    rv_push(l, v);
    return del + mulq15(g, v);
}
static inline int32_t rv_modallpass(uint32_t l, int32_t x, int32_t g, uint32_t d_q8)
{
    uint32_t whole = d_q8 >> 8;
    int32_t fr = (int32_t)(d_q8 & 255u), s0 = rv_tap(l, whole), s1 = rv_tap(l, whole + 1u), del, v;
    del = s0 + (((s1 - s0) * fr) >> 8);
    v = x - mulq15(g, del);
    rv_push(l, v);
    return del + mulq15(g, v);
}

/* ---------------------------------------------------------- shimmer --- */
/* amb-iguos: the reverb's tank, pitched up an octave (or a fifth, an octave and a fifth, or down an octave) inside its
 * own feedback, so each pass round the tank climbs: the loop gain stays the decay's, the tail cannot run away. A two-tap delay-line pitch shifter: two read heads half a window
 * apart sweep through a 2048-sample line at (ratio - 1) samples a sample, each faded in and out by a triangle. */
#define SH_N 2048u
static int16_t sh_buf[SH_N] GR_POOL;
static struct {
    uint32_t w;
    int32_t d;                       /* the first head's delay, Q16 samples (0 .. SH_N) */
    int32_t out, lp;                 /* the last shifted sample, a gentle low-pass on the way in (Q15) */
} sh;
static const int32_t SH_RATE[4] = {65536, 32657, 130849, -32768};          /* +12, +7, +19, -12: (ratio - 1), Q16 */

static inline int32_t sh_tap(int32_t d_q16)
{
    int32_t whole = d_q16 >> 16, fr = (d_q16 >> 1) & 0x7FFF;
    uint32_t a = (sh.w - 1u - (uint32_t)whole) & (SH_N - 1u), b = (a - 1u) & (SH_N - 1u);
    return sh_buf[a] + (((sh_buf[b] - sh_buf[a]) * fr) >> 15);
}

/* x: the reverb's wet mono sample; returns the shifted sample */
static inline int32_t sh_step(int32_t x, uint32_t interval)
{
    const int32_t len = (int32_t)(SH_N - 4u) << 16, half = len / 2;
    int32_t d2, w1, t1, t2;
    sh.lp += (x - sh.lp) >> 1;                       /* a touch darker each time round */
    sh_buf[sh.w] = (int16_t)sat16(sh.lp);
    sh.w = (sh.w + 1u) & (SH_N - 1u);
    sh.d -= SH_RATE[interval & 3u];
    if (sh.d < 0)
        sh.d += len;
    else if (sh.d >= len)
        sh.d -= len;
    d2 = sh.d + half;
    if (d2 >= len)
        d2 -= len;
    t1 = sh.d >> 15;                                /* 0 .. 2 x (SH_N - 4): the triangle of the first head */
    w1 = t1 < (half >> 15) ? t1 * 32767 / (half >> 15) : (2 * (half >> 15) - t1) * 32767 / (half >> 15);
    t2 = 32767 - w1;
    sh.out = mulq15(sh_tap(sh.d), w1) + mulq15(sh_tap(d2), t2);
    return sh.out;
}

/* adds the wet signal; decay and damp x 1000; shimmer x 1000 and its interval (0 .. 3) */
static void gr_reverb_run(const int32_t *inL, const int32_t *inR, int32_t *outL, int32_t *outR, uint32_t n,
                          int32_t decay1000, int32_t damp1000, int32_t shim1000, uint32_t interval)
{
    int32_t shg = (shim1000 * 26214) / 1000;                                   /* up to 0.8 of the feedback pitched */
    static const uint16_t TL[7] = {266, 2974, 1913, 1996, 1990, 187, 1066}, TR[7] = {353, 3627, 1228, 2673, 2111, 335, 121};
    int32_t decay = (decay1000 * 32768 + 500) / 1000, dampc = 32768 - (damp1000 * 32768 + 500) / 1000;
    int32_t dif2 = clampi(decay + 4915, 8192, 16384);                         /* min(.5, max(.25, decay + .15)) */
    uint32_t i;
    if (!rv.ready)
        return;
    for (i = 0; i < n; i++) {
        int32_t x, a, b, a1, a2, a3, b1, b2, b3, lfo, wl, wr;
        rv.bw += mulq15(32751, ((inL[i] + inR[i]) >> 1) - rv.bw);
        x = rv.bw;
        x = rv_allpass(RV_DIF0, x, 24576, 142);
        x = rv_allpass(RV_DIF1, x, 24576, 107);
        x = rv_allpass(RV_DIF2, x, 20480, 379);
        x = rv_allpass(RV_DIF3, x, 20480, 277);
        rv.lfo += 87650u;                                                       /* 0.9 Hz */
        lfo = sin32(rv.lfo);                                                    /* Q15 */
        a = x + mulq15(decay, rv.fbr);
        a = rv_modallpass(RV_D1L, a, -22938, (672u << 8) + (uint32_t)((lfo * 16 * 256) >> 15));
        a1 = rv_tap(RV_D2L, 4453);
        rv_push(RV_D2L, a);
        rv.lpl += mulq15(dampc, a1 - rv.lpl);
        a2 = rv_allpass(RV_APL, mulq15(rv.lpl, decay), dif2, 1800);
        a3 = rv_tap(RV_D3L, 3720);
        rv_push(RV_D3L, a2);
        b = x + mulq15(decay, rv.fbl);
        b = rv_modallpass(RV_D1R, b, -22938, (908u << 8) - (uint32_t)((lfo * 16 * 256) >> 15));
        b1 = rv_tap(RV_D2R, 4217);
        rv_push(RV_D2R, b);
        rv.lpr += mulq15(dampc, b1 - rv.lpr);
        b2 = rv_allpass(RV_APR, mulq15(rv.lpr, decay), dif2, 2656);
        b3 = rv_tap(RV_D3R, 3163);
        rv_push(RV_D3R, b2);
        if (shg) {                                                              /* the shimmer, in the loop */
            int32_t up = sh_step((a3 + b3) >> 1, interval);
            a3 += mulq15(shg, up - a3);
            b3 += mulq15(shg, up - b3);
        }
        rv.fbl = a3;
        rv.fbr = b3;
        wl = rv_tap(RV_D2R, TL[0]) + rv_tap(RV_D2R, TL[1]) - rv_tap(RV_APR, TL[2]) + rv_tap(RV_D3R, TL[3]) -
             rv_tap(RV_D2L, TL[4]) - rv_tap(RV_APL, TL[5]) - rv_tap(RV_D3L, TL[6]);
        wr = rv_tap(RV_D2L, TR[0]) + rv_tap(RV_D2L, TR[1]) - rv_tap(RV_APL, TR[2]) + rv_tap(RV_D3L, TR[3]) -
             rv_tap(RV_D2R, TR[4]) - rv_tap(RV_APR, TR[5]) - rv_tap(RV_D3R, TR[6]);
        outL[i] += mulq15(19661, wl);                                           /* 0.6 */
        outR[i] += mulq15(19661, wr);
    }
}
