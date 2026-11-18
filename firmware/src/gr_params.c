/* SPDX-License-Identifier: GPL-3.0-only
 * Fixed-point helpers, the parameter store and the way values are shown. */

/* ------------------------------------------------------------- math --- */
static inline int32_t mulq15(int32_t a, int32_t b) { return (int32_t)(((int64_t)a * b) >> 15); }
static inline int32_t mulq16(int32_t a, int32_t b) { return (int32_t)(((int64_t)a * b) >> 16); }
static inline int32_t clampi(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline int32_t absi(int32_t v) { return v < 0 ? -v : v; }

/* 2^(x / 65536), x >= 0 and below 15: Q16 */
static uint32_t exp2_q16(uint32_t x)
{
    uint32_t ip = x >> 16, idx = (x >> 8) & 255u, f = x & 255u;
    uint32_t v = GR_EXP2[idx] + (((GR_EXP2[idx + 1u] - GR_EXP2[idx]) * f) >> 8);
    return v << ip;
}

/* 2^(x / 65536) for a signed x in -16 .. 15 octaves: Q16 */
static uint32_t exp2s_q16(int32_t x)
{
    if (x >= 0)
        return exp2_q16((uint32_t)x);
    if (x < -(24 << 16))
        return 0;
    {
        uint32_t ip = (uint32_t)(-x + 65535) >> 16, v = exp2_q16((uint32_t)(x + (int32_t)(ip << 16)));
        return v >> ip;
    }
}

/* the pitch ratio of n semitones (Q8: 256 = one semitone), Q16 */
static uint32_t semis_ratio(int32_t st_q8)
{
    return exp2s_q16((int32_t)(((int64_t)st_q8 * 5461) >> 8));          /* st / 12 octaves */
}

static uint32_t isqrt32(uint32_t v)
{
    uint32_t r = 0, b = 1u << 30;
    while (b > v)
        b >>= 2;
    while (b) {
        if (v >= r + b) {
            v -= r + b;
            r = (r >> 1) + b;
        } else {
            r >>= 1;
        }
        b >>= 2;
    }
    return r;
}

/* sine of a 32-bit phase, Q15, interpolated */
static inline int32_t sin32(uint32_t ph)
{
    uint32_t i = ph >> 22;
    int32_t a = GR_SINE[i], b = GR_SINE[i + 1u];
    return a + (((b - a) * (int32_t)((ph >> 7) & 0x7FFFu)) >> 15);
}

static uint32_t gr_rng_s = 0x6D2B79F5u;
static inline uint32_t gr_rand(void)
{
    gr_rng_s ^= gr_rng_s << 13;
    gr_rng_s ^= gr_rng_s >> 17;
    gr_rng_s ^= gr_rng_s << 5;
    return gr_rng_s;
}
static inline int32_t gr_rand_bi(void)           /* -32768 .. 32767 */
{
    return (int32_t)(gr_rand() >> 16) - 32768;
}

/* -------------------------------------------------------- parameters --- */
static gr_pset_t gr_ps[NPART];
static const uint8_t gr_kind[NPART] = {KIND_GRAIN, KIND_GRAIN, KIND_DRONE, KIND_PAD};
static inline int gr_ambient(uint32_t part) { return gr_kind[part] != KIND_GRAIN; }
/* the destinations an LFO of this part can pick from, and the names the screen shows for the destination cell */
static inline const uint8_t *gr_dest_of(uint32_t part) { return gr_ambient(part) ? GR_ALFO_DEST : GR_LFO_DEST; }
static const uint8_t *gr_dest_tab = GR_LFO_DEST;          /* what gr_info_fmt shows a destination from (set per part by the screens) */

static int gr_is_global(uint32_t p)
{
    return p == P_VOLUME || p == P_DLYTIME || p == P_DLYFB || p == P_DLYDAMP || p == P_REVDECAY || p == P_REVDAMP ||
           p == P_TEMPO || p == P_SEQGSWING || p == P_SEQCLICK || p == P_SEQRECSTART || p == P_SEQDELAY || p == P_SEQQUANT ||
           p == P_SEQSYNC || p == P_SEQOUT;
}

/* the real value of a norm (x mul) of any parameter description */
static int32_t gr_info_real(const gr_pinfo_t *i, uint32_t n)
{
    if (n > 65535u)
        n = 65535u;
    if (i->curve == GR_E) {
        uint32_t x = (uint32_t)(((uint64_t)n * (uint32_t)i->lg) >> 16);
        return (int32_t)(((uint64_t)(uint32_t)i->mn * exp2_q16(x)) >> 16);
    }
    return i->mn + (int32_t)(((uint32_t)(i->mx - i->mn) * n + 32767u) / 65535u);
}
static int32_t gr_norm_to_real(uint32_t p, uint32_t n) { return gr_info_real(&GR_PINFO[p], n); }
static inline uint32_t gr_info_index(const gr_pinfo_t *i, uint32_t n)
{
    return i->steps ? (n * i->steps + 32767u) / 65535u : 0u;
}
/* snaps a norm to its steps */
static int32_t gr_info_snap(const gr_pinfo_t *i, int32_t n)
{
    n = clampi(n, 0, 65535);
    if (i->steps)
        n = (int32_t)(((uint32_t)gr_info_index(i, (uint32_t)n) * 65535u + i->steps / 2u) / i->steps);
    return n;
}

static void gr_set_norm(uint32_t part, uint32_t p, int32_t n)
{
    gr_pset_t *s = &gr_ps[gr_is_global(p) ? 0 : part];
    n = gr_info_snap(&GR_PINFO[p], n);
    s->norm[p] = (uint16_t)n;
    s->real[p] = gr_norm_to_real(p, (uint32_t)n);
}

static inline const gr_pset_t *gr_store(uint32_t part, uint32_t p) { return &gr_ps[gr_is_global(p) ? 0 : part]; }
static inline uint32_t gr_norm(uint32_t part, uint32_t p) { return gr_store(part, p)->norm[p]; }
static inline int32_t gr_real(uint32_t part, uint32_t p) { return gr_store(part, p)->real[p]; }
/* index of a stepped parameter (a choice, the LFO destination, a channel) */
static inline uint32_t gr_index(uint32_t part, uint32_t p)
{
    return gr_info_index(&GR_PINFO[p], gr_norm(part, p));
}

/* the norm that gives `real` (x mul): the curves rise with the norm, so a bisection finds it */
static int32_t gr_norm_of_real(uint32_t p, int32_t real)
{
    int32_t lo = 0, hi = 65535, k;
    for (k = 0; k < 17; k++) {
        int32_t mid = (lo + hi) / 2;
        if (gr_norm_to_real(p, (uint32_t)mid) < real)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

static void gr_params_defaults(void)
{
    uint32_t part, p;
    for (part = 0; part < NPART; part++) {
        for (p = 0; p < P_COUNT; p++)
            gr_set_norm(part, p, GR_PINFO[p].def);
        gr_set_norm(part, P_MIDICH, (int32_t)(part * 65535u / 15u));          /* channels 1 to 4 */
        if (gr_kind[part] == KIND_DRONE) {                                      /* a low, slow, wide drone */
            gr_set_norm(part, P_AWAVE, gr_norm_of_real(P_AWAVE, 10));
            gr_set_norm(part, P_AHARM, gr_norm_of_real(P_AHARM, 40));
            gr_set_norm(part, P_TRANSPOSE, gr_norm_of_real(P_TRANSPOSE, -24 * 256));
            gr_set_norm(part, P_AATK, gr_norm_of_real(P_AATK, 3000 * 16));
            gr_set_norm(part, P_AREL, gr_norm_of_real(P_AREL, 8000 * 16));
            gr_set_norm(part, P_CUTOFF, gr_norm_of_real(P_CUTOFF, 3000 * 16));
            gr_set_norm(part, P_FTYPE, 65535 * 2 / 4);                           /* a low-pass */
            gr_set_norm(part, P_REVSEND, gr_norm_of_real(P_REVSEND, 450));
        } else if (gr_kind[part] == KIND_PAD) {                                 /* a pad: an octave down, a slow bloom */
            gr_set_norm(part, P_AWAVE, gr_norm_of_real(P_AWAVE, 8));
            gr_set_norm(part, P_AHARM, gr_norm_of_real(P_AHARM, 24));
            gr_set_norm(part, P_TRANSPOSE, gr_norm_of_real(P_TRANSPOSE, -12 * 256));
            gr_set_norm(part, P_AATK, gr_norm_of_real(P_AATK, 1500 * 16));
            gr_set_norm(part, P_AREL, gr_norm_of_real(P_AREL, 3500 * 16));
            gr_set_norm(part, P_CUTOFF, gr_norm_of_real(P_CUTOFF, 6000 * 16));
            gr_set_norm(part, P_FTYPE, 65535 * 2 / 4);
            gr_set_norm(part, P_REVSEND, gr_norm_of_real(P_REVSEND, 400));
        }
    }
}

/* a MIDI value 0..127 -> norm */
static inline int32_t gr_cc_norm(uint32_t v) { return (int32_t)(v * 65535u / 127u); }

/* ---------------------------------------------------------- display --- */
static void gr_cat(char *d, const char *s)
{
    uint32_t n = str_len(d);
    if (n < 23u)
        str_cpy(d + n, s, 24u - n);
}

/* v / div with `dec` decimals (0 .. 3), rounded; plus: a + for a positive value */
static void gr_fix(char *d, int32_t v, int32_t div, int dec, int plus)
{
    char t[12];
    int32_t scale = 1, k, q, ip, fp;
    for (k = 0; k < dec; k++)
        scale *= 10;
    q = v >= 0 ? (v * scale + div / 2) / div : -((-v * scale + div / 2) / div);
    d[0] = 0;
    if (q < 0)
        gr_cat(d, "-");
    else if (plus)
        gr_cat(d, "+");
    ip = (q < 0 ? -q : q) / scale;
    fp = (q < 0 ? -q : q) % scale;
    fmt_fix(t, ip, 0);
    gr_cat(d, t);
    if (dec) {
        char f[6];
        gr_cat(d, ".");
        for (k = dec - 1; k >= 0; k--) {
            f[k] = (char)('0' + fp % 10);
            fp /= 10;
        }
        f[dec] = 0;
        gr_cat(d, f);
    }
}

/* shows the norm n of a parameter description */
static void gr_info_fmt(char *d, const gr_pinfo_t *i, uint32_t n)
{
    int32_t v = gr_info_real(i, n), m = i->mul;
    uint32_t idx = gr_info_index(i, n);
    d[0] = 0;
    switch (i->unit) {
    case GR_U_HZ:
        if (v >= 1000 * m) {
            gr_fix(d, v, 1000 * m, 1, 0);
            gr_cat(d, " kHz");
        } else if (v >= 100 * m) {
            gr_fix(d, v, m, 0, 0);
            gr_cat(d, " Hz");
        } else if (v >= 10 * m) {
            gr_fix(d, v, m, 1, 0);
            gr_cat(d, " Hz");
        } else {
            gr_fix(d, v, m, 2, 0);
            gr_cat(d, " Hz");
        }
        break;
    case GR_U_BIPOLAR:
        gr_fix(d, v, m, 2, 1);
        break;
    case GR_U_CHOICE:
        str_cpy(d, i->labels[idx], 24);
        break;
    case GR_U_DEST: {
        uint32_t t = gr_dest_tab[idx];
        str_cpy(d, t == P_COUNT ? "Off" : GR_PINFO[t].name, 24);
        break;
    }
    case GR_U_CHAN:
        gr_fix(d, (int32_t)idx + 1, 1, 0, 0);
        break;
    case GR_U_PCT:
        gr_fix(d, v * 100, i->mx, 0, 0);
        gr_cat(d, "%");
        break;
    case GR_U_MS:
        if (v >= 1000 * m) {
            gr_fix(d, v, 1000 * m, 2, 0);
            gr_cat(d, " s");
        } else {
            gr_fix(d, v, m, 0, 0);
            gr_cat(d, " ms");
        }
        break;
    case GR_U_PERSEC:
        gr_fix(d, v, m, 1, 0);
        gr_cat(d, " /s");
        break;
    case GR_U_SPEED:
        gr_fix(d, v, m, 2, 1);
        gr_cat(d, "x");
        break;
    case GR_U_SEMI:
        gr_fix(d, v, m, 1, 1);
        gr_cat(d, " st");
        break;
    case GR_U_NUM:
        gr_fix(d, v, m, v % m == 0 ? 0 : 2, 0);
        break;
    case GR_U_PAN: {                                  /* C, L1 .. L63, R1 .. R63 */
        int32_t k = ((v - i->mx / 2) * 63 + (v >= i->mx / 2 ? i->mx / 4 : -(i->mx / 4))) / (i->mx / 2);
        if (k == 0)
            str_cpy(d, "C", 24);
        else {
            d[0] = k < 0 ? 'L' : 'R';
            fmt_fix(d + 1, k < 0 ? -k : k, 0);
        }
        break;
    }
    default:
        gr_fix(d, v, m, 2, 0);
        break;
    }
    if (i->sfx && i->sfx[0])
        gr_cat(d, i->sfx);
}

static void gr_fmt(char *d, uint32_t part, uint32_t p) { gr_info_fmt(d, &GR_PINFO[p], gr_norm(part, p)); }
