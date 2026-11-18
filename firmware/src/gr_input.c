/* SPDX-License-Identifier: GPL-3.0-only
 * amb-iguos: the panel. Every control does one thing, the thing printed on it, on every screen:
 *
 *   HOME ESCENA   EDIT FUENTE   SCL ESCALA   ENV VOZ   LFO MOVIMIENTO   FX ESPACIO   SEQ DERIVA   GLO SISTEMA
 *   SAVE          PRESETS; on PRESETS, SAVE saves into the slot (twice if it is in use)
 *   (a section's button pressed again: its next page)
 *   PLAY          the Deriva on / off              REC   the FREEZE on / off           ARP   hold the layer's notes
 *   K1 .. K4      the four dials on the screen     SELECT  the page of the section     ALGORITHM  the layer A / B / D
 *   PRESETS       the preset slot (opens PRESETS)  OCT- / OCT+  the keys' octave (both 5 s: the update mode, main.c)
 *   MASTER        the volume (master.c) */
static const uint8_t BTN_SECTION[NB] = {[B_HOME] = SC_ESCENA + 1, [B_EDIT] = SC_FUENTE + 1, [B_SCL] = SC_ESCALA + 1,
                                        [B_ENV] = SC_VOZ + 1, [B_LFO] = SC_MOV + 1, [B_FX] = SC_ESPACIO + 1,
                                        [B_SEQ] = SC_DERIVA + 1, [B_GLO] = SC_SISTEMA + 1, [B_SAVE] = SC_PRESETS + 1};

static void ui_set_page(uint32_t sc)
{
    ui.page = (uint8_t)sc;
    ui.confirm = 0;
    if (ui.sub[sc] >= ui_npages(sc))
        ui.sub[sc] = 0;
}

static void ui_set_part(uint32_t part)
{
    uint32_t sc;
    ui.part = (uint8_t)part;
    gr_kb_part = (uint8_t)part;
    for (sc = 0; sc < SC_COUNT; sc++)
        if (ui.sub[sc] >= ui_npages(sc))
            ui.sub[sc] = 0;
}

static void ui_turn_norm(const gr_pinfo_t *i, uint16_t *norm, int32_t d, int32_t accel)
{
    int32_t step = i->steps ? 65535 / i->steps : 328, n = (int32_t)*norm + d * (i->steps ? 1 : accel) * step;
    if (i->steps) {
        int32_t idx = (int32_t)gr_info_index(i, *norm) + d;
        n = idx <= 0 ? 0 : idx >= (int32_t)i->steps ? 65535 : (int32_t)((uint32_t)idx * 65535u / i->steps);
    }
    *norm = (uint16_t)clampi(n, 0, 65535);
}

/* the layer's sound, remade from FUENTE's settings (a moment of silence on that layer); a short test note if nothing
 * plays there */
static void ui_regenerate(void)
{
    uint32_t part = ui.part;
    ui.regen_ms = 0;
    if (gr_ambient(part) || gr_src_kind[part] == 1u)
        return;
    gr_generate(part);
    if (!gr_st.notes[part] && !ui.audition_ms) {
        ui.audition_note = (uint32_t)sc_note(0, 4);
        gr_cmd(CMD_NOTE_ON, part, ui.audition_note, 100);
        ui.audition_ms = fm1_ms + 900u;
        ui.audition_part = (uint8_t)part;
    }
}

/* FUENTE's first dial: the generators, then the stored samples that exist */
static void ui_turn_source(int32_t d)
{
    uint32_t part = ui.part, total = GEN_COUNT + ui_nslots(), cur;
    gr_gen_t *g = &gr_gen[part];
    if (gr_src_kind[part] == 1u) {
        uint32_t s, n = 0;
        for (s = 0; s < gr_src_slot[part]; s++)
            n += (smp_used >> s) & 1u;
        cur = GEN_COUNT + n;
    } else {
        cur = g->type;
    }
    cur = (cur + total + (uint32_t)(d % (int32_t)total)) % total;
    if (cur < GEN_COUNT) {
        g->type = (uint8_t)cur;
        gr_src_kind[part] = 0;
        ui.regen_ms = fm1_ms + 500u;
    } else {
        int32_t slot = ui_nth_slot(cur - GEN_COUNT);
        if (slot >= 0) {
            ui.regen_ms = 0;
            ui_notice("Cargando\205");
            ui_draw();
            if (smp_load((uint32_t)slot, part) == LKE_OK) {
                gr_src_kind[part] = 1;
                gr_src_slot[part] = (uint8_t)slot;
                ui_notice("Muestra cargada");
            } else {
                ui_notice("No se pudo leer");
            }
        }
    }
    if (ui.sub[SC_FUENTE] >= ui_npages(SC_FUENTE))
        ui.sub[SC_FUENTE] = 0;
}

static void ui_turn_cell(const cl_t *cl, int32_t d, int32_t accel)
{
    uint32_t part = ui.part;
    switch (cl->kind) {
    case CL_P: {
        uint16_t v = (uint16_t)gr_norm(part, cl->arg);
        ui_turn_norm(&GR_PINFO[cl->arg], &v, d, accel);
        gr_set_norm(part, cl->arg, v);
        if (cl->arg == P_AWAVE || cl->arg == P_AHARM)
            amb_poll();
        break;
    }
    case CL_GTYPE: ui_turn_source(d); break;
    case CL_GLEN:
        ui_turn_norm(&GR_GENLEN, &gr_gen[part].length, d, accel);
        ui.regen_ms = fm1_ms + 600u;
        break;
    case CL_GP: {
        gr_gen_t *g = &gr_gen[part];
        ui_turn_norm(&GR_GENTYPES[g->type].p[cl->arg], &g->norm[g->type][cl->arg], d, accel);
        ui.regen_ms = fm1_ms + 600u;
        break;
    }
    case CL_SLOT:
        ui.slot = (uint8_t)((ui.slot + NPRESET + (uint32_t)(d % NPRESET)) % NPRESET);
        ui.confirm = 0;
        break;
    case CL_LOAD:
        if (d > 0) {
            if (preset_load(ui.slot) == 0) {
                char t[28];
                str_cpy(t, "Preset ", sizeof t);
                fmt_int(t + 7, (int32_t)ui.slot + 1);
                gr_cat(t, " cargado");
                ui_notice(t);
            } else {
                ui_notice("Ranura vac\355a");
            }
        }
        break;
    default: break;
    }
}

static void ui_input(void)
{
    uint32_t now = fm1_ms, pressed, released, b, k;
    int32_t d;
    pressed = fm1_input_edges(&released);
    (void)released;
    if (ui.audition_ms && (int32_t)(now - ui.audition_ms) >= 0) {
        gr_cmd(CMD_NOTE_OFF, ui.audition_part, ui.audition_note, 0);
        ui.audition_ms = 0;
    }
    if (ui.regen_ms && (int32_t)(now - ui.regen_ms) >= 0)
        ui_regenerate();
    if (ui.confirm && (uint32_t)(now - ui.confirm_ms) > 3000u)
        ui.confirm = 0;
    {   /* the four dials */
        cl_t cl[4];
        ui_cells(ui.page, ui.sub[ui.page], ui.part, cl);
        for (k = 0; k < 4u; k++) {
            d = panel_enc(EN_K1 + k);
            if (d && cl[k].kind != CL_NONE) {
                int32_t a = d < 0 ? -d : d, accel = a >= 3 ? 8 : a == 2 ? 4 : (now - ui.turned[k]) < 40u ? 2 : 1;
                ui_turn_cell(&cl[k], d, accel);
                ui.turned[k] = now;
                ui_cells(ui.page, ui.sub[ui.page], ui.part, cl);           /* the source may change the cells */
            }
        }
    }
    d = panel_enc(EN_ALGO);                                               /* the layer */
    if (d)
        ui_set_part((ui.part + NPART + (uint32_t)(d % (int32_t)NPART)) % NPART);
    d = panel_enc(EN_SELECT);                                             /* the page of the section */
    if (d) {
        uint32_t n = ui_npages(ui.page);
        ui.sub[ui.page] = (uint8_t)((ui.sub[ui.page] + n + (uint32_t)(d % (int32_t)n)) % n);
    }
    d = panel_enc(EN_PRESET);                                             /* the preset slot */
    if (d) {
        if (ui.page != SC_PRESETS)
            ui_set_page(SC_PRESETS);
        ui.slot = (uint8_t)((ui.slot + NPRESET + (uint32_t)(d % NPRESET)) % NPRESET);
        ui.confirm = 0;
        ui.turned[0] = now;
    }
    for (b = 0; b < (uint32_t)NB; b++) {
        if (!(pressed & (1u << PANEL_BTN[b])))
            continue;
        if (b == B_SAVE && ui.page == SC_PRESETS) {                       /* SAVE on PRESETS: save */
            if (((ui.saved_mask >> ui.slot) & 1) && ui.confirm != 1) {
                ui.confirm = 1;
                ui.confirm_ms = now;
            } else {
                char t[28];
                ui.confirm = 0;
                if (preset_save(ui.slot) == 0) {
                    str_cpy(t, "Guardado en ", sizeof t);
                    fmt_int(t + 12, (int32_t)ui.slot + 1);
                    ui_notice(t);
                } else {
                    ui_notice("No se pudo guardar");
                }
            }
            continue;
        }
        if (BTN_SECTION[b]) {
            uint32_t sc = (uint32_t)BTN_SECTION[b] - 1u;
            if (ui.page == sc) {                                          /* again: the next page */
                uint32_t n = ui_npages(sc);
                ui.sub[sc] = (uint8_t)((ui.sub[sc] + 1u) % n);
            } else {
                ui_set_page(sc);
            }
            continue;
        }
        switch (b) {
        case B_PLAY:
            dr_on = !dr_on;
            ui_notice(dr_on ? "Deriva" : "Deriva parada");
            break;
        case B_REC:
            gr_cmd(CMD_FREEZE, 0, !gr_freeze, 0);
            ui_notice(!gr_freeze ? "Freeze" : "Freeze fuera");
            break;
        case B_ARP: {                                                     /* hold the layer's notes */
            uint32_t on = !gr_index(ui.part, P_LATCH);
            gr_set_norm(ui.part, P_LATCH, on ? 65535 : 0);
            ui_notice(on ? "Mantener notas" : "Notas libres");
            break;
        }
        case B_OCTDN:
        case B_OCTUP: {
            int32_t o = gr_kb_oct + (b == B_OCTUP ? 1 : -1);
            char t[28];
            gr_kb_oct = (int8_t)clampi(o, -3, 3);
            str_cpy(t, "Octava ", sizeof t);
            if (gr_kb_oct > 0)
                gr_cat(t, "+");
            fmt_fix(t + str_len(t), gr_kb_oct, 0);
            ui_notice(t);
            break;
        }
        default:
            break;
        }
    }
}

/* the LEDs: the section's button, PLAY for the Deriva, REC for the Freeze, ARP for held notes, the octave, the keys */
static void ui_leds(void)
{
    uint32_t b, k;
    for (b = 0; b < (uint32_t)NB; b++) {
        int on = (BTN_SECTION[b] && (uint32_t)BTN_SECTION[b] - 1u == ui.page) || (b == B_PLAY && dr_on) ||
                 (b == B_REC && gr_freeze) || (b == B_ARP && gr_index(ui.part, P_LATCH)) || (b == B_OCTUP && gr_kb_oct > 0) ||
                 (b == B_OCTDN && gr_kb_oct < 0);
        fm1_led_key(PANEL_BTN[b], on);
    }
    for (k = 0; k < KB_KEYS; k++)
        fm1_led_key(14u + k, (fm1_in.notes >> k) & 1u);
}
