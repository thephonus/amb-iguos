/* SPDX-License-Identifier: GPL-3.0-only
 * The panel: encoders and buttons to the pages, and the LEDs.
 *   K1..K4 turn the four dials of the active row     ALGORITHM  the row            SELECT  the page
 *   PRESETS the preset slot                          ARP the part (1 .. 4)         OCT- / OCT+ the octave of the keys
 *   HOME GRAIN, ENV VOICE, FX EFFECTS, LFO FILTER + LFO, EDIT GENERATE, SCL SAMPLES, SAVE PRESETS, GLO INFO
 *   SEQ: the sequencer's pages (Steps, Tools, Sequencer), and held, a fill. PLAY and REC are the transport and the
 *   record arm, except on GENERATE (PLAY makes the sound), SAMPLES (PLAY loads, REC erases) and PRESETS (PLAY loads
 *   the slot, SAVE saves it twice if it is in use, REC resets everything twice).
 *   On STEPS and TOOLS the PRESETS knob moves the step cursor. Nothing is recorded unless REC was pressed: REC arms, and
 *   the keys then start the recording (Rec start: First note), or PLAY does after a count-in (Count-in), or each chord
 *   is written into the step under the cursor (Step). REC and SEQ together start recording from the top after
 *   Rec delay seconds, counted down in the status bar; any transport key cancels it. */
static const uint8_t BTN_PAGE[NB] = {[B_FX] = PG_FX + 1, [B_ENV] = PG_VOICE + 1, [B_LFO] = PG_MOD + 1, [B_EDIT] = PG_GEN + 1,
                                     [B_HOME] = PG_GRAIN + 1, [B_SCL] = PG_SAMPLES + 1, [B_SAVE] = PG_PRESETS + 1,
                                     [B_GLO] = PG_INFO + 1};

/* keys played while stopped on the Steps page with REC armed and Rec start on Step: a chord written into the step under
 * the cursor when the keys are up */
static void ui_chord_poll(void)
{
    uint32_t edges = fm1_input_note_edges(), k;
    int active = ui.page == PG_STEPS && !seq.playing && !seq.counting && !seq.recording && !seq.delaying && seq.armed &&
                 gr_index(0, P_SEQRECSTART) == 2u;             /* only with REC armed in Step mode */
    if (!active) {
        ui.chord = NO_NOTES;
        return;
    }
    for (k = 0; k < KB_KEYS; k++)
        if ((edges >> k) & 1u) {
            int32_t note = KB_FIRST + (int32_t)k + 12 * gr_kb_oct;
            if (note >= 0 && note <= 127)
                sn_add(&ui.chord, (uint32_t)note);
        }
    if (ui.chord != NO_NOTES && fm1_in.notes == 0u) {            /* all keys up: the chord is the step */
        seq_step_t st;
        uint32_t cur = ui_cur();
        seq_save_undo(ui.part);
        seq_get(ui.part, cur, &st);
        if (st.time != STT_NOTE) {
            uint8_t vel = st.vel;
            step_blank(&st);
            st.vel = vel;
        }
        st.time = STT_NOTE;
        st.notes = ui.chord;
        st.vel = 100;
        seq_set(ui.part, cur, &st);
        ui.last_note[ui.part] = (uint8_t)sn_note(ui.chord, 0);
        ui.chord = NO_NOTES;
        ui_move_cursor(1);
    }
}

static void ui_set_page(uint32_t pg)
{
    ui.page = (uint8_t)pg;
    ui.bank = 0;
    ui.confirm = 0;
}

static void ui_set_part(uint32_t part)
{
    ui.part = (uint8_t)part;
    gr_kb_part = (uint8_t)part;
    if (ui.bank >= ui_nbanks(ui.page))
        ui.bank = 0;
}

static void ui_do_generate(void)
{
    ui_notice("Generating...");
    ui_draw();
    gr_cmd(CMD_ALL_OFF, ui.part, 0, 0);
    gr_generate(ui.part);
    gr_cmd(CMD_NOTE_ON, ui.part, ROOT_NOTE, 100);                     /* a test note, as the Granular synth */
    ui.audition_ms = fm1_ms + 700u;
    ui.audition_part = ui.part;
    ui_notice(gr_pt[ui.part].smp.name);
}

static void ui_input(void)
{
    uint32_t now = fm1_ms, pressed, released, b, k;
    int32_t d;
    pressed = fm1_input_edges(&released);
    (void)released;
    seq.fill_held = (fm1_in.buttons >> PANEL_BTN[B_SEQ]) & 1u;
    ui_chord_poll();
    if (ui.audition_ms && (int32_t)(now - ui.audition_ms) >= 0) {
        gr_cmd(CMD_NOTE_OFF, ui.audition_part, ROOT_NOTE, 0);
        ui.audition_ms = 0;
    }
    if (ui.confirm && (uint32_t)(now - ui.confirm_ms) > 3000u)
        ui.confirm = 0;
    for (k = 0; k < 4u; k++) {                                        /* the four dials */
        d = panel_enc(EN_K1 + k);
        if (d) {
            int32_t a = d < 0 ? -d : d, accel = a >= 3 ? 8 : a == 2 ? 4 : (now - ui.turned[k]) < 40u ? 2 : 1;
            uint32_t cell = ui.bank * 4u + k;
            if (ui.page == PG_PRESETS) {
                if (k == 0u) {
                    ui.slot = (uint8_t)((ui.slot + NPRESET + (uint32_t)d % NPRESET) % NPRESET);
                    ui.confirm = 0;
                }
            } else if (ui.page == PG_SAMPLES) {
                if (k == 0u) {
                    ui.sel = (uint8_t)((ui.sel + NSLOT + (uint32_t)d % NSLOT) % NSLOT);
                    ui.confirm = 0;
                }
            } else if (cell < ui_ncells(ui.page)) {
                ui_turn_cell(ui.page, cell, d, accel);
            }
            ui.turned[k] = now;
        }
    }
    d = panel_enc(EN_ALGO);
    if (d && ui.page != PG_PRESETS && ui.page != PG_SAMPLES) {
        uint32_t nb = ui_nbanks(ui.page);
        ui.bank = (uint8_t)((ui.bank + nb + (uint32_t)(d % (int32_t)nb)) % nb);
    }
    d = panel_enc(EN_SELECT);
    if (d)
        ui_set_page((ui.page + PG_COUNT + (uint32_t)(d % PG_COUNT)) % PG_COUNT);
    d = panel_enc(EN_PRESET);
    if (d && (ui.page == PG_STEPS || ui.page == PG_TOOLS)) {
        ui_move_cursor(d);
        d = 0;
    }
    if (d) {
        ui.slot = (uint8_t)((ui.slot + NPRESET + (uint32_t)(d % NPRESET)) % NPRESET);
        ui.confirm = 0;
        ui.turned[0] = now;
        if (ui.page != PG_PRESETS) {
            char t[28];
            str_cpy(t, "Preset ", sizeof t);
            fmt_int(t + 7, (int32_t)ui.slot + 1);
            str_cpy(t + str_len(t), ((ui.saved_mask >> ui.slot) & 1) ? " (saved)" : " (empty)", 10);
            ui_notice(t);
        }
    }
    {   /* REC and SEQ together: record from the top after the Rec delay */
        uint32_t sb = 1u << PANEL_BTN[B_SEQ], rb = 1u << PANEL_BTN[B_REC];
        if (((pressed & sb) && (fm1_in.buttons & rb)) || ((pressed & rb) && (fm1_in.buttons & sb))) {
            seq.rec_part = (uint8_t)ui.part;
            seq.delay_req = 1;
            pressed &= ~(sb | rb);
        }
    }
    for (b = 0; b < (uint32_t)NB; b++) {
        if (!(pressed & (1u << PANEL_BTN[b])))
            continue;
        if (BTN_PAGE[b] && !(b == B_SAVE && (ui.page == PG_PRESETS || ui.page == PG_GEN))) {
            ui_set_page((uint32_t)BTN_PAGE[b] - 1u);
            continue;
        }
        switch (b) {
        case B_ARP:
            ui_set_part((ui.part + 1u) % NPART);
            break;
        case B_OCTDN:
        case B_OCTUP: {
            int32_t o = gr_kb_oct + (b == B_OCTUP ? 1 : -1);
            char t[28];
            gr_kb_oct = (int8_t)clampi(o, -3, 3);
            str_cpy(t, "Octave ", sizeof t);
            fmt_fix(t + 7, gr_kb_oct, 0);
            ui_notice(t);
            break;
        }
        case B_SEQ:
            ui_set_page(ui.page == PG_STEPS ? PG_TOOLS : ui.page == PG_TOOLS ? PG_SEQ : PG_STEPS);
            break;
        case B_PLAY:
            if ((ui.page != PG_GEN && ui.page != PG_SAMPLES && ui.page != PG_PRESETS) || (gr_ambient(ui.part) && ui.page != PG_PRESETS))
                seq.play_req = 1;
            else if (ui.page == PG_GEN)
                ui_do_generate();
            else if (ui.page == PG_SAMPLES) {
                static const char *const MSG[] = {"Loaded", "Bad argument", "Too big", "Flash error", "Damaged", "Slot is empty"};
                int rc;
                ui_notice("Loading...");
                ui_draw();
                gr_cmd(CMD_ALL_OFF, ui.part, 0, 0);
                rc = smp_load(ui.sel, ui.part);
                ui_notice(MSG[rc < 6 ? rc : 3]);
            } else if (ui.page == PG_PRESETS) {
                if (preset_load(ui.slot) == 0)
                    ui_notice("Preset loaded");
                else
                    ui_notice("Slot is empty");
            }
            break;
        case B_SAVE:                                                   /* on the Presets and Generate pages */
            if (ui.page == PG_GEN) {
                int rc = smp_store(ui.part);
                if (rc >= 0) {
                    char t[28];
                    str_cpy(t, "Stored in slot ", sizeof t);
                    fmt_int(t + 15, rc + 1);
                    ui_notice(t);
                } else {
                    ui_notice(rc == -1 ? "No free slot" : "Flash error");
                }
            } else if (ui.page == PG_PRESETS) {
                if (((ui.saved_mask >> ui.slot) & 1) && ui.confirm != 1) {
                    ui.confirm = 1;
                    ui.confirm_ms = now;
                } else {
                    ui.confirm = 0;
                    ui_notice(preset_save(ui.slot) == 0 ? "Preset saved" : "Save failed");
                }
            }
            break;
        case B_REC:
            if ((ui.page != PG_SAMPLES && ui.page != PG_PRESETS) || (gr_ambient(ui.part) && ui.page == PG_SAMPLES)) {
                seq.rec_part = (uint8_t)ui.part;
                seq.rec_req = 1;
            } else if (ui.page == PG_SAMPLES) {
                if (ui.confirm != 3) {
                    ui.confirm = 3;
                    ui.confirm_ms = now;
                } else {
                    ui.confirm = 0;
                    ui_notice(smp_erase(ui.sel) == LKE_OK ? "Slot erased" : "Flash error");
                }
            } else if (ui.page == PG_PRESETS) {
                if (ui.confirm != 2) {
                    ui.confirm = 2;
                    ui.confirm_ms = now;
                } else {
                    uint32_t part;
                    ui.confirm = 0;
                    state_defaults();
                    for (part = 0; part < NPART; part++) {
                        gr_cmd(CMD_ALL_OFF, part, 0, 0);
                        gr_generate(part);
                    }
                    ui_notice("Defaults");
                }
            }
            break;
        default:
            break;
        }
    }
}

/* the LEDs: the page's button, ARP for part 2, the keys that sound */
static void ui_leds(void)
{
    uint32_t b, k;
    for (b = 0; b < (uint32_t)NB; b++) {
        int on = (BTN_PAGE[b] && (uint32_t)BTN_PAGE[b] - 1u == ui.page) || (b == B_ARP && ui.part >= 2u) ||
                 (b == B_OCTUP && gr_kb_oct > 0) || (b == B_OCTDN && gr_kb_oct < 0);
        fm1_led_key(PANEL_BTN[b], on);
    }
    fm1_led_key(PANEL_BTN[B_SEQ], ui.page == PG_STEPS || ui.page == PG_TOOLS || ui.page == PG_SEQ);
    fm1_led_key(PANEL_BTN[B_PLAY], seq.playing || seq.counting);
    fm1_led_key(PANEL_BTN[B_REC], seq.recording || seq.armed || seq.counting || seq.delaying);
    for (k = 0; k < KB_KEYS; k++)
        fm1_led_key(14u + k, (fm1_in.notes >> k) & 1u);
}
