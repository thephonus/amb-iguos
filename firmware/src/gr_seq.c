/* SPDX-License-Identifier: GPL-3.0-only
 * The step sequencer, after the Granular synth's Sequencer.h, in integer ticks (1536 to the beat): one pattern per
 * part of up to 64 steps, each a chord of up to 4 notes, a tie or a rest, with force, ratchet, slide, micro
 * timing (1/64 of a step), a fill condition and 24 parameter locks. All timing comes from one position, so
 * patterns of any length and division stay in phase. Swing, live recording, undo, a metronome, MIDI clock in
 * (the position follows the master) and MIDI out (notes, clock, transport).
 *
 * seq_advance() runs in the audio code once a block, before the grains are made; the screens edit the patterns
 * while they play (a step is two 32-bit words, written one after the other). Not ported: the sections, chain and
 * song of the Granular synth. */
#define SEQ_STEPS 64
#define SEQ_LOCKS 48
#define TPB 1536                                  /* ticks to the beat */
#define NUDGE_MIN (-32)
#define NUDGE_MAX 31
#define NO_NOTES 0xFFFFFFFFu
enum { STT_REST, STT_NOTE, STT_TIE };
enum { FILL_NORMAL, FILL_ONLY, FILL_NONE };

typedef struct {
    uint32_t notes;                               /* 4 notes, 0xFF = none */
    uint8_t time, vel, ratchet, fill, slide;
    int8_t nudge;
} seq_step_t;

typedef struct {
    uint32_t notes[SEQ_STEPS], meta[SEQ_STEPS], locks[SEQ_LOCKS];
} seq_track_t;

static seq_track_t seq_tr[NPART], seq_undo[NPART];
static uint8_t seq_has_glide[NPART];                   /* a part has a lock that glides */
static uint8_t seq_undo_ok[NPART];

/* ------------------------------------------------------- the steps --- */
static uint32_t step_meta(const seq_step_t *s)
{
    return (uint32_t)s->time | (uint32_t)(s->vel & 127u) << 2 | (uint32_t)((s->ratchet - 1u) & 3u) << 9 |
           (uint32_t)((s->nudge - NUDGE_MIN) & 127) << 11 | (uint32_t)s->fill << 18 | (uint32_t)s->slide << 20;
}
static void step_blank(seq_step_t *s)
{
    s->notes = NO_NOTES;
    s->time = STT_REST;
    s->vel = 100;
    s->ratchet = 1;
    s->fill = FILL_NORMAL;
    s->slide = 0;
    s->nudge = 0;
}
static void step_unpack(seq_step_t *s, uint32_t notes, uint32_t meta)
{
    uint32_t t = meta & 3u, f = (meta >> 18) & 3u;
    int32_t nu = (int32_t)((meta >> 11) & 127u) + NUDGE_MIN;
    s->notes = notes;
    s->time = (uint8_t)(t <= 2u ? t : STT_REST);
    s->vel = (uint8_t)((meta >> 2) & 127u);
    s->ratchet = (uint8_t)(((meta >> 9) & 3u) + 1u);
    s->nudge = (int8_t)(nu > NUDGE_MAX ? NUDGE_MAX : nu);
    s->fill = (uint8_t)(f <= 2u ? f : FILL_NORMAL);
    s->slide = (uint8_t)((meta >> 20) & 1u);
}
static uint32_t seq_empty_meta(void)
{
    seq_step_t s;
    step_blank(&s);
    return step_meta(&s);
}
static void seq_get(uint32_t part, uint32_t i, seq_step_t *s) { step_unpack(s, seq_tr[part].notes[i], seq_tr[part].meta[i]); }
static void seq_set(uint32_t part, uint32_t i, const seq_step_t *s)
{
    seq_tr[part].meta[i] = step_meta(s);
    seq_tr[part].notes[i] = s->notes;
}
static void seq_clear_step(uint32_t part, uint32_t i)
{
    seq_tr[part].notes[i] = NO_NOTES;
    seq_tr[part].meta[i] = seq_empty_meta();
}
static inline uint32_t sn_note(uint32_t notes, uint32_t i) { return (notes >> (i * 8u)) & 0xFFu; }
static uint32_t sn_count(uint32_t notes)
{
    uint32_t i, n = 0;
    for (i = 0; i < 4u; i++)
        n += sn_note(notes, i) != 0xFFu;
    return n;
}
static int sn_add(uint32_t *notes, uint32_t v)
{
    uint32_t i;
    for (i = 0; i < 4u; i++)
        if (sn_note(*notes, i) == v)
            return 1;
    for (i = 0; i < 4u; i++)
        if (sn_note(*notes, i) == 0xFFu) {
            *notes = (*notes & ~(0xFFu << (i * 8u))) | v << (i * 8u);
            return 1;
        }
    return 0;
}

/* the parameter locks: step | param << 8 | glide << 15 | value << 16, a free one has step 0xFF. A lock holds its value for
 * its step; a lock with GLIDE moves on from its step to the next lock of the same parameter (round the end of the
 * pattern too), in a straight line: slow sweeps over many bars. */
#define LOCK_FREE 0x000000FFu
static inline uint32_t lock_step(uint32_t w) { return w & 0xFFu; }
static inline uint32_t lock_param(uint32_t w) { return (w >> 8) & 0x7Fu; }
static inline uint32_t lock_glide(uint32_t w) { return (w >> 15) & 1u; }
static inline uint32_t lock_value(uint32_t w) { return w >> 16; }

static void seq_clear_track(uint32_t part)
{
    uint32_t i;
    for (i = 0; i < SEQ_STEPS; i++)
        seq_clear_step(part, i);
    for (i = 0; i < SEQ_LOCKS; i++)
        seq_tr[part].locks[i] = LOCK_FREE;
    seq_has_glide[part] = 0;
}
static int seq_find_lock(uint32_t part, uint32_t step, uint32_t param)
{
    uint32_t i;
    for (i = 0; i < SEQ_LOCKS; i++) {
        uint32_t w = seq_tr[part].locks[i];
        if (lock_step(w) == step && lock_param(w) == param)
            return (int)i;
    }
    return -1;
}
static int seq_step_has_lock(uint32_t part, uint32_t step)
{
    uint32_t i;
    for (i = 0; i < SEQ_LOCKS; i++)
        if (lock_step(seq_tr[part].locks[i]) == step)
            return 1;
    return 0;
}
static uint32_t seq_locks_used(uint32_t part)
{
    uint32_t i, n = 0;
    for (i = 0; i < SEQ_LOCKS; i++)
        n += seq_tr[part].locks[i] != LOCK_FREE;
    return n;
}
static void seq_count_glides(uint32_t part)
{
    uint32_t i, n = 0;
    for (i = 0; i < SEQ_LOCKS; i++)
        n += seq_tr[part].locks[i] != LOCK_FREE && lock_glide(seq_tr[part].locks[i]);
    seq_has_glide[part] = n != 0u;
}
/* glide: 0 off, 1 on, 2 as it is (a new lock: off) */
static int seq_set_lock_g(uint32_t part, uint32_t step, uint32_t param, int32_t norm, uint32_t glide)
{
    int slot = seq_find_lock(part, step, param);
    uint32_t i, g = glide == 2u ? (slot >= 0 ? lock_glide(seq_tr[part].locks[slot]) : 0u) : glide;
    for (i = 0; slot < 0 && i < SEQ_LOCKS; i++)
        if (seq_tr[part].locks[i] == LOCK_FREE)
            slot = (int)i;
    if (slot < 0)
        return 0;
    seq_tr[part].locks[slot] = step | param << 8 | g << 15 | (uint32_t)clampi(norm, 0, 65535) << 16;
    seq_count_glides(part);
    return 1;
}
static int seq_set_lock(uint32_t part, uint32_t step, uint32_t param, int32_t norm) { return seq_set_lock_g(part, step, param, norm, 2u); }
static void seq_clear_step_locks(uint32_t part, uint32_t step)
{
    uint32_t i;
    for (i = 0; i < SEQ_LOCKS; i++)
        if (lock_step(seq_tr[part].locks[i]) == step)
            seq_tr[part].locks[i] = LOCK_FREE;
    seq_count_glides(part);
}
static void seq_move_step_locks(uint32_t part, uint32_t from, uint32_t to)
{
    uint32_t i;
    for (i = 0; i < SEQ_LOCKS; i++) {
        uint32_t w = seq_tr[part].locks[i];
        if (lock_step(w) == from)
            seq_tr[part].locks[i] = (w & ~0xFFu) | to;
    }
}
/* what a lock may change: the part's own continuous settings (the LFO's destinations) */
static int seq_lockable(uint32_t part, uint32_t p)
{
    uint32_t i;
    if (gr_is_global(p))
        return 0;
    for (i = 1; i < D_COUNT; i++)
        if (gr_dest_of(part)[i] == p)
            return 1;
    return 0;
}
static const uint8_t SEQ_LOCKCH[] = {P_POSITION, P_SIZE, P_DENSITY, P_SPREAD, P_SCAN, P_JITTER, P_PITCHRND, P_PAN, P_REVERSE,
                                     P_TRANSPOSE, P_ATTACK, P_RELEASE, P_LEVEL, P_CUTOFF, P_RESO, P_DRIVE, P_DLYSEND, P_REVSEND};
static const uint8_t SEQ_ALOCKCH[] = {P_AHARM, P_ABAL, P_APITCH, P_ADETUNE, P_AMODRATE, P_AMODDEPTH, P_CUTOFF, P_RESO, P_LEVEL, P_APAN,
                                      P_L1RATE, P_L1DEPTH, P_REVSEND, P_DLYSEND};
/* the parameters a part's locks can pick from */
static inline uint32_t seq_nlockch(uint32_t part) { return gr_ambient(part) ? (uint32_t)sizeof SEQ_ALOCKCH : (uint32_t)sizeof SEQ_LOCKCH; }
static inline uint32_t seq_lockch(uint32_t part, uint32_t i) { return gr_ambient(part) ? SEQ_ALOCKCH[i % sizeof SEQ_ALOCKCH] : SEQ_LOCKCH[i % sizeof SEQ_LOCKCH]; }

static void seq_save_undo(uint32_t part)
{
    memcpy(&seq_undo[part], &seq_tr[part], sizeof seq_tr[part]);
    seq_undo_ok[part] = 1;
}
/* one level: undoing again brings the edit back */
static int seq_swap_undo(uint32_t part)
{
    static seq_track_t tmp;
    if (!seq_undo_ok[part])
        return 0;
    memcpy(&tmp, &seq_tr[part], sizeof tmp);
    memcpy(&seq_tr[part], &seq_undo[part], sizeof tmp);
    memcpy(&seq_undo[part], &tmp, sizeof tmp);
    return 1;
}
static void seq_init_data(void)
{
    uint32_t part;
    for (part = 0; part < NPART; part++) {
        seq_clear_track(part);
        seq_undo_ok[part] = 0;
    }
}

/* ------------------------------------------------------------ time --- */
#define SEQ_DIV_TICKS GR_DIV_TICKS
static inline int seq_triplet(uint32_t div) { return GR_DIV_TRIP[div]; }
static inline uint32_t seq_length(uint32_t part) { return (uint32_t)gr_real(part, P_SEQLEN); }
static inline uint32_t seq_division(uint32_t part) { return gr_index(part, P_SEQDIV); }
static inline int32_t seq_floor_div(int32_t a, int32_t b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

#define SEQ_PEND 24u
#define SEQ_HELD 8u
#define SEQ_GAPS 24u
typedef struct {
    uint8_t used, started, vel;
    int32_t on, off;
    uint32_t notes;
} seq_pend_t;

static struct {
    volatile uint8_t playing, recording, armed, counting, count_beat, fill_held, rec_part;
    volatile uint8_t play_req, rec_req, stop_req, delay_req;
    volatile uint8_t delaying, delay_sec;          /* REC + SEQ: recording starts after a few seconds */
    uint32_t delay_blocks;
    volatile int16_t play_step[NPART];
    volatile uint8_t beat_flash, following;
    volatile uint16_t ext_bpm;
    int32_t pos;                                  /* ticks */
    uint32_t frac;                                /* Q16 of a tick */
    uint32_t count_pos;                           /* the count-in, ticks Q16 */
    int32_t next_click;
    int32_t last_step[NPART], skip_step[NPART];
    int8_t last_div[NPART];
    uint8_t slide_from[NPART];
    seq_pend_t pend[NPART][SEQ_PEND];
    int32_t lk_active[NPART][P_COUNT];
    int32_t lk_want[P_COUNT];
    int32_t lk_step[NPART][P_COUNT];              /* what the locks of the step playing want, -1 none */
    uint8_t held_note[SEQ_HELD];
    int32_t held_start[SEQ_HELD];
    uint8_t clock_cmd;
    uint32_t clock_pulses, clock_seen, since_pulse, gaps[SEQ_GAPS], gap_i;
    uint32_t clock_out_phase;                     /* Q16 ticks */
    int32_t click_at;                             /* frame in this block, -1 none */
    uint8_t click_accent;
    int32_t click_pos, click_start;
    uint32_t click_ph;
    uint32_t adv;                                 /* ticks this block, Q16 */
    uint32_t bpm_q8;
} seq;

static inline int seq_following(void) { return gr_index(0, P_SEQSYNC) != 0u; }
static inline int seq_clock_alive(void) { return seq.clock_seen < FS / 2u; }

static void seq_midi_out(uint32_t part, uint32_t status, uint32_t d1, uint32_t d2)
{
    if (gr_index(0, P_SEQOUT) < 1u)
        return;
    midi_out_event((status >> 4) | (status | gr_index(part, P_MIDICH)) << 8 | d1 << 16 | d2 << 24);
}
static void seq_midi_rt(uint32_t status)
{
    if (gr_index(0, P_SEQOUT) >= 2u)
        midi_out_event(0xFu | status << 8);
}

/* a clock byte (F8, FA, FB, FC) from the MIDI ring */
static void seq_clock_event(uint32_t status)
{
    if (status == 0xF8u) {
        seq.clock_pulses++;
        seq.clock_seen = 0;
        seq.gaps[seq.gap_i++ % SEQ_GAPS] = seq.since_pulse;
        seq.since_pulse = 0;
    } else if (seq_following()) {
        if (status == 0xFAu) {
            seq.clock_cmd = 1;
            seq.clock_pulses = 0;
        } else if (status == 0xFBu) {
            seq.clock_cmd = 2;
        } else if (status == 0xFCu) {
            seq.clock_cmd = 3;
        }
    }
}

static uint32_t seq_bpm_q8(void)
{
    if (seq_following() && seq_clock_alive() && seq.gap_i >= SEQ_GAPS / 2u) {
        uint32_t n = seq.gap_i < SEQ_GAPS ? seq.gap_i : SEQ_GAPS, i, total = 0, bpm;
        for (i = 0; i < n; i++)
            total += seq.gaps[i];
        if (total) {
            bpm = 28224000u / (total / n ? total / n : 1u);                  /* FS x 60 x 256 / 24 / frames per pulse */
            seq.ext_bpm = (uint16_t)(bpm >> 8);
            seq.following = 1;
            return bpm < 2560u ? 2560u : bpm > 102400u ? 102400u : bpm;
        }
    }
    seq.following = 0;
    return (uint32_t)gr_real(0, P_TEMPO) << 8;
}

static void seq_reset_track(uint32_t part)
{
    uint32_t i;
    seq.last_step[part] = -1;
    seq.last_div[part] = -1;
    for (i = 0; i < SEQ_PEND; i++)
        seq.pend[part][i].used = 0;
    seq.slide_from[part] = 0xFF;
    seq.skip_step[part] = -1;
}

static void seq_send_notes(uint32_t part, uint32_t notes, uint32_t vel, int on, uint32_t offset)
{
    uint32_t i;
    for (i = 0; i < 4u; i++) {
        uint32_t n = sn_note(notes, i);
        if (n == 0xFFu)
            continue;
        if (on) {
            gr_seq_note_on(part, n, vel, offset);
            seq_midi_out(part, 0x90u, n, vel);
        } else {
            gr_note_off(part, n);
            seq_midi_out(part, 0x80u, n, 0);
        }
    }
}

static void seq_silence_track(uint32_t part)
{
    uint32_t i;
    for (i = 0; i < SEQ_PEND; i++) {
        seq_pend_t *p = &seq.pend[part][i];
        if (p->used && p->started)
            seq_send_notes(part, p->notes, 0, 0, 0);
        p->used = 0;
    }
    seq.slide_from[part] = 0xFF;
}

static void seq_start_recording(uint32_t part)
{
    uint32_t i;
    seq_save_undo(part);
    seq.armed = 0;
    seq.recording = 1;
    for (i = 0; i < SEQ_HELD; i++)
        seq.held_note[i] = 0xFF;
}

static void seq_finish_held(uint32_t part, int32_t start)
{
    uint32_t sl = SEQ_DIV_TICKS[seq_division(part)], length = seq_length(part);
    int32_t end = seq_floor_div(seq.pos + (int32_t)(sl / 2u), (int32_t)sl), k;
    for (k = start + 1; k < end && k < start + (int32_t)length; k++) {
        uint32_t index = (uint32_t)k % length;
        seq_step_t ex, tie;
        seq_get(part, index, &ex);
        if (ex.time == STT_NOTE)
            break;
        step_blank(&tie);
        tie.time = STT_TIE;
        seq_set(part, index, &tie);
    }
}
static void seq_end_held(uint32_t part)
{
    uint32_t i;
    for (i = 0; i < SEQ_HELD; i++) {
        if (seq.held_note[i] != 0xFF)
            seq_finish_held(part, seq.held_start[i]);
        seq.held_note[i] = 0xFF;
    }
}

static void seq_start_playing(int from_top)
{
    uint32_t part;
    if (seq.playing)
        return;
    if (from_top) {
        seq.pos = 0;
        seq.frac = 0;
    }
    for (part = 0; part < NPART; part++)
        seq_reset_track(part);
    seq.next_click = (seq.pos + TPB - 1) / TPB;
    seq_midi_rt(from_top ? 0xFAu : 0xFBu);
    seq.playing = 1;
}

static void seq_stop_playing(void)
{
    uint32_t part, was = seq.playing;
    seq.playing = 0;
    seq.counting = 0;
    seq.delaying = 0;
    seq.armed = 0;
    if (seq.recording) {
        seq.recording = 0;
        seq_end_held(seq.rec_part);
    }
    for (part = 0; part < NPART; part++) {
        uint32_t p;
        seq_silence_track(part);
        seq.play_step[part] = -1;
        gr_seq_set_locks(part, 0);
        for (p = 0; p < P_COUNT; p++)
            seq.lk_active[part][p] = seq.lk_step[part][p] = -1;
    }
    if (was && gr_index(0, P_SEQSTOP) == 1u)                        /* Cut: no release tail either */
        for (part = 0; part < NPART; part++)
            gr_all_sound_off(part);
    if (was)
        seq_midi_rt(0xFCu);
}

static void seq_requests(void)
{
    if (seq.clock_cmd == 1) {
        seq.pos = 0;
        seq.frac = 0;
        seq_start_playing(1);
    } else if (seq.clock_cmd == 2) {
        seq_start_playing(0);
    } else if (seq.clock_cmd == 3) {
        seq_stop_playing();
    }
    seq.clock_cmd = 0;
    if (seq.delaying && (seq.stop_req || seq.play_req || seq.rec_req || seq.delay_req)) {      /* any transport key cancels */
        seq.delaying = 0;
        seq.stop_req = seq.play_req = seq.rec_req = seq.delay_req = 0;
        seq.armed = 0;
    }
    if (seq.delay_req) {                          /* REC + SEQ: start recording from the top after the delay */
        uint32_t secs = (uint32_t)gr_real(0, P_SEQDELAY);
        seq.delay_req = 0;
        if (!seq.playing && !seq.counting) {
            if (secs == 0u) {
                seq_start_recording(seq.rec_part);
                seq_start_playing(1);
            } else {
                seq.delaying = 1;
                seq.armed = 0;
                seq.delay_blocks = secs * 34453u / 100u;              /* blocks of 128 frames in `secs` seconds */
                seq.delay_sec = (uint8_t)secs;
            }
        }
    }
    if (seq.stop_req) {
        seq.stop_req = 0;
        seq_stop_playing();
    }
    if (seq.play_req) {
        seq.play_req = 0;
        if (seq.counting) {
            seq.counting = 0;
            seq.armed = 0;
        } else if (seq.playing) {
            seq_stop_playing();
        } else if (seq.armed && gr_index(0, P_SEQRECSTART) == 1u) {
            seq.counting = 1;
            seq.count_pos = 0;
            seq.count_beat = 4;
            seq.next_click = 0;
        } else {
            if (seq.armed)
                seq_start_recording(seq.rec_part);
            seq_start_playing(1);
        }
    }
    if (seq.rec_req) {
        seq.rec_req = 0;
        if (seq.recording) {
            seq.recording = 0;
            seq_end_held(seq.rec_part);
        } else if (seq.playing) {
            seq_start_recording(seq.rec_part);
        } else {
            seq.armed = !seq.armed;
        }
    }
}

/* ------------------------------------------------------ triggering --- */
/* the value of a gliding parameter at a moment of the pattern, or -1 when no glide is running there */
static int32_t seq_glide_value(uint32_t part, uint32_t p, int32_t t, int32_t sl, int32_t cycle)
{
    int32_t cur = t / sl, pi = -1, ni = -1, i, t0, t1, v0, v1;
    uint32_t pstep = 0, nstep = 0;
    for (i = 0; i < (int32_t)SEQ_LOCKS; i++) {                        /* the last lock at or before now, and the first after */
        uint32_t w = seq_tr[part].locks[i], s = lock_step(w);
        if (w == LOCK_FREE || lock_param(w) != p)
            continue;
        if ((int32_t)s <= cur && (pi < 0 || s > pstep)) {
            pi = i;
            pstep = s;
        }
    }
    if (pi < 0)                                                  /* before the first lock: the last one carries over */
        for (i = 0; i < (int32_t)SEQ_LOCKS; i++) {
            uint32_t w = seq_tr[part].locks[i], s = lock_step(w);
            if (w != LOCK_FREE && lock_param(w) == p && (pi < 0 || s > pstep)) {
                pi = i;
                pstep = s;
            }
        }
    if (pi < 0 || !lock_glide(seq_tr[part].locks[pi]))
        return -1;
    for (i = 0; i < (int32_t)SEQ_LOCKS; i++) {
        uint32_t w = seq_tr[part].locks[i], s = lock_step(w);
        if (w == LOCK_FREE || lock_param(w) != p || s <= pstep)
            continue;
        if (ni < 0 || s < nstep) {
            ni = i;
            nstep = s;
        }
    }
    if (ni < 0)                                                  /* none later: round the end to the first one */
        for (i = 0; i < (int32_t)SEQ_LOCKS; i++) {
            uint32_t w = seq_tr[part].locks[i], s = lock_step(w);
            if (w != LOCK_FREE && lock_param(w) == p && (ni < 0 || s < nstep)) {
                ni = i;
                nstep = s;
            }
        }
    v0 = (int32_t)lock_value(seq_tr[part].locks[pi]);
    v1 = (int32_t)lock_value(seq_tr[part].locks[ni]);
    t0 = (int32_t)pstep * sl;
    t1 = (int32_t)nstep * sl;
    if (t1 <= t0)
        t1 += cycle;
    if (t < t0)
        t += cycle;
    if (t1 - t0 < 1 || ni == pi)
        return v0;
    {
        uint32_t span = (uint32_t)(t1 - t0), dt = (uint32_t)(t - t0);
        while (span > 0xFFFFu) {
            span >>= 1;
            dt >>= 1;
        }
        if (dt > span)
            dt = span;
        return v0 + (int32_t)(((int64_t)(v1 - v0) * (int32_t)((dt << 16) / span)) >> 16);
    }
}

/* hand the engine what the part's locks want now: the step's own, and the gliding ones from where the pattern is */
static void seq_push_locks(uint32_t part)
{
    uint32_t i, p, different = 0;
    int any = 0;
    int32_t sl = SEQ_DIV_TICKS[seq_division(part)], cycle;
    uint32_t length = seq_length(part), done_lo = 0, done_hi = 0;
    if (length < 1u)
        length = 1u;
    cycle = (int32_t)length * sl;
    for (p = 0; p < P_COUNT; p++)
        seq.lk_want[p] = seq.lk_step[part][p];
    if (seq_has_glide[part] && seq.pos >= 0)
        for (i = 0; i < SEQ_LOCKS; i++) {
            uint32_t w = seq_tr[part].locks[i], gp = lock_param(w);
            uint32_t bit = gp < 32u ? 1u << gp : 1u << (gp - 32u);
            if (w == LOCK_FREE || !lock_glide(w) || gp >= P_COUNT || !seq_lockable(part, gp))
                continue;
            if ((gp < 32u ? done_lo : done_hi) & bit)
                continue;
            if (gp < 32u)
                done_lo |= bit;
            else
                done_hi |= bit;
            {
                int32_t v = seq_glide_value(part, gp, seq.pos % cycle, sl, cycle);
                if (v >= 0)
                    seq.lk_want[gp] = v;
            }
        }
    for (p = 0; p < P_COUNT; p++) {
        different |= (uint32_t)(seq.lk_want[p] != seq.lk_active[part][p]);
        any |= seq.lk_want[p] >= 0;
    }
    if (!different)
        return;
    for (p = 0; p < P_COUNT; p++)
        seq.lk_active[part][p] = seq.lk_want[p];
    gr_seq_set_locks(part, any ? seq.lk_want : 0);
}

static void seq_apply_locks(uint32_t part, uint32_t index)
{
    uint32_t i, p;
    for (p = 0; p < P_COUNT; p++)
        seq.lk_step[part][p] = -1;
    for (i = 0; i < SEQ_LOCKS; i++) {
        uint32_t w = seq_tr[part].locks[i];
        if (lock_step(w) == index && lock_param(w) < P_COUNT && seq_lockable(part, lock_param(w)))
            seq.lk_step[part][lock_param(w)] = (int32_t)lock_value(w);
    }
    seq_push_locks(part);
}

/* turning a knob while recording: a gliding lock at the step playing, so the move is replayed as a sweep */
static void seq_record_knob(uint32_t part, uint32_t p, int32_t norm)
{
    int32_t step;
    if (!seq.recording || !seq.playing || part != seq.rec_part || p >= P_COUNT || !seq_lockable(part, p))
        return;
    step = seq.play_step[part];
    if (step < 0)
        return;
    seq_set_lock_g(part, (uint32_t)step, p, norm, 1u);
}

/* the whole pattern round, in seconds (tenths for the screens that want them) */
static uint32_t seq_cycle_tenths(uint32_t part)
{
    uint32_t length = seq_length(part), bpm = seq_bpm_q8();
    if (length < 1u)
        length = 1u;
    return bpm ? length * SEQ_DIV_TICKS[seq_division(part)] * 100u / bpm : 0u;     /* ticks x 60 x 256 / (1536 x bpm_q8), x10 */
}

static void seq_add_pending(uint32_t part, int32_t on, int32_t off, uint32_t notes, uint32_t vel, int started)
{
    uint32_t i;
    for (i = 0; i < SEQ_PEND; i++) {
        seq_pend_t *p = &seq.pend[part][i];
        if (!p->used) {
            p->used = 1;
            p->started = (uint8_t)started;
            p->on = on;
            p->off = off;
            p->notes = notes;
            p->vel = (uint8_t)vel;
            return;
        }
    }
}

static void seq_trigger(uint32_t part, int32_t k, uint32_t index, uint32_t length, const seq_step_t *step, int32_t when,
                        int32_t sl)
{
    int fill = seq.fill_held, ties = 0, can_slide;
    uint32_t prev, gate = (uint32_t)gr_real(part, P_SEQGATE);
    seq.play_step[part] = (int16_t)index;
    if ((step->fill == FILL_ONLY && !fill) || (step->fill == FILL_NONE && fill))
        return;                                               /* a silent whole: no note, no ratchet and no lock */
    seq_apply_locks(part, index);
    if (k == seq.skip_step[part])
        return;                                               /* just recorded, and heard as it was played */
    prev = seq.slide_from[part];
    seq.slide_from[part] = 0xFF;
    if (step->time == STT_TIE) {
        if (prev != 0xFFu)
            seq.slide_from[part] = (uint8_t)prev;
        return;
    }
    if (step->time != STT_NOTE || sn_count(step->notes) == 0)
        return;
    for (;;) {
        seq_step_t nx;
        if (ties >= (int)length - 1)
            break;
        seq_get(part, (index + (uint32_t)ties + 1u) % length, &nx);
        if (nx.time != STT_TIE)
            break;
        ties++;
    }
    can_slide = prev != 0xFFu && sn_count(step->notes) == 1;
    if (can_slide) {
        /* glide from the note still sounding: it takes over that note's voice */
        uint32_t i, blocks = (uint32_t)(sl / 2) * 65536u / (seq.adv ? seq.adv : 1u);
        for (i = 0; i < SEQ_PEND; i++) {
            seq_pend_t *p = &seq.pend[part][i];
            if (p->used && p->started && p->notes == (0xFFFFFF00u | prev))
                p->used = 0;
        }
        if (sn_note(step->notes, 0) != prev)
            gr_seq_slide(part, prev, sn_note(step->notes, 0), step->vel, blocks);
        seq_midi_out(part, 0x80u, prev, 0);
        seq_midi_out(part, 0x90u, sn_note(step->notes, 0), step->vel);
        seq_add_pending(part, when, when + ties * sl + (step->slide ? sl * 5 / 4 : sl * (int32_t)gate / 100), step->notes, step->vel, 1);
    } else if (step->ratchet > 1u && ties == 0) {
        int32_t hit = sl / step->ratchet, r;
        uint32_t g = gate < 90u ? gate : 90u;
        for (r = 0; r < (int32_t)step->ratchet; r++) {
            int32_t on = when + r * hit;
            seq_add_pending(part, on, on + hit * (int32_t)g / 100, step->notes, step->vel, 0);
        }
    } else {
        seq_add_pending(part, when, when + ties * sl + (step->slide ? sl * 5 / 4 : sl * (int32_t)gate / 100), step->notes, step->vel, 0);
    }
    if (step->slide && sn_count(step->notes) == 1)
        seq.slide_from[part] = (uint8_t)sn_note(step->notes, 0);
}

static int32_t seq_swing_offset(uint32_t part, int32_t k, int32_t sl, uint32_t div)
{
    int32_t s;
    if (!(k & 1) || seq_triplet(div))
        return 0;
    s = gr_real(part, P_SEQSWING) + gr_real(0, P_SEQGSWING);
    if (s > 100)
        s = 100;
    return s * sl / 200;
}

static void seq_run_track(uint32_t part, int32_t from, int32_t to)
{
    uint32_t length = seq_length(part), div = seq_division(part);
    int32_t sl = SEQ_DIV_TICKS[div], first, last, k;
    if (length < 1u)
        length = 1u;
    if ((int32_t)div != seq.last_div[part]) {
        seq.last_div[part] = (int8_t)div;
        seq.last_step[part] = seq_floor_div(from, sl) - (from > 0 ? 0 : 1);
    }
    first = seq_floor_div(from, sl);
    last = seq_floor_div(to, sl) + 1;
    for (k = first; k <= last; k++) {
        seq_step_t st;
        uint32_t index;
        int32_t when;
        if (k <= seq.last_step[part] || k < 0)
            continue;
        index = (uint32_t)k % length;
        seq_get(part, index, &st);
        when = k * sl + seq_swing_offset(part, k, sl, div) + (int32_t)st.nudge * sl / 64;
        if (when < 0)
            when = 0;
        if (when >= to)
            break;
        seq.last_step[part] = k;
        seq_trigger(part, k, index, length, &st, when < from ? from : when, sl);
    }
}

/* notes whose time has come: on (at its frame in the block) and off */
static void seq_fire_pending(uint32_t part, int32_t from, int32_t to)
{
    uint32_t i;
    for (i = 0; i < SEQ_PEND; i++) {
        seq_pend_t *p = &seq.pend[part][i];
        if (!p->used)
            continue;
        if (!p->started && p->on < to) {
            int32_t dt = ((p->on - from) << 16) - (int32_t)seq.frac;
            uint32_t frames = dt <= 0 ? 0u : (uint32_t)dt * BLK / (seq.adv ? seq.adv : 1u);
            p->started = 1;
            seq_send_notes(part, p->notes, p->vel, 1, frames);
        }
        if (p->started && p->off < to) {
            seq_send_notes(part, p->notes, 0, 0, 0);
            p->used = 0;
        }
    }
}

/* ------------------------------------------------------- recording --- */
/* a note played on a key or MIDI in */
static void seq_note_played(uint32_t part, uint32_t note, uint32_t vel, int on)
{
    uint32_t i;
    if (on && part == seq.rec_part && seq.armed && gr_index(0, P_SEQRECSTART) == 0u) {
        seq_start_recording(part);                               /* "first note": it starts the loop */
        seq_start_playing(1);
    }
    if (!seq.recording || part != seq.rec_part)
        return;
    if (on) {
        uint32_t sl = SEQ_DIV_TICKS[seq_division(part)], length = seq_length(part);
        int32_t beat = seq.pos - (int32_t)((37u * seq.bpm_q8) / (120u * 256u)), k, index;       /* the latency of a key */
        seq_step_t st;
        if (beat < 0)
            beat = 0;
        k = seq_floor_div(beat + (int32_t)(sl / 2u), (int32_t)sl);
        index = (int32_t)((uint32_t)k % length);
        seq_get(part, (uint32_t)index, &st);
        if (st.time != STT_NOTE) {
            step_blank(&st);
            st.time = STT_NOTE;
        }
        if (!sn_add(&st.notes, note))                            /* the chord is full: the last note gives way */
            st.notes = (st.notes & 0x00FFFFFFu) | note << 24;
        st.vel = (uint8_t)vel;
        if (!gr_index(0, P_SEQQUANT)) {
            int32_t off = (beat - k * (int32_t)sl) * 64 / (int32_t)sl;
            st.nudge = (int8_t)clampi(off, NUDGE_MIN, NUDGE_MAX);
        }
        seq_set(part, (uint32_t)index, &st);
        if (k > seq.last_step[part])
            seq.skip_step[part] = k;                             /* it hasn't played yet this pass, and it sounds */
        for (i = 0; i < SEQ_HELD; i++)
            if (seq.held_note[i] == 0xFF) {
                seq.held_note[i] = (uint8_t)note;
                seq.held_start[i] = k;
                break;
            }
    } else {
        for (i = 0; i < SEQ_HELD; i++)
            if (seq.held_note[i] == note) {
                seq_finish_held(part, seq.held_start[i]);
                seq.held_note[i] = 0xFF;
            }
    }
}

/* -------------------------------------------------------- the block --- */
static void seq_advance(void)
{
    uint32_t part;
    int32_t from, to;
    uint32_t nf;
    seq.since_pulse += BLK;
    if (seq.clock_seen < 0xFFFFFFu)
        seq.clock_seen += BLK;
    seq_requests();
    seq.bpm_q8 = seq_bpm_q8();
    seq.adv = (uint32_t)(((uint64_t)seq.bpm_q8 * 4870u) >> 8);       /* ticks a block, Q16: bpm x 1536 x 128 / (60 x 44100) */
    seq.click_at = -1;
    if (gr_index(0, P_SEQOUT) >= 2u) {                               /* MIDI clock out */
        seq.clock_out_phase += seq.adv;
        while (seq.clock_out_phase >= (uint32_t)(TPB / 24) << 16) {
            seq.clock_out_phase -= (uint32_t)(TPB / 24) << 16;
            midi_out_event(0xFu | 0xF8u << 8);
        }
    }
    if (seq.delaying) {                                              /* waiting to record */
        if (seq.delay_blocks > 1u)
            seq.delay_blocks--;
        else {
            seq.delaying = 0;
            seq_start_recording(seq.rec_part);
            seq_start_playing(1);
            return;
        }
        seq.delay_sec = (uint8_t)((seq.delay_blocks * 100u + 34452u) / 34453u);       /* whole seconds left, rounded up */
        return;
    }
    if (seq.counting) {                                              /* the count-in: four clicks */
        uint32_t end = seq.count_pos + seq.adv;
        while (seq.next_click < 4 && (uint32_t)seq.next_click * TPB < end >> 16) {
            seq.click_at = 0;
            seq.click_accent = seq.next_click == 0;
            seq.count_beat = (uint8_t)(4 - seq.next_click);
            seq.next_click++;
        }
        seq.count_pos = end;
        if (seq.count_pos >= (uint32_t)(4 * TPB) << 16) {
            seq.counting = 0;
            seq_start_recording(seq.rec_part);
            seq_start_playing(1);
        }
        return;
    }
    if (!seq.playing) {
        seq.beat_flash = 0;
        return;
    }
    if (seq.following && seq_clock_alive()) {                        /* never run ahead of the master by more than a pulse */
        int32_t master = (int32_t)(seq.clock_pulses * (TPB / 24u));
        if (master - seq.pos > TPB / 24) {
            seq.pos = master;
            seq.frac = 0;
        }
    }
    from = seq.pos;
    nf = seq.frac + seq.adv;
    to = from + (int32_t)(nf >> 16);
    if (seq.following && seq_clock_alive()) {
        int32_t limit = (int32_t)((seq.clock_pulses + 1u) * (TPB / 24u));
        if (to > limit) {
            to = limit > from ? limit : from;
            nf = (nf & 0xFFFFu);
        }
    }
    {   /* the metronome: a click on each beat */
        uint32_t mode = gr_index(0, P_SEQCLICK);
        int on = mode == 2u || (mode == 1u && seq.recording);
        while (seq.next_click * TPB < to) {
            if (on && seq.next_click * TPB >= from) {
                int32_t dt = ((seq.next_click * TPB - from) << 16) - (int32_t)seq.frac;
                seq.click_at = dt <= 0 ? 0 : (int32_t)((uint32_t)dt * BLK / (seq.adv ? seq.adv : 1u));
                seq.click_accent = seq.next_click % 4 == 0;
            }
            seq.next_click++;
        }
    }
    for (part = 0; part < NPART; part++) {
        seq_run_track(part, from, to);
        seq_fire_pending(part, from, to);
    }
    seq.pos = to;
    for (part = 0; part < NPART; part++)
        if (seq_has_glide[part])
            seq_push_locks(part);
    seq.frac = nf & 0xFFFFu;
    seq.beat_flash = (uint8_t)((uint32_t)seq.pos % TPB < TPB / 8u);
}

/* the metronome's blip, added to the block (after the effects) */
static void seq_mix_click(int32_t *l, int32_t *r)
{
    int32_t i;
    if (seq.click_at >= 0) {
        seq.click_pos = 0;
        seq.click_start = seq.click_at;
        seq.click_ph = 0;
    }
    if (seq.click_pos < 0)
        return;
    for (i = seq.click_start; i < (int32_t)BLK; i++) {
        int32_t t, env, s;
        if (seq.click_pos >= 1400) {
            seq.click_pos = -1;
            return;
        }
        t = seq.click_pos * 23;                                       /* 1400 -> ~32768 */
        env = 32768 - t;
        env = mulq15(env, env);
        seq.click_ph += seq.click_accent ? 194780u * 2u : 194780u;    /* 2000 / 1000 Hz */
        s = mulq15(mulq15(sin32(seq.click_ph), env), 7209);           /* 0.22 */
        l[i] += s;
        r[i] += s;
        seq.click_pos++;
    }
    seq.click_start = 0;
}

static void seq_init(void)
{
    uint32_t part, p;
    seq_init_data();
    memset(&seq, 0, sizeof seq);
    seq.click_pos = -1;
    seq.rec_part = 0;
    seq.clock_seen = FS;
    for (part = 0; part < NPART; part++) {
        seq.play_step[part] = -1;
        seq.slide_from[part] = 0xFF;
        for (p = 0; p < P_COUNT; p++)
            seq.lk_active[part][p] = -1;
    }
}
