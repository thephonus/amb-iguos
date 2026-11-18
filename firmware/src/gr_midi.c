/* SPDX-License-Identifier: GPL-3.0-only
 * MIDI in (USB and the TRS jack share one ring, usb.c midi_in_q) and the keys, drained by the audio code once a
 * block. Both parts follow their own MIDI channel (a part's MIDI ch parameter); two parts on one channel play as
 * a layer. Note 60 plays a sample at its original pitch. CCs set the parameters that have one (gr_params, the
 * table of the README); CC 64 is sustain, CC 120 all sound off, CC 123 all notes off. */
static uint8_t gr_cc_param[128];            /* CC -> parameter, 255 = none (the granular parts) */
static uint8_t gr_cc_param_a[128];          /* the same for the drone and the pad */
static volatile uint32_t gr_midi_activity;  /* the screen's MIDI mark: counts messages that reached a part */

static void gr_midi_init(void)
{
    uint32_t i;
    for (i = 0; i < 128u; i++)
        gr_cc_param[i] = gr_cc_param_a[i] = 255;
    for (i = 0; i < GR_ACC_N; i++)
        gr_cc_param_a[GR_ACC[i].cc] = GR_ACC[i].param;
    for (i = 0; i < P_COUNT; i++)
        if (GR_PINFO[i].cc < 128u)
            gr_cc_param[GR_PINFO[i].cc] = (uint8_t)i;
}

static void gr_midi_part(uint32_t part, uint32_t st, uint32_t d1, uint32_t d2)
{
    switch (st) {
    case 0x90u:
        if (d2) {
            gr_note_on(part, d1, d2);
            seq_note_played(part, d1, d2, 1);
            break;
        }
        gr_note_off(part, d1);                  /* note on with velocity 0 */
        seq_note_played(part, d1, 0, 0);
        break;
    case 0x80u:
        gr_note_off(part, d1);
        seq_note_played(part, d1, 0, 0);
        break;
    case 0xE0u:
        gr_bend(part, (int32_t)((d2 << 7) | d1) - 8192);
        break;
    case 0xB0u:
        if (d1 == 64u)
            gr_sustain(part, d2 >= 64u);
        else if (d1 == 120u)
            gr_all_sound_off(part);
        else if (d1 == 123u)
            gr_all_notes_off(part);
        else if (d1 < 128u && (gr_ambient(part) ? gr_cc_param_a[d1] : gr_cc_param[d1]) != 255)
            gr_set_norm(part, gr_ambient(part) ? gr_cc_param_a[d1] : gr_cc_param[d1], gr_cc_norm(d2));
        break;
    default:
        return;
    }
    gr_midi_activity++;
}

static void gr_midi_event(uint32_t st, uint32_t ch, uint32_t d1, uint32_t d2)
{
    uint32_t part;
    for (part = 0; part < NPART; part++)
        if (gr_index(part, P_MIDICH) == ch)
            gr_midi_part(part, st, d1, d2);
}

/* ------------------------------------------------------------- keys --- */
#define KB_KEYS 27u                          /* F3 .. G5 */
#define KB_FIRST 53                          /* MIDI note of the first key */
static volatile uint8_t gr_kb_part;          /* the part the keys play (the screen's part) */
static volatile int8_t gr_kb_oct;            /* octaves up or down (OCT- / OCT+) */
static volatile uint8_t gr_kb_hold;          /* set from power-on to the end of the splash: the keys sound nothing */
static uint32_t kb_prev;
static uint8_t kb_part_of[KB_KEYS], kb_note_of[KB_KEYS];

/* Key hold (the Latch setting of a part): letting go of a key leaves its note sounding, a second press of the key
 * lets it go. For drones: play a chord, take the hands away. The sequencer sees the key as released. */
static uint32_t kb_latched, kb_skip;         /* keys holding a note, keys whose next release is the second press's */

static void gr_keys_block(void)
{
    uint32_t cur = fm1_in.notes, ch = cur ^ kb_prev, k;
    if (gr_kb_hold) {
        kb_prev = cur;
        return;
    }
    for (k = 0; kb_latched && k < KB_KEYS; k++)                   /* the setting was turned off: let them go */
        if (((kb_latched >> k) & 1u) && !gr_index(kb_part_of[k], P_LATCH)) {
            gr_note_off(kb_part_of[k], kb_note_of[k]);
            kb_latched &= ~(1u << k);
        }
    for (k = 0; ch && k < KB_KEYS; k++, ch >>= 1) {
        if (!(ch & 1u))
            continue;
        if ((cur >> k) & 1u) {
            int32_t note = KB_FIRST + (int32_t)k + 12 * gr_kb_oct;
            if ((kb_latched >> k) & 1u) {                         /* pressed again: let it go */
                gr_note_off(kb_part_of[k], kb_note_of[k]);
                kb_latched &= ~(1u << k);
                kb_skip |= 1u << k;
                continue;
            }
            if (gr_index(0, P_KEYSNAP))
                note = sc_snap(note);                             /* amb-iguos: the keys in the scale */
            if (note < 0 || note > 127)
                continue;
            kb_part_of[k] = gr_kb_part;
            kb_note_of[k] = (uint8_t)note;
            gr_note_on(gr_kb_part, (uint32_t)note, 100u);
            seq_note_played(gr_kb_part, (uint32_t)note, 100u, 1);
            gr_midi_activity++;
        } else {
            if ((kb_skip >> k) & 1u) {
                kb_skip &= ~(1u << k);
                continue;
            }
            if (gr_index(kb_part_of[k], P_LATCH))
                kb_latched |= 1u << k;                            /* the note stays */
            else
                gr_note_off(kb_part_of[k], kb_note_of[k]);
            seq_note_played(kb_part_of[k], kb_note_of[k], 0, 0);
        }
    }
    kb_prev = cur;
}

/* the MIDI ring: a message at a time */
static void gr_midi_block(void)
{
    if (midi_in_overflow) {                 /* a broken stream: let go of every note */
        uint32_t part;
        mi_r = mi_w;
        RING_PUBLISH();
        for (part = 0; part < NPART; part++)
            gr_all_notes_off(part);
        midi_in_overflow = 0;
    }
    while (mi_r != mi_w) {
        uint32_t at = mi_r % MQ, pkt = midi_in_q[at], status = (pkt >> 8) & 0xFFu;
        uint32_t d1 = (pkt >> 16) & 0x7Fu, d2 = (pkt >> 24) & 0x7Fu;
        mi_r++;
        if (status >= 0xF8u) {
            seq_clock_event(status);        /* clock and transport: the sequencer's */
            continue;
        }
        gr_midi_event(status & 0xF0u, status & 0x0Fu, d1, d2);
    }
}

/* ---------------------------------------- main loop -> audio commands --- */
/* The main loop never touches the notes: it queues what it wants (a test note, a panic) and the audio code does it. */
enum { CMD_NOTE_ON, CMD_NOTE_OFF, CMD_ALL_OFF, CMD_FREEZE };
#define CMDQ 16u
static struct { uint8_t type, part, a, b; } gr_cmdq[CMDQ];
static volatile uint32_t cmd_w, cmd_r;

static void gr_cmd(uint32_t type, uint32_t part, uint32_t a, uint32_t b)
{
    if (cmd_w - cmd_r >= CMDQ)
        return;
    gr_cmdq[cmd_w % CMDQ].type = (uint8_t)type;
    gr_cmdq[cmd_w % CMDQ].part = (uint8_t)part;
    gr_cmdq[cmd_w % CMDQ].a = (uint8_t)a;
    gr_cmdq[cmd_w % CMDQ].b = (uint8_t)b;
    RING_PUBLISH();
    cmd_w++;
}

static void gr_cmd_block(void)
{
    while (cmd_r != cmd_w) {
        uint32_t at = cmd_r % CMDQ;
        uint32_t t = gr_cmdq[at].type, p = gr_cmdq[at].part, a = gr_cmdq[at].a, b = gr_cmdq[at].b;
        cmd_r++;
        if (t == CMD_NOTE_ON)
            gr_note_on(p, a, b);
        else if (t == CMD_NOTE_OFF)
            gr_note_off(p, a);
        else if (t == CMD_FREEZE)
            dr_freeze_set(a);
        else
            gr_all_sound_off(p);
    }
}
