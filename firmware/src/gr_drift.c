/* SPDX-License-Identifier: GPL-3.0-only
 * amb-iguos: the scale, the DERIVA (a generative player in the scale) and the FREEZE.
 * Everything here runs in the audio code, once a block (dr_block, from audio.c), like the sequencer.
 *
 *  - The scale is a tonic (C .. B) and a mode (GR_MODE_DEG). The keys can snap to it (Teclas: En escala).
 *  - The Deriva walks the scale: every Ritmo seconds (more or less) it plays, with the probability Densidad, a note
 *    or a chord of stacked thirds of the scale on layer A, B or both, held for about Duración seconds. The walk moves
 *    by small steps within Rango octaves above Octava. It can hold the tonic (and its fifth) on the drone, and it
 *    makes the granular layers wander slowly (Evolución, Vel. evol.): position, size, density and the filter.
 *  - The Freeze holds everything: the notes that sound (a sustain on every layer), the grains' place in the sound
 *    (no scan, no wandering), the Deriva's notes (no new ones, none ends), and a long reverb tail. */

static volatile uint8_t dr_on;               /* PLAY */

#define BLK_PER_S 345                        /* 44100 / 128 */
#define DR_NOTES 12u
#define DR_NWAND 4u                          /* the wanderers of a layer */

static struct { uint8_t part, note; uint32_t left; } dr_n[DR_NOTES];    /* left: blocks, 0 = free */
static uint32_t dr_wait = 1u;
static int32_t dr_deg;                       /* the walk, in degrees of the scale from the tonic of Octava */
static uint8_t dr_drone[2], dr_drone_n;      /* what the drone holds */
static struct { uint32_t ph; int32_t from, to; } dr_w[NPART][DR_NWAND];
static int32_t dr_off[NPART][DR_NWAND];      /* Q16 norm offsets, read by gr_modulate */
/* what the screen draws: the last notes the Deriva played (a ring), and a clock in blocks */
#define DR_HIST 32u
static volatile struct { uint32_t t, len; uint8_t note, part; } dr_hist[DR_HIST];
static volatile uint32_t dr_hist_n, dr_clock;
/* what each wanderer moves: the granular layers, then the drone */
static const uint8_t DR_WP[DR_NWAND] = {P_POSITION, P_SIZE, P_DENSITY, P_CUTOFF};
static const uint8_t DR_WPA[DR_NWAND] = {P_AHARM, P_ABAL, P_CUTOFF, P_ADETUNE};
static const int32_t DR_WK[DR_NWAND] = {26000, 14000, 14000, 16000};    /* the reach of each, x1/65536 of the range */

/* ------------------------------------------------------------- scale --- */
static inline uint32_t sc_key(void) { return gr_index(0, P_KEY); }
static inline uint32_t sc_mode(void) { return gr_index(0, P_MODE); }

/* degree d (any integer) of the scale, above the tonic of octave `oct` (C4 = 60 is octave 4): a MIDI note */
static int32_t sc_note(int32_t d, int32_t oct)
{
    uint32_t m = sc_mode(), n = GR_MODE_N[m];
    int32_t o = d >= 0 ? d / (int32_t)n : -((-d + (int32_t)n - 1) / (int32_t)n), i = d - o * (int32_t)n;
    return 12 * (oct + 1) + (int32_t)sc_key() + 12 * o + GR_MODE_DEG[m][i];
}

/* the nearest note of the scale (a tie goes down) */
static int32_t sc_snap(int32_t note)
{
    uint32_t m = sc_mode(), n = GR_MODE_N[m], i;
    int32_t rel = ((note - (int32_t)sc_key()) % 12 + 12) % 12, best = 0, bd = 99;
    for (i = 0; i <= n; i++) {                       /* the degrees and the octave above */
        int32_t s = i < n ? GR_MODE_DEG[m][i] : 12, d = absi(s - rel);
        if (d < bd) {
            bd = d;
            best = s;
        }
    }
    return note - rel + best;
}

/* -------------------------------------------------------------- notes --- */
static void dr_note_off_all(void)
{
    uint32_t i;
    for (i = 0; i < DR_NOTES; i++)
        if (dr_n[i].left) {
            gr_note_off(dr_n[i].part, dr_n[i].note);
            dr_n[i].left = 0;
        }
}

static void dr_play(uint32_t part, int32_t note, uint32_t vel, uint32_t blocks)
{
    uint32_t i, k = DR_NOTES, oldest = 0, least = 0xFFFFFFFFu;
    if (note < 12 || note > 120)
        return;
    for (i = 0; i < DR_NOTES; i++) {
        if (!dr_n[i].left) {
            k = i;
            break;
        }
        if (dr_n[i].left < least) {
            least = dr_n[i].left;
            oldest = i;
        }
    }
    if (k == DR_NOTES) {                             /* all busy: the one closest to its end gives way */
        k = oldest;
        gr_note_off(dr_n[k].part, dr_n[k].note);
    }
    for (i = 0; i < DR_NOTES; i++)                  /* the same note already held: it only lasts longer */
        if (dr_n[i].left && dr_n[i].part == part && dr_n[i].note == (uint8_t)note) {
            dr_n[i].left = blocks;
            return;
        }
    dr_n[k].part = (uint8_t)part;
    dr_n[k].note = (uint8_t)note;
    dr_n[k].left = blocks ? blocks : 1u;
    {
        uint32_t h = dr_hist_n % DR_HIST;
        dr_hist[h].t = dr_clock;
        dr_hist[h].len = blocks;
        dr_hist[h].note = (uint8_t)note;
        dr_hist[h].part = (uint8_t)part;
        dr_hist_n++;
    }
    gr_note_on(part, (uint32_t)note, vel);
}

/* a random number 0 .. n-1 */
static inline uint32_t dr_rnd(uint32_t n) { return n ? (uint32_t)(((uint64_t)(gr_rand() >> 8) * n) >> 24) : 0u; }

/* a time in seconds x100 (Ritmo, Duración), scattered 0.6x .. 1.4x: blocks */
static uint32_t dr_blocks(int32_t s100)
{
    uint32_t b = (uint32_t)s100 * BLK_PER_S / 100u;
    return b * (60u + dr_rnd(81u)) / 100u + 1u;
}

static void dr_event(void)
{
    uint32_t n = GR_MODE_N[sc_mode()], top = (uint32_t)gr_real(0, P_DRANGE) * n, chord = (uint32_t)gr_real(0, P_DCHORD);
    uint32_t layers = gr_index(0, P_DLAYERS), part, i, vel, len;
    int32_t step, oct = gr_real(0, P_DOCT);
    static const int8_t STEPS[8] = {-2, -1, -1, 0, 1, 1, 2, 3};
    if (dr_rnd(1000u) >= (uint32_t)gr_real(0, P_DPROB))
        return;                                      /* a rest */
    step = STEPS[dr_rnd(8u)];
    if (dr_rnd(6u) == 0u)
        step *= 2;                                   /* now and then a leap */
    dr_deg += step;
    if (dr_deg < 0)
        dr_deg = -dr_deg;                            /* the walk bounces off the ends of the range */
    if (dr_deg >= (int32_t)top)
        dr_deg = 2 * ((int32_t)top - 1) - dr_deg;
    if (dr_deg < 0)
        dr_deg = 0;
    part = layers == 2u ? dr_rnd(2u) : layers;
    vel = 55u + dr_rnd(45u);
    len = dr_blocks(gr_real(0, P_DLEN));
    for (i = 0; i < chord; i++) {                    /* thirds of the scale above the walk */
        dr_play(part, sc_note(dr_deg + 2 * (int32_t)i, oct), vel - 8u * i, len + dr_rnd(BLK_PER_S));
        if (layers == 2u && chord > 2u && i == 1u)
            part ^= 1u;                              /* a big chord spreads over both layers */
    }
}

/* the drone holds the tonic (and the fifth) while the Deriva plays */
static void dr_drone_set(uint32_t want)
{
    uint8_t notes[2];
    uint32_t n = 0, i, same;
    if (want) {
        notes[n++] = (uint8_t)sc_note(0, 4);
        if (want == 2u)
            notes[n++] = (uint8_t)(sc_note(0, 4) + 7);
    }
    same = n == dr_drone_n;
    for (i = 0; same && i < n; i++)
        same = notes[i] == dr_drone[i];
    if (same)
        return;
    for (i = 0; i < dr_drone_n; i++)
        gr_note_off(NGPART, dr_drone[i]);
    for (i = 0; i < n; i++) {
        gr_note_on(NGPART, notes[i], 90u);
        dr_drone[i] = notes[i];
    }
    dr_drone_n = (uint8_t)n;
}

/* --------------------------------------------------------- wandering --- */
static void dr_wander(void)
{
    uint32_t part, k;
    int32_t hz = gr_real(0, P_DESPD), amount;            /* Hz x1000 */
    uint32_t inc = (uint32_t)(((uint64_t)hz * 4294967u) / BLK_PER_S);   /* a cycle of the wanderer: 2^32 */
    amount = gr_real(0, P_DEVOL);                        /* x1000 */
    amount += (gr_real(0, P_MACRO4) > 0 ? gr_real(0, P_MACRO4) : 0) / 2;   /* the MOVIMIENTO macro adds */
    if (!dr_on)
        amount = 0;
    for (part = 0; part <= NGPART; part++)
        for (k = 0; k < DR_NWAND; k++) {
            int32_t t, s, target;
            if (!gr_freeze) {
                uint32_t prev = dr_w[part][k].ph;
                dr_w[part][k].ph += inc + (inc >> 3) * k;    /* each at its own speed */
                if (dr_w[part][k].ph < prev) {
                    dr_w[part][k].from = dr_w[part][k].to;
                    dr_w[part][k].to = gr_rand_bi();
                }
            }
            t = (int32_t)(dr_w[part][k].ph >> 16);           /* 0..65535, smoothstep */
            s = mulq16(mulq16(t, t), 196608 - 2 * t);
            target = dr_w[part][k].from + mulq16(dr_w[part][k].to - dr_w[part][k].from, s);    /* Q15 */
            target = (int32_t)(((int64_t)target * DR_WK[k] / 32768) * amount / 1000);         /* Q16 norm */
            if (gr_freeze)
                continue;
            dr_off[part][k] += (target - dr_off[part][k]) / 64;                              /* glides when it stops */
        }
}

/* the Deriva's movement of a layer's parameter (gr_modulate) */
static int32_t dr_offset(uint32_t part, uint32_t p)
{
    uint32_t k;
    const uint8_t *w = gr_ambient(part) ? DR_WPA : DR_WP;
    if (part > NGPART)
        return 0;
    for (k = 0; k < DR_NWAND; k++)
        if (w[k] == p)
            return dr_off[part][k];
    return 0;
}

/* --------------------------------------------------------------- block --- */
static uint8_t dr_was_on;

static void dr_block(void)
{
    uint32_t i;
    dr_clock++;
    dr_wander();
    if (!dr_on) {
        if (dr_was_on) {
            dr_note_off_all();
            dr_drone_set(0);
            dr_was_on = 0;
        }
        return;
    }
    if (!dr_was_on) {                                /* started: the walk begins on the tonic */
        dr_was_on = 1;
        dr_deg = 0;
        dr_wait = 1u;
    }
    if (gr_freeze)
        return;                                      /* frozen: what sounds goes on sounding */
    for (i = 0; i < DR_NOTES; i++)
        if (dr_n[i].left && --dr_n[i].left == 0u)
            gr_note_off(dr_n[i].part, dr_n[i].note);
    dr_drone_set(gr_index(0, P_DDRONE));
    if (--dr_wait == 0u) {
        dr_wait = dr_blocks(gr_real(0, P_DRATE));
        dr_event();
    }
}

/* the FREEZE, from the main loop (gr_cmd) */
static void dr_freeze_set(uint32_t on)
{
    uint32_t part;
    gr_freeze = (uint8_t)(on != 0u);
    for (part = 0; part < NPART; part++)
        gr_sustain(part, on);
}
