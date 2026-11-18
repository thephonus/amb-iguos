/* SPDX-License-Identifier: GPL-3.0-only
 * The master filter of a part: a 24 dB/octave vintage ladder and a 12 dB/octave resonant filter (low-pass,
 * band-pass, high-pass), all fixed point. The shapes are those of the Granular synth's Filter.h. States and
 * signals are Q23 (Q15 with 8 more bits, so that low cutoffs keep their resolution); coefficients are Q16. */
enum { FT_OFF, FT_LADDER, FT_LP, FT_BP, FT_HP };

typedef struct {
    int32_t l[4], r[4];          /* ladder stages / SVF ic1, ic2 */
    int32_t g;                   /* last cutoff coefficient, Q24 (0 = none yet) */
    uint8_t mode;
} gr_filter_t;

/* smooth limiter reaching +-1 at +-1.5 (Q23 in and out) */
static inline int32_t flt_sat(int32_t x)
{
    const int32_t one = 1 << 23, lim = 3 << 22;
    int32_t x2, x3;
    if (x > lim)
        return one;
    if (x < -lim)
        return -one;
    x2 = (int32_t)(((int64_t)x * x) >> 23);
    x3 = (int32_t)(((int64_t)x2 * x) >> 23);
    return x - (int32_t)(((int64_t)x3 * 9709) >> 16);
}

/* tan(pi f / FS) for the cutoff's norm (Q16), Q24 */
static uint32_t flt_tan(uint32_t norm)
{
    uint32_t i = norm >> 8, f = norm & 255u;
    if (i >= 256u)
        return GR_TAN[256];
    return GR_TAN[i] + (uint32_t)(((uint64_t)(GR_TAN[i + 1u] - GR_TAN[i]) * f) >> 8);
}

static void flt_ladder(int32_t *x, uint32_t n, uint32_t g16, uint32_t reso, uint32_t drive, int32_t *s)
{
    uint32_t h = 0xFFFFFFFFu / (65536u + g16);
    int32_t G = mulq16((int32_t)g16, (int32_t)h), G2 = mulq16(G, G), G3 = mulq16(G2, G), G4 = mulq16(G2, G2);
    int32_t k = (int32_t)(((uint64_t)reso * 268698u) >> 16);                     /* 4.1 * resonance */
    uint32_t solve = 0xFFFFFFFFu / (65536u + (uint32_t)mulq16(k, G4));
    int32_t inGain = 65536 + (int32_t)(4u * drive);
    uint32_t den = 65536u + drive + (drive >> 1);                                  /* 1 + 1.5 drive */
    int32_t outGain = mulq16(65536 + mulq16(k, 29491), (int32_t)(0xFFFFFFFFu / den));
    int32_t s1 = s[0], s2 = s[1], s3 = s[2], s4 = s[3];
    uint32_t i;
    for (i = 0; i < n; i++) {
        int32_t sum = (int32_t)(((int64_t)G3 * s1 + (int64_t)G2 * s2 + (int64_t)G * s3) >> 16) + s4;
        int32_t u, v, y1, y2, y3, y4;
        sum = mulq16(sum, (int32_t)h);
        u = mulq16(mulq16(x[i] << 8, inGain) - mulq16(k, sum), (int32_t)solve);
        u = flt_sat(u);
        v = mulq16(u - s1, G);
        y1 = v + s1;
        s1 = y1 + v;
        v = mulq16(y1 - s2, G);
        y2 = v + s2;
        s2 = y2 + v;
        v = mulq16(y2 - s3, G);
        y3 = v + s3;
        s3 = y3 + v;
        v = mulq16(y3 - s4, G);
        y4 = v + s4;
        s4 = y4 + v;
        x[i] = mulq16(y4, outGain) >> 8;
    }
    s[0] = s1;
    s[1] = s2;
    s[2] = s3;
    s[3] = s4;
}

static void flt_svf(int32_t *x, uint32_t n, uint32_t g16, uint32_t reso, uint32_t drive, uint32_t mode, int32_t *s)
{
    int32_t k = 131072 - (int32_t)(((uint64_t)reso * 128450u) >> 16);                /* 2 - 1.96 resonance */
    uint32_t a1 = 0xFFFFFFFFu / (65536u + (uint32_t)mulq16((int32_t)g16, (int32_t)g16 + k));
    int32_t A1 = (int32_t)a1, A2 = mulq16((int32_t)g16, A1), A3 = mulq16((int32_t)g16, A2);
    uint32_t kh = (uint32_t)(k >> 1);
    int32_t sq = (int32_t)isqrt32((kh > 65535u ? 65535u : kh) << 16);                /* sqrt(k / 2), Q16 */
    int32_t inGain = mulq16(65536 + (int32_t)(4u * drive), 19661 + mulq16(45875, sq));  /* (1 + 4 drive)(0.3 + 0.7 sqrt) */
    uint32_t den = 65536u + drive + (drive >> 1);
    int32_t outGain = (int32_t)(0xFFFFFFFFu / den);
    int32_t bandGain = k > 16384 ? k : 16384;
    int32_t ic1 = s[0], ic2 = s[1];
    uint32_t i;
    for (i = 0; i < n; i++) {
        int32_t in = mulq16(x[i] << 8, inGain), v3 = in - ic2, v1, v2, out;
        v1 = mulq16(A1, ic1) + mulq16(A2, v3);
        v2 = ic2 + mulq16(A2, ic1) + mulq16(A3, v3);
        ic1 = 2 * v1 - ic1;
        ic2 = 2 * v2 - ic2;
        if (mode == FT_LP)
            out = v2;
        else if (mode == FT_BP)
            out = mulq16(v1, bandGain);
        else
            out = in - mulq16(k, v1) - v2;
        x[i] = mulq16(flt_sat(out), outGain) >> 8;
    }
    s[0] = ic1;
    s[1] = ic2;
}

/* BLK stereo frames in place; cutoff is the (modulated) norm */
static void gr_filter_run(gr_filter_t *f, int32_t *left, int32_t *right, uint32_t mode, uint32_t cut_norm,
                          uint32_t reso, uint32_t drive)
{
    const uint32_t sub = 16u, nsub = BLK / sub;
    uint32_t target, start, s;
    if (mode != f->mode) {
        uint32_t k;
        f->mode = (uint8_t)mode;
        for (k = 0; k < 4u; k++)
            f->l[k] = f->r[k] = 0;
        f->g = 0;
    }
    if (mode == FT_OFF)
        return;
    target = flt_tan(cut_norm);
    start = f->g ? (uint32_t)f->g : target;
    f->g = (int32_t)target;
    for (s = 0; s < nsub; s++) {
        uint32_t g = (uint32_t)((int32_t)start + ((int32_t)target - (int32_t)start) * (int32_t)(s + 1u) / (int32_t)nsub);
        uint32_t g16 = g >> 8;
        if (mode == FT_LADDER) {
            flt_ladder(left + s * sub, sub, g16, reso, drive, f->l);
            flt_ladder(right + s * sub, sub, g16, reso, drive, f->r);
        } else {
            flt_svf(left + s * sub, sub, g16, reso, drive, mode, f->l);
            flt_svf(right + s * sub, sub, g16, reso, drive, mode, f->r);
        }
    }
}
