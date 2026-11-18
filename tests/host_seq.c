/* SPDX-License-Identifier: GPL-3.0-only
 * Host test of the sequencer (gr_seq.c): timing, ties, ratchets, slides, swing, fills, locks, recording,
 * the count-in, MIDI clock in and MIDI out. The MIDI out packets are the probe: each is stamped with its block.
 *   cc -O1 -Ifirmware/src -Ibuild/gen tests/host_seq.c -o build/host_seq -lm && build/host_seq */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#define GR_POOL
static uint32_t fm1_ms;
static void gr_render(int32_t *, int32_t *);
static void fm1_wdt_feed(void) { int32_t l[128], r[128]; fm1_ms++; gr_render(l, r); }
#define GR_YIELD() ((void)0)
static unsigned str_len(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }
static void str_cpy(char *d, const char *s, unsigned max) { unsigned i = 0; while (s[i] && i + 1 < max) { d[i] = s[i]; i++; } d[i] = 0; }
static void fmt_fix(char *b, int32_t v, int decimals) { (void)b; (void)v; (void)decimals; }
#include "gr_core.h"
#include "gr_params.c"
#include "gr_filter.c"
#include "gr_fx.c"
#include "gr_engine.c"
#include "gr_gen.c"
#include "gr_amb.c"

static uint32_t blockno;
typedef struct { uint32_t block, pkt; } ev_t;
static ev_t evs[4096]; static uint32_t nev;
static void midi_out_event(uint32_t pkt) { if (nev < 4096) { evs[nev].block = blockno; evs[nev].pkt = pkt; nev++; } }
#include "gr_seq.c"
#define MQ 64u
static volatile uint32_t midi_in_q[MQ], mi_r, mi_w, midi_in_overflow;
static struct { uint32_t notes; } fm1_in;
#define RING_PUBLISH() ((void)0)
#include "gr_midi.c"

static int16_t smp_mem[NPART][SMP_FRAMES + 1];
static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static void run(uint32_t blocks)
{
    uint32_t i;
    for (i = 0; i < blocks; i++) { int32_t l[128], r[128]; seq_advance(); gr_render(l, r); seq_mix_click(l, r); blockno++; }
}
static void reset(void)
{
    seq_init(); nev = 0; blockno = 0;
    gr_params_defaults();
    gr_set_norm(0, P_SEQOUT, 65535 / 2);                   /* notes */
    gr_set_norm(0, P_SEQCLICK, 0);
    gr_set_norm(0, P_TEMPO, gr_norm_of_real(P_TEMPO, 120));/* 120 */
    gr_set_norm(0, P_SEQLEN, 65535 * 15 / 63);
    gr_set_norm(0, P_SEQDIV, 65535 * 14 / 16);              /* 1/16 */
    gr_set_norm(1, P_SEQLEN, 65535 * 15 / 63);
    gr_set_norm(1, P_SEQDIV, 65535 * 14 / 16);
    {   uint32_t p; for (p = 0; p < 2; p++) { memset(gr_pt[p].v, 0, sizeof gr_pt[p].v); gr_pt[p].idle = 1; seq_clear_track(p); } }
}
static void put(uint32_t part, uint32_t i, uint32_t note, uint32_t vel)
{
    seq_step_t s; step_blank(&s); s.time = STT_NOTE; s.notes = 0xFFFFFF00u | note; s.vel = (uint8_t)vel; seq_set(part, i, &s);
}
static uint32_t count_on(uint32_t note) { uint32_t i, n = 0; for (i = 0; i < nev; i++) if ((evs[i].pkt >> 8 & 0xF0) == 0x90 && (evs[i].pkt >> 16 & 127) == note) n++; return n; }
static int32_t first_block(uint32_t status, uint32_t note, uint32_t nth)
{
    uint32_t i, n = 0;
    for (i = 0; i < nev; i++) if ((evs[i].pkt >> 8 & 0xF0) == status && (evs[i].pkt >> 16 & 127) == note && n++ == nth) return (int32_t)evs[i].block;
    return -1;
}

int main(void)
{
    uint32_t part;
    gr_params_defaults(); gr_gen_defaults(); gr_engine_init();
    for (part = 0; part < NPART; part++) gr_pt[part].smp.data = smp_mem[part];
    gr_sample_begin(0); gr_sample_end(0, gr_gen_render(0, smp_mem[0]), GEN_RATE);

    /* 1: a step every 4: 125 ms steps at 120 BPM, a hit every 500 ms = 172.3 blocks; the pattern loops in 2 s */
    reset();
    put(0, 0, 60, 100); put(0, 4, 62, 100); put(0, 8, 64, 100); put(0, 12, 65, 100);
    seq.play_req = 1;
    run(1400);
    {
        int32_t a = first_block(0x90, 60, 0), b = first_block(0x90, 62, 0), c = first_block(0x90, 60, 1), d = first_block(0x80, 60, 0);
        printf("timing: C at block %d, D at %d, C again at %d, C off at %d\n", a, b, c, d);
        CHECK(a <= 1 && abs(b - 172) <= 1 && abs(c - 689) <= 1, "step timing");
        CHECK(d > a && abs(d - a - 34) <= 2, "gate 80%% of a step (34 blocks): off %d blocks after", d - a);
        CHECK(count_on(65) == 2, "two loops of the last note");
    }

    /* 2: ties, ratchet, slide, swing, nudge, fill */
    reset();
    {
        seq_step_t s;
        put(0, 0, 60, 100); step_blank(&s); s.time = STT_TIE; seq_set(0, 1, &s); seq_set(0, 2, &s);   /* a note of 3 steps */
        put(0, 4, 62, 100); seq_get(0, 4, &s); s.ratchet = 4; seq_set(0, 4, &s);
        put(0, 8, 64, 100); seq_get(0, 8, &s); s.fill = FILL_ONLY; seq_set(0, 8, &s);
        put(0, 12, 67, 100); seq_get(0, 12, &s); s.nudge = 16; seq_set(0, 12, &s);                    /* a quarter of a step late */
    }
    seq.play_req = 1;
    run(700);
    {
        int32_t on = first_block(0x90, 60, 0), off = first_block(0x80, 60, 0);
        printf("tie: on %d off %d (two ties + 80%% gate = %.0f blocks)\n", on, off, 2.8 * 43.07);
        CHECK(abs(off - on - 120) <= 2, "tied length");
        CHECK(count_on(62) == 4, "ratchet x4 gives %u hits", count_on(62));
        CHECK(count_on(64) == 0, "a fill-only step stays silent");
        CHECK(first_block(0x90, 67, 0) - 12 * 43 >= 9 && first_block(0x90, 67, 0) - 12 * 43 <= 13, "nudge: %d", first_block(0x90, 67, 0) - 12 * 43);
    }
    seq.fill_held = 1;
    nev = 0; run(700);
    CHECK(count_on(64) >= 1, "the fill step plays while a fill is held");
    seq.fill_held = 0;

    /* swing: odd steps are late by half a step at 100 % */
    reset();
    put(0, 0, 60, 100); put(0, 1, 61, 100);
    gr_set_norm(0, P_SEQSWING, 65535);
    seq.play_req = 1; run(100);
    printf("swing: step 1 at block %d (straight 43, swung 65)\n", first_block(0x90, 61, 0));
    CHECK(abs(first_block(0x90, 61, 0) - 65) <= 2, "swing");

    /* slide: the second note takes over the first's voice */
    reset();
    {
        seq_step_t s;
        put(0, 0, 60, 100); seq_get(0, 0, &s); s.slide = 1; seq_set(0, 0, &s); put(0, 1, 67, 100);
    }
    seq.play_req = 1; run(60);
    CHECK(count_on(60) == 1 && count_on(67) == 1, "slide notes");
    {
        uint32_t i, n = 0;
        for (i = 0; i < NNOTE; i++) if (gr_pt[0].v[i].active) n++;
        printf("slide: %u voice(s) active, glide %d\n", n, gr_pt[0].v[0].glide);
        CHECK(n == 1, "slide keeps one voice");
    }

    /* locks: the cutoff is locked on step 4 only */
    reset();
    put(0, 0, 60, 100); put(0, 4, 62, 100);
    seq_set_lock(0, 4, P_CUTOFF, 12345);
    seq.play_req = 1; run(176);
    printf("lock at step 4: engine lock %d\n", gr_pt[0].lock[P_CUTOFF]);
    CHECK(gr_pt[0].lock[P_CUTOFF] == 12345, "the lock applies on its step");
    run(45);                                             /* step 5 plays: no lock there */
    CHECK(gr_pt[0].lock[P_CUTOFF] == -1, "the lock goes at the next step");
    seq.stop_req = 1; run(2);
    CHECK(!seq.playing, "stop");

    /* gliding locks: from step 0 (10000) to step 8 (50000), then a plain lock on step 8 releases after its step */
    reset();
    put(0, 0, 60, 100);
    seq_set_lock_g(0, 0, P_CUTOFF, 10000, 1); seq_set_lock_g(0, 8, P_CUTOFF, 50000, 0);
    seq.play_req = 1; run(172);                                  /* step 4: halfway */
    printf("glide at step 4: %d (want ~30000)\n", gr_pt[0].lock[P_CUTOFF]);
    CHECK(abs(gr_pt[0].lock[P_CUTOFF] - 30000) < 900, "the lock glides: %d", gr_pt[0].lock[P_CUTOFF]);
    run(172 + 10);                                               /* just after step 8 */
    CHECK(abs(gr_pt[0].lock[P_CUTOFF] - 50000) < 600, "the glide arrives: %d", gr_pt[0].lock[P_CUTOFF]);
    seq.stop_req = 1; run(2);
    CHECK(gr_pt[0].lock[P_CUTOFF] == -1, "stop frees the locks");

    /* recording a knob: a gliding lock at the step playing */
    reset();
    put(0, 0, 60, 100);
    seq.rec_part = 0; seq.rec_req = 1; run(1); seq.play_req = 0;
    seq_start_recording(0); seq_start_playing(1); run(90);        /* in step 2 */
    seq_record_knob(0, P_CUTOFF, 4242);
    {
        int32_t k = seq_find_lock(0, 2, P_CUTOFF);
        CHECK(k >= 0 && lock_value(seq_tr[0].locks[k]) == 4242 && lock_glide(seq_tr[0].locks[k]), "a knob move is recorded as a glide lock");
    }
    seq.stop_req = 1; run(2);
    seq_record_knob(0, P_CUTOFF, 1000);
    CHECK(seq_find_lock(0, 2, P_CUTOFF) >= 0 && lock_value(seq_tr[0].locks[seq_find_lock(0, 2, P_CUTOFF)]) == 4242, "nothing is recorded when stopped");

    /* long steps and gates over 100 % */
    reset();
    gr_set_norm(0, P_SEQDIV, 65535 * 4 / 16);/* 1 bar */
    printf("cycle: %u tenths of a second (16 bars at 120 = 32.0 s)\n", seq_cycle_tenths(0));
    CHECK(seq_cycle_tenths(0) == 320, "cycle time");
    gr_set_norm(0, P_SEQDIV, 65535 * 14 / 16);
    gr_set_norm(0, P_SEQGATE, gr_norm_of_real(P_SEQGATE, 400));
    put(0, 0, 60, 100);
    seq.play_req = 1; run(300);
    {
        int32_t on = first_block(0x90, 60, 0), off = first_block(0x80, 60, 0);
        printf("gate 400%%: on %d off %d (4 steps = 172 blocks)\n", on, off);
        CHECK(abs(off - on - 172) <= 3, "a gate of four steps");
    }
    seq.stop_req = 1; run(2);

    /* key hold: a key let go keeps its note, a second press lets it go */
    reset();
    gr_kb_part = 0; gr_kb_hold = 0; gr_kb_oct = 0; kb_prev = 0; kb_latched = kb_skip = 0;
    gr_set_norm(0, P_LATCH, 65535);
    fm1_in.notes = 1u << 7; gr_keys_block(); run(20);
    CHECK(gr_pt[0].v[0].active && gr_pt[0].v[0].gate, "a key sounds");
    fm1_in.notes = 0; gr_keys_block(); run(20);
    CHECK(gr_pt[0].v[0].active && gr_pt[0].v[0].gate, "the key is held after letting go");
    fm1_in.notes = 1u << 7; gr_keys_block(); run(5);
    CHECK(!gr_pt[0].v[0].gate, "a second press lets it go");
    fm1_in.notes = 0; gr_keys_block(); run(5);
    CHECK(!gr_pt[0].v[0].gate && !kb_skip, "and its release does nothing");
    fm1_in.notes = 1u << 7; gr_keys_block(); fm1_in.notes = 0; gr_keys_block();
    gr_set_norm(0, P_LATCH, 0); gr_keys_block(); run(5);
    CHECK(!gr_pt[0].v[0].gate, "turning the setting off lets the keys go");

    /* recording: first note starts it, quantised to the grid, a held note makes ties */
    reset();
    seq.rec_part = 0; seq.rec_req = 1; run(1);
    CHECK(seq.armed && !seq.playing, "armed");
    seq_note_played(0, 64, 110, 1);
    CHECK(seq.recording && seq.playing, "the first note starts the loop");
    run(80);                                             /* a bit over 1.5 steps */
    seq_note_played(0, 64, 0, 0);
    {
        seq_step_t s; seq_get(0, 0, &s);
        printf("recorded: step 0 time %u note %u vel %u\n", s.time, sn_note(s.notes, 0), s.vel);
        CHECK(s.time == STT_NOTE && sn_note(s.notes, 0) == 64 && s.vel == 110, "recorded step");
        seq_get(0, 1, &s);
        CHECK(s.time == STT_TIE, "the held note makes a tie");
    }
    seq.stop_req = 1; run(2);
    CHECK(seq_swap_undo(0) && seq.armed == 0, "undo");
    {
        seq_step_t s; seq_get(0, 0, &s);
        CHECK(s.time == STT_REST, "undo empties the pass");
    }

    /* count-in */
    reset();
    gr_set_norm(0, P_SEQRECSTART, 32768);
    seq.rec_part = 0; seq.rec_req = 1; run(1); seq.play_req = 1; run(1);
    CHECK(seq.counting, "counting");
    run(2 * 172 + 10);
    CHECK(seq.counting && seq.count_beat < 4, "mid count-in beat %u", seq.count_beat);
    run(2 * 172 + 20);
    printf("count-in end: counting %u playing %u recording %u armed %u pos %d\n", seq.counting, seq.playing, seq.recording, seq.armed, seq.pos);
    CHECK(!seq.counting && seq.playing && seq.recording, "after four beats it records");

    /* REC + SEQ: recording starts after the delay (2 s = 689 blocks), and a transport key cancels it */
    reset();
    gr_set_norm(0, P_SEQDELAY, 65535 * 2 / 10);
    seq.rec_part = 0; seq.delay_req = 1; run(1);
    CHECK(seq.delaying && seq.delay_sec == 2 && !seq.playing, "delaying %u s", seq.delay_sec);
    run(400);
    CHECK(seq.delaying && seq.delay_sec == 1, "one second left: %u", seq.delay_sec);
    run(300);
    CHECK(!seq.delaying && seq.playing && seq.recording, "after 2 s it records: delaying %u playing %u rec %u", seq.delaying, seq.playing, seq.recording);
    seq.stop_req = 1; run(2);
    reset();
    gr_set_norm(0, P_SEQDELAY, 65535 * 2 / 10);
    seq.delay_req = 1; run(100); seq.rec_req = 1; run(1);
    CHECK(!seq.delaying && !seq.playing && !seq.armed, "a transport key cancels the delay");
    gr_set_norm(0, P_SEQDELAY, 0);
    seq.delay_req = 1; run(1);
    CHECK(seq.playing && seq.recording, "no delay: at once");
    seq.stop_req = 1; run(2);
    /* nothing is recorded unless REC was pressed: a key while playing leaves the pattern alone */
    reset();
    put(0, 0, 60, 100); seq.play_req = 1; run(5);
    seq_note_played(0, 70, 100, 1); seq_note_played(0, 70, 0, 0);
    { seq_step_t s; seq_get(0, 1, &s); seq_get(0, 0, &s); CHECK(!seq.recording && sn_note(s.notes, 0) == 60 && sn_count(s.notes) == 1, "playing a key does not record"); }
    seq.stop_req = 1; run(2);

    /* MIDI clock: follow 90 BPM pulses */
    reset();
    gr_set_norm(0, P_SEQSYNC, 65535);
    put(0, 0, 60, 100); put(0, 4, 62, 100);
    {
        uint32_t b, frames_per_pulse = 44100 * 60 / (24 * 90);       /* 1225 frames */
        uint32_t acc = 0, pulses = 0;
        seq_clock_event(0xFA);
        for (b = 0; b < 600; b++) {
            int32_t l[128], r[128];
            acc += 128;
            while (acc >= frames_per_pulse) { acc -= frames_per_pulse; seq_clock_event(0xF8); pulses++; }
            seq_advance(); gr_render(l, r); blockno++;
        }
        printf("clock: %u pulses, following %u, external bpm %u, position %d ticks (master %u)\n", pulses, seq.following, seq.ext_bpm, seq.pos, pulses * 64);
        CHECK(seq.following && abs((int)seq.ext_bpm - 90) <= 1, "bpm %u", seq.ext_bpm);
        CHECK(abs(seq.pos - (int32_t)(pulses * 64)) <= 64 * 2, "locked to the master");
        seq_clock_event(0xFC); run(1);
        CHECK(!seq.playing, "clock stop");
    }
    printf(fails ? "%d FAILURES\n" : "sequencer ok\n", fails);
    return fails != 0;
}
