/* SPDX-License-Identifier: GPL-3.0-only
 * The sequencer's screens (included by gr_ui.c): the piano roll and the Steps page, the Tools page (parameter locks,
 * copy, move, delete, undo) and the transport mark of the status bar. After the Granular synth's Screen.h and
 * UiModel.h. The sequencer's settings (tempo, swing, length ...) are an ordinary page of dials (PG_SEQ). */
static const uint8_t FORCE_VEL[5] = {40, 70, 100, 118, 127};

static int32_t force_of(uint32_t vel) { return vel >= 124u ? 4 : vel >= 110u ? 3 : vel >= 85u ? 2 : vel >= 55u ? 1 : 0; }

static uint32_t tie_count(uint32_t part, uint32_t index, uint32_t length)
{
    uint32_t ties = 0;
    while (ties + 1u < length) {
        seq_step_t s;
        seq_get(part, (index + ties + 1u) % length, &s);
        if (s.time != STT_TIE)
            break;
        ties++;
    }
    return ties;
}

static void note_name(char *d, uint32_t note)
{
    static const char *const N[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    char t[6];
    str_cpy(d, N[note % 12u], 8);
    fmt_fix(t, (int32_t)(note / 12u) - 1, 0);
    gr_cat(d, t);
}

static uint32_t ui_len(void)
{
    uint32_t n = seq_length(ui.part);
    return n < 1u ? 1u : n;
}
static uint32_t ui_cur(void)
{
    if (ui.cursor[ui.part] >= ui_len())
        ui.cursor[ui.part] = 0;
    return ui.cursor[ui.part];
}
static void ui_move_cursor(int32_t d)
{
    int32_t n = (int32_t)ui_len(), c = ((int32_t)ui_cur() + d) % n;
    ui.cursor[ui.part] = (uint8_t)(c < 0 ? c + n : c);
}
/* one undo step for a run of edits: a new one after a pause */
static void ui_undo_point(void)
{
    if ((uint32_t)(fm1_ms - ui.undo_ms) > 1500u)
        seq_save_undo(ui.part);
    ui.undo_ms = fm1_ms;
}

static void cell_label(cell_t *c, const char *name, const char *value, uint32_t kind)
{
    c->name = name;
    str_cpy(c->value, value, sizeof c->value);
    c->kind = (uint8_t)kind;
}

static void describe_steps(uint32_t i, cell_t *c)
{
    static const char *const FORCE[5] = {"Ghost", "Soft", "Normal", "Hard", "Accent"};
    static const char *const FILLC[3] = {"Always", "Fill only", "No fill"};
    uint32_t part = ui.part, cur = ui_cur(), length = ui_len();
    seq_step_t s;
    char t[16];
    seq_get(part, cur, &s);
    switch (i) {
    case 0:
        fmt_int(c->value, (int32_t)cur + 1);
        gr_cat(c->value, "/");
        fmt_int(t, (int32_t)length);
        gr_cat(c->value, t);
        c->name = "Step";
        c->kind = CK_NUMBER;
        break;
    case 1:
        c->name = "Note";
        c->kind = CK_NUMBER;
        if (s.time == STT_TIE)
            str_cpy(c->value, "Tie", sizeof c->value);
        else if (s.time != STT_NOTE || sn_count(s.notes) == 0)
            str_cpy(c->value, "Rest", sizeof c->value);
        else {
            note_name(c->value, sn_note(s.notes, 0));
            if (sn_count(s.notes) > 1u) {
                gr_cat(c->value, "+");
                fmt_int(t, (int32_t)sn_count(s.notes) - 1);
                gr_cat(c->value, t);
            }
        }
        break;
    case 2:
        c->name = "Force";
        c->kind = CK_CHOICE;
        c->count = 5;
        c->index = (uint8_t)force_of(s.vel);
        str_cpy(c->value, FORCE[c->index], sizeof c->value);
        break;
    case 3:
        c->name = "Length";
        c->kind = CK_NUMBER;
        fmt_int(c->value, s.time == STT_NOTE ? 1 + (int32_t)tie_count(part, cur, length) : 0);
        break;
    case 4:
        c->name = "Ratchet";
        c->kind = CK_CHOICE;
        c->count = 4;
        c->index = (uint8_t)(s.ratchet - 1u);
        str_cpy(c->value, "x", sizeof c->value);
        fmt_int(t, s.ratchet);
        gr_cat(c->value, t);
        break;
    case 5:
        c->name = "Slide";
        c->kind = CK_CHOICE;
        c->count = 2;
        c->index = s.slide;
        str_cpy(c->value, s.slide ? "On" : "Off", sizeof c->value);
        break;
    case 6:
        c->name = "Nudge";
        c->kind = CK_BIDIAL;
        c->norm = ((int32_t)s.nudge + 32) * 65535 / 63;
        gr_fix(c->value, s.nudge, 1, 0, 1);
        break;
    default:
        c->name = "Plays";
        c->kind = CK_CHOICE;
        c->count = 3;
        c->index = s.fill;
        str_cpy(c->value, FILLC[s.fill < 3u ? s.fill : 0u], sizeof c->value);
        break;
    }
}

/* the time the pattern takes to come round: 3:20, or 12.5 s when short */
static void seq_cycle_text(char *out, uint32_t max, uint32_t part)
{
    uint32_t t = seq_cycle_tenths(part);
    char a[16];
    if (t >= 600u) {
        uint32_t s = (t + 5u) / 10u;
        fmt_int(a, (int32_t)(s / 60u));
        str_cpy(out, a, max);
        gr_cat(out, ":");
        fmt_int(a, (int32_t)(s % 60u));
        if (s % 60u < 10u)
            gr_cat(out, "0");
        gr_cat(out, a);
    } else {
        fmt_int(a, (int32_t)(t / 10u));
        str_cpy(out, a, max);
        gr_cat(out, ".");
        fmt_int(a, (int32_t)(t % 10u));
        gr_cat(out, a);
        gr_cat(out, " s");
    }
}

static void describe_tools(uint32_t i, cell_t *c)
{
    uint32_t part = ui.part, cur = ui_cur(), lp = seq_lockch(part, ui.lockch);
    int slot = seq_find_lock(part, cur, lp);
    char t[16];
    switch (i) {
    case 0:
        describe_steps(0, c);
        break;
    case 1:
        cell_label(c, "Lock", GR_PINFO[lp].name, CK_CHOICE);
        c->count = 0;
        break;
    case 2:
        c->name = "Lock value";
        c->kind = CK_DIAL;
        if (slot >= 0) {
            c->norm = (int32_t)lock_value(seq_tr[part].locks[slot]);
            gr_info_fmt(c->value, &GR_PINFO[lp], (uint32_t)c->norm);
        } else {
            str_cpy(c->value, "Not set", sizeof c->value);
        }
        break;
    case 3: cell_label(c, "", "Clear locks", CK_BUTTON); break;
    case 4: cell_label(c, "", "Copy step", CK_BUTTON); break;
    case 5:
        c->name = "Move step";
        c->kind = CK_NUMBER;
        fmt_int(c->value, (int32_t)cur + 1);
        break;
    case 6: cell_label(c, "", "Delete step", CK_BUTTON); break;
    case 7:
        cell_label(c, "", ui.arm_cell == 7 ? "Sure?" : "Clear track", CK_BUTTON);
        c->asking = ui.arm_cell == 7;
        break;
    case 8: cell_label(c, "", "Undo", CK_BUTTON); break;
    case 9:
        c->name = "Locks used";
        c->kind = CK_NUMBER;
        fmt_int(c->value, (int32_t)seq_locks_used(part));
        gr_cat(c->value, "/");
        fmt_int(t, SEQ_LOCKS);
        gr_cat(c->value, t);
        break;
    case 11:
        c->name = "Glide";
        c->kind = CK_CHOICE;
        c->count = 2;
        c->index = slot >= 0 && lock_glide(seq_tr[part].locks[slot]) ? 1u : 0u;
        str_cpy(c->value, slot < 0 ? "No lock" : c->index ? "On" : "Off", sizeof c->value);
        break;
    default:
        cell_from_info(c, &GR_PINFO[P_SEQQUANT], gr_norm(0, P_SEQQUANT));
        break;
    }
}

/* the Steps page: a turn of one dial */
static void turn_steps(uint32_t cell, int32_t d, int32_t accel)
{
    uint32_t part = ui.part, cur = ui_cur(), length = ui_len();
    seq_step_t s;
    seq_get(part, cur, &s);
    if (cell == 0u) {
        ui_move_cursor(d);
        return;
    }
    ui_undo_point();
    if (cell == 1u) {                                             /* an empty step takes the last note, then the chord moves */
        if (s.time != STT_NOTE || sn_count(s.notes) == 0) {
            uint8_t vel = s.vel;
            step_blank(&s);
            s.vel = vel;
            s.time = STT_NOTE;
            s.notes = 0xFFFFFF00u | ui.last_note[part];
        } else {
            uint32_t i, notes = NO_NOTES;
            for (i = 0; i < 4u; i++) {
                int32_t n = (int32_t)sn_note(s.notes, i);
                if (n != 0xFF && (n + d < 0 || n + d > 127))
                    return;
            }
            for (i = 0; i < 4u; i++) {
                uint32_t n = sn_note(s.notes, i);
                if (n != 0xFFu)
                    notes = (notes & ~(0xFFu << (i * 8u))) | (uint32_t)((int32_t)n + d) << (i * 8u);
            }
            s.notes = notes;
            ui.last_note[part] = (uint8_t)sn_note(s.notes, 0);
        }
        seq_set(part, cur, &s);
        return;
    }
    if (s.time != STT_NOTE)
        return;
    if (cell == 2u) {
        s.vel = FORCE_VEL[clampi(force_of(s.vel) + d, 0, 4)];
    } else if (cell == 3u) {                                      /* the ties after the note */
        uint32_t ties = tie_count(part, cur, length);
        int32_t k;
        for (k = 0; k < (d < 0 ? -d : d); k++) {
            if (d > 0) {
                seq_step_t nx, tie;
                uint32_t next = (cur + ties + 1u) % length;
                seq_get(part, next, &nx);
                if (ties + 1u >= length || nx.time != STT_REST)
                    break;
                step_blank(&tie);
                tie.time = STT_TIE;
                seq_set(part, next, &tie);
                ties++;
            } else if (ties > 0u) {
                seq_clear_step(part, (cur + ties) % length);
                ties--;
            }
        }
        return;
    } else if (cell == 4u) {
        s.ratchet = (uint8_t)clampi((int32_t)s.ratchet + d, 1, 4);
    } else if (cell == 5u) {
        s.slide = d > 0;
    } else if (cell == 6u) {
        s.nudge = (int8_t)clampi((int32_t)s.nudge + d * accel, NUDGE_MIN, NUDGE_MAX);
    } else {
        s.fill = (uint8_t)clampi((int32_t)s.fill + d, 0, 2);
    }
    seq_set(part, cur, &s);
}

static int ui_act_ok(void)                                        /* one action per turn burst */
{
    if ((uint32_t)(fm1_ms - ui.act_ms) < 350u)
        return 0;
    ui.act_ms = fm1_ms;
    return 1;
}

/* the Tools page: a turn of one dial (the buttons are actions: one turn does it, Clear track asks twice) */
static void turn_tools(uint32_t cell, int32_t d, int32_t accel)
{
    uint32_t part = ui.part, cur = ui_cur(), length = ui_len();
    if (ui.arm_cell >= 0 && (uint32_t)(fm1_ms - ui.arm_ms) > 3000u)
        ui.arm_cell = -1;
    if (cell == 0u) {
        ui_move_cursor(d);
    } else if (cell == 1u) {
        ui.lockch = (uint8_t)(((int32_t)(ui.lockch % seq_nlockch(part)) + (int32_t)seq_nlockch(part) + d % (int32_t)seq_nlockch(part)) % (int32_t)seq_nlockch(part));
    } else if (cell == 2u) {                                      /* the first turn starts from the part's own value */
        uint32_t lp = seq_lockch(part, ui.lockch);
        int slot = seq_find_lock(part, cur, lp);
        int32_t n = slot >= 0 ? (int32_t)lock_value(seq_tr[part].locks[slot]) : (int32_t)gr_norm(part, lp);
        const gr_pinfo_t *pi = &GR_PINFO[lp];
        ui_undo_point();
        if (pi->steps) {
            n = (int32_t)(((uint32_t)gr_info_index(pi, (uint32_t)n) + (uint32_t)(int32_t)d) * 65535u / pi->steps);
            n = gr_info_snap(pi, n);
        } else {
            n += d * accel * 328;
        }
        if (!seq_set_lock(part, cur, lp, n))
            ui_notice("No lock left");
    } else if (cell == 5u) {                                      /* move the step (and its locks) along the pattern */
        uint32_t to = (cur + length + (d > 0 ? 1u : length - 1u)) % length;
        seq_step_t a, b;
        ui_undo_point();
        seq_get(part, cur, &a);
        seq_get(part, to, &b);
        seq_set(part, to, &a);
        seq_set(part, cur, &b);
        seq_move_step_locks(part, cur, 0xFEu);
        seq_move_step_locks(part, to, cur);
        seq_move_step_locks(part, 0xFEu, to);
        ui.cursor[part] = (uint8_t)to;
    } else if (cell == 11u) {                                     /* glide: the lock moves on to the next one of its parameter */
        uint32_t lp = seq_lockch(part, ui.lockch);
        int slot = seq_find_lock(part, cur, lp);
        if (slot < 0) {
            ui_notice("Set a lock first");
        } else {
            ui_undo_point();
            seq_set_lock_g(part, cur, lp, (int32_t)lock_value(seq_tr[part].locks[slot]), d > 0 ? 1u : 0u);
        }
    } else if (cell == 10u) {
        gr_set_norm(0, P_SEQQUANT, (int32_t)gr_norm(0, P_SEQQUANT) + (d > 0 ? 65535 : -65535));
    } else if (cell == 3u || cell == 4u || cell == 6u || cell == 7u || cell == 8u) {
        if (!ui_act_ok())
            return;
        if (cell == 7u && ui.arm_cell != 7) {
            ui.arm_cell = 7;
            ui.arm_ms = fm1_ms;
            return;
        }
        ui.arm_cell = -1;
        if (cell == 8u) {
            ui_notice(seq_swap_undo(part) ? "Undone" : "Nothing to undo");
            return;
        }
        seq_save_undo(part);
        if (cell == 3u) {
            seq_clear_step_locks(part, cur);
        } else if (cell == 4u) {                                  /* copy to the next step and go there */
            uint32_t next = (cur + 1u) % length;
            seq_step_t s;
            seq_get(part, cur, &s);
            seq_set(part, next, &s);
            ui.cursor[part] = (uint8_t)next;
        } else if (cell == 6u) {
            seq_clear_step(part, cur);
            seq_clear_step_locks(part, cur);
        } else {
            seq_clear_track(part);
        }
    }
}

/* ------------------------------------------------------- piano roll --- */
static void picture_roll(void)
{
    uint32_t part = ui.part, cur = ui_cur(), view = cur / 16u * 16u, i, key = 2166136261u;
    int32_t playing = seq.playing || seq.recording ? seq.play_step[part] : -1, low = 127, high = 0, base, range, x, y;
    uint16_t acc = accent_of(part);
    seq_step_t st[16];
    for (i = 0; i < 16u; i++) {
        uint32_t idx = view + i < SEQ_STEPS ? view + i : SEQ_STEPS - 1u, n;
        seq_get(part, idx, &st[i]);
        key = hash_add(key, seq_tr[part].notes[idx]);
        key = hash_add(key, seq_tr[part].meta[idx] + (uint32_t)seq_step_has_lock(part, idx) * 977u);
        if (st[i].time == STT_NOTE)
            for (n = 0; n < 4u; n++) {
                int32_t v = (int32_t)sn_note(st[i].notes, n);
                if (v == 0xFF)
                    continue;
                if (v < low)
                    low = v;
                if (v > high)
                    high = v;
            }
    }
    key = hash_add(key, cur + 1u + (uint32_t)(playing + 2) * 256u + part * 65536u + ui_len() * 4099u);
    key |= 1u;
    if (key == ui.pic_key)
        return;
    ui.pic_key = key;
    if (low > high) {
        low = 57;
        high = 69;
    }
    range = high - low < 12 ? 12 : high - low;
    base = low - (range - (high - low)) / 2;
    cv_begin(240, PIC_H, UC_BG);
    for (i = 0; i < 16u; i++) {                                   /* beats as columns of grains, the cursor's step lit */
        int32_t cx = (int32_t)i * 15;
        uint16_t dot = i % 4u == 0u ? UC_UNLIT : ux_mix(UC_BG, UC_UNLIT, 45);
        if (view + i == cur)
            cv_rrect(cx + 1, 0, 13, PIC_H, 4, UC_PANEL, UC_BG);
        if (view + i >= ui_len())
            continue;
        for (y = 2; y < PIC_H; y += i % 4u == 0u ? 4 : 8)
            cv_rect(cx + 7, y, 1, 1, dot);
    }
    if (playing >= (int32_t)view && playing < (int32_t)view + 16)
        cv_rect((playing - (int32_t)view) * 15 + 1, 0, 13, 2, UC_TEXT);
    for (i = 0; i < 16u; i++) {
        const seq_step_t *s = &st[i];
        int32_t cx = (int32_t)i * 15, ties = 0, n, width;
        uint16_t col;
        if (s->time != STT_NOTE)
            continue;
        while (i + (uint32_t)ties + 1u < 16u && st[i + (uint32_t)ties + 1u].time == STT_TIE)
            ties++;
        col = s->fill == FILL_ONLY ? ux_mix(UC_BG, UC_CORAL, 35 + 65 * (int32_t)s->vel / 127) : ux_mix(UC_BG, acc, 35 + 65 * (int32_t)s->vel / 127);
        width = 15 * (ties + 1) - 4;
        for (n = 0; n < 4; n++) {
            int32_t v = (int32_t)sn_note(s->notes, (uint32_t)n);
            if (v == 0xFF)
                continue;
            y = PIC_H - 7 - (v - base) * (PIC_H - 12) / range;
            if (s->ratchet > 1u && ties == 0) {
                int32_t each = width / (int32_t)s->ratchet, r;
                for (r = 0; r < (int32_t)s->ratchet; r++)
                    cv_rrect(cx + 2 + r * each, y - 2, each > 2 ? each - 1 : 2, 5, 2, col, UC_BG);
            } else {
                cv_rrect(cx + 2, y - 2, width, 5, 2, col, UC_BG);
            }
            if (s->vel >= 124u)
                cv_rect(cx + 2, y - 2, 2, 5, UC_TEXT);
            if (s->slide)
                for (x = 0; x < 3; x++)
                    cv_rect(cx + 15 * (ties + 1) - 2 + x * 3, y, 2, 1, col);
        }
        if (s->nudge != 0 || seq_step_has_lock(part, view + i))
            cv_dot(cx + 10, 5, 2, UC_TEXT);
        if (s->fill == FILL_NONE)
            cv_ring(cx + 5, 5, 2, UC_CORAL);
    }
    cv_blit(0, STATUS_H + 4);
}

/* the transport mark: a triangle while playing, a red ring when ready, a red dot recording, a number counting in */
static void draw_transport_mark(int32_t x, int32_t y, uint16_t acc)
{
    if (seq.counting || seq.delaying) {
        char b[4];
        fmt_int(b, seq.delaying ? seq.delay_sec : seq.count_beat);
        cv_text_in(x - 8, y - 6, 16, &AF_S, b, UC_CORAL, UC_BG);
    } else if (seq.recording) {
        cv_dot(x, y, seq.beat_flash ? 6 : 5, UC_WARN);
    } else if (seq.armed) {
        cv_ring(x, y, 5, UC_WARN);
    } else if (seq.playing) {
        int32_t k;
        uint16_t c = seq.beat_flash ? UC_TEXT : acc;
        for (k = -6; k <= 6; k++) {                               /* a triangle, pointing right */
            int32_t w = 10 - (k < 0 ? -k : k) * 10 / 6;
            cv_rect(x - 4, y + k, w + 1, 1, c);
        }
    }
}
