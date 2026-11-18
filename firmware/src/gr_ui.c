/* SPDX-License-Identifier: GPL-3.0-only
 * The screens of Mini-Grain, after the Granular synth's Screen.h / UiModel.h, on the FM-1's 240 x 240 panel and
 * its seven encoders. Everything is made of grains: dials are rings of dots, and each page's picture is stippled
 * like sand. Part 1 is sand-coloured, part 2 aqua, coral while a dial is being turned.
 *
 *   status bar  22 px   part, page, the sound, grains playing and the CPU as six dots
 *   picture     66 px   the page's picture (waveform with the grains, envelope, echoes)
 *   dials       4 x 66  K1..K4 are the four dials of the active row; ALGORITHM picks the row
 *   page dots   at the bottom */

enum { PG_GRAIN, PG_VOICE, PG_FX, PG_MOD, PG_GEN, PG_SAMPLES, PG_PRESETS, PG_STEPS, PG_TOOLS, PG_SEQ, PG_INFO, PG_COUNT };

#define CELL_W 58
#define CELL_H 66
#define ROW_PITCH 68
#define STATUS_H 22
#define PIC_H 66
#define PIC_BOTTOM (STATUS_H + PIC_H + 8)
#define DOTS_Y 234
#define COMPACT_H 34
#define COMPACT_PITCH 37

static const uint8_t PL_GRAIN[] = {P_POSITION, P_SIZE, P_DENSITY, P_SPREAD, P_SCAN, P_JITTER, P_PITCHRND, P_PAN};
static const uint8_t PL_VOICE[] = {P_ATTACK, P_RELEASE, P_REVERSE, P_TRANSPOSE, P_LEVEL, P_VOLUME, P_MIDICH, P_LATCH};
static const uint8_t PL_FX[] = {P_DLYTIME, P_DLYFB, P_DLYDAMP, P_DLYSEND, P_REVDECAY, P_REVDAMP, P_REVSEND};
static const uint8_t PL_MOD[] = {P_FTYPE, P_CUTOFF, P_RESO, P_DRIVE, P_L1WAVE, P_L1RATE, P_L1DEPTH, P_L1DEST,
                                 P_L2WAVE, P_L2RATE, P_L2DEPTH, P_L2DEST};
static const uint8_t PL_SEQ[] = {P_TEMPO, P_SEQGSWING, P_SEQCLICK, P_SEQRECSTART, P_SEQLEN, P_SEQDIV, P_SEQSWING,
                                 P_SEQGATE, P_SEQSYNC, P_SEQOUT, P_SEQSTOP, P_SEQDELAY};
static const uint8_t PL_AOSC[] = {P_AWAVE, P_AHARM, P_ASTRUCT, P_APITCH, P_ADETUNE, P_ABAL, P_AMODRATE, P_AMODDEPTH};
static const uint8_t PL_AVOICE[] = {P_AATK, P_AREL, P_TRANSPOSE, P_LEVEL, P_APAN, P_AVOICE, P_AGLIDE, P_AUNIDET,
                                    P_AMODSHAPE, P_MIDICH, P_VOLUME, P_LATCH};

static const char *const PAGE_TITLE[PG_COUNT] = {"Grain", "Voice", "Effects", "Filter + LFO", "Generate", "Samples",
                                                 "Presets", "Steps", "Tools", "Sequencer", "Info"};

static struct {
    uint8_t page, part, bank, slot;
    uint8_t confirm;                     /* 1: SAVE asked to overwrite, 2: RESET asked */
    uint8_t sel;                         /* the Samples page's cursor */
    uint32_t turned[4];                  /* ms of each knob's last turn */
    uint32_t confirm_ms, notice_ms;
    char notice[28];
    uint32_t cell_key[12], status_key, pic_key, list_key, last_midi_ms;
    uint32_t midi_seen;
    uint8_t force, last_page, last_part;
    uint16_t saved_mask;                 /* preset slots in use (bit per slot) */
    uint8_t cursor[NPART];               /* the step cursor of each part's pattern */
    uint8_t lockch;                      /* the parameter the Tools page's lock acts on (SEQ_LOCKCH) */
    int8_t arm_cell;                     /* a destructive action waiting for its second turn, or -1 */
    uint32_t act_ms, undo_ms, arm_ms;
    uint32_t chord;                      /* the notes of the chord being entered from the keys (0xFF = none) */
    uint8_t last_note[NPART];
    uint8_t audition_part, uboot;        /* uboot: seconds left before the update mode (main.c) */
    uint32_t audition_ms;
} ui;

static void ui_notice(const char *s)
{
    str_cpy(ui.notice, s, sizeof ui.notice);
    ui.notice_ms = fm1_ms ? fm1_ms : 1u;
}

/* ------------------------------------------------------- primitives --- */
static inline uint16_t px_get(int32_t x, int32_t y)
{
    return (uint16_t)(((cv_px[y * (int32_t)cv_w + x] >> 8) & 0xFFu) | ((cv_px[y * (int32_t)cv_w + x] & 0xFFu) << 8));
}

/* an anti-aliased dot of radius r (fillSmoothCircle) */
static void cv_dot(int32_t cx, int32_t cy, int32_t r, uint16_t c)
{
    int32_t x, y, inner = r * r - r, outer = r * r + r;
    for (y = cy - r - 1; y <= cy + r + 1; y++)
        for (x = cx - r - 1; x <= cx + r + 1; x++) {
            int32_t d2 = (x - cx) * (x - cx) + (y - cy) * (y - cy);
            if (x < 0 || y < 0 || x >= (int32_t)cv_w || y >= (int32_t)cv_h || d2 >= outer)
                continue;
            if (d2 <= inner)
                cv_pset(x, y, c);
            else
                cv_pset(x, y, ux_mix(px_get(x, y), c, (outer - d2) * 100 / (2 * r)));
        }
}

static void cv_ring(int32_t cx, int32_t cy, int32_t r, uint16_t c)              /* a 1 px circle */
{
    int32_t x, y;
    for (y = cy - r - 1; y <= cy + r + 1; y++)
        for (x = cx - r - 1; x <= cx + r + 1; x++) {
            int32_t d2 = (x - cx) * (x - cx) + (y - cy) * (y - cy), lo = r * r - r, hi = r * r + r;
            if (x < 0 || y < 0 || x >= (int32_t)cv_w || y >= (int32_t)cv_h || d2 < lo - r || d2 > hi + r)
                continue;
            if (d2 >= lo && d2 <= hi)
                cv_pset(x, y, c);
            else
                cv_pset(x, y, ux_mix(px_get(x, y), c, 40));
        }
}

/* an outline of a rounded rectangle: the rectangle in c, then the inside in `inner` */
static void cv_rr_outline(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, uint16_t c, uint16_t inner, uint16_t under)
{
    cv_rrect(x, y, w, h, r, c, under);
    cv_rrect(x + 1, y + 1, w - 2, h - 2, r > 1 ? r - 1 : 1, inner, c);
}

static uint32_t hash_add(uint32_t h, uint32_t v) { return (h ^ v) * 16777619u; }
static uint32_t hash_str(uint32_t h, const char *s)
{
    while (*s)
        h = hash_add(h, (uint8_t)*s++);
    return h;
}

static inline uint16_t accent_of(uint32_t part) { return part == 0u ? UC_SAND : part == 1u ? UC_AQUA : part == 2u ? UC_LILAC : UC_MINT; }

/* ------------------------------------------------------------ dials --- */
#define DIAL_DOTS 13
static int8_t dial_dx[DIAL_DOTS], dial_dy[DIAL_DOTS];
static void dial_init(void)
{
    int32_t i;
    for (i = 0; i < DIAL_DOTS; i++) {
        /* 270 degrees, starting at 135 degrees (down left), the gap at the bottom */
        uint32_t ph;
        ph = (uint32_t)(0x60000000u + (uint32_t)(0xC0000000u / (DIAL_DOTS - 1)) * (uint32_t)i);
        dial_dx[i] = (int8_t)((sin32(ph + 0x40000000u) * 15 + (sin32(ph + 0x40000000u) < 0 ? -16384 : 16384)) >> 15);
        dial_dy[i] = (int8_t)((sin32(ph) * 15 + (sin32(ph) < 0 ? -16384 : 16384)) >> 15);
    }
}

/* norm 0..65535 (marker: the LFO's position or -1) */
static void draw_dial(int32_t cx, int32_t cy, int32_t norm, int bipolar, uint16_t lit, int32_t marker)
{
    int32_t from = bipolar ? 32768 : 0, low = norm < from ? norm : from, high = norm < from ? from : norm, i;
    const int32_t slot = 65536 / (2 * (DIAL_DOTS - 1));
    for (i = 0; i < DIAL_DOTS; i++) {
        int32_t at = i * 65536 / (DIAL_DOTS - 1), amount = 0;
        if (at + slot <= high && at - slot >= low)
            amount = 100;
        else if (at >= low - slot && at <= high + slot) {
            int32_t a = at - slot > low ? at - slot : low, b = at + slot < high ? at + slot : high;
            amount = (b - a) * 100 / (2 * slot);
            if (amount < 0)
                amount = 0;
        }
        cv_dot(cx + dial_dx[i], cy + dial_dy[i], amount > 50 ? 2 : 1, ux_mix(UC_UNLIT, lit, amount));
    }
    if (marker >= 0) {
        uint32_t ph = 0x60000000u + (uint32_t)(((uint64_t)marker * 0xC0000000u) >> 16);
        cv_dot(cx + ((sin32(ph + 0x40000000u) * 9) >> 15), cy + ((sin32(ph) * 9) >> 15), 2, UC_TEXT);
    }
}

static void draw_icon(int32_t cx, int32_t cy, uint32_t icon, uint16_t lit)
{
    int32_t i;
    for (i = 0; i < 26; i++)
        cv_rect(cx - 24 + i * 48 / 25, cy - ((GR_ICON[icon * 26u + (uint32_t)i] * 11) >> 5), 2, 2, lit);
}

static void draw_choice_dots(int32_t cx, int32_t cy, int32_t count, int32_t index, uint16_t lit)
{
    int32_t gap = count > 6 ? 8 : 11, x0 = cx - (count - 1) * gap / 2, i;
    if (count > 9)
        return;
    for (i = 0; i < count; i++)
        cv_dot(x0 + i * gap, cy, i == index ? 3 : 1, i == index ? lit : UC_UNLIT);
}

/* the icon of a choice's label, or -1 */
static int32_t icon_of(const char *s)
{
    static const char *const N[11] = {"Sine", "Triangle", "Saw", "Square", "Sample+Hold", "Smooth rnd", "Off",
                                      "Vintage 24", "Res LP", "Res BP", "Res HP"};
    int32_t i;
    for (i = 0; i < 11; i++)
        if (str_eq(N[i], s))
            return i;
    return -1;
}

typedef struct {
    const char *name;
    char value[28];
    int32_t norm, marker;
    uint8_t kind;                    /* CK_* */
    uint8_t count, index, sel, edit, asking;
} cell_t;
enum { CK_DIAL, CK_BIDIAL, CK_CHOICE, CK_NUMBER, CK_DEST, CK_BUTTON, CK_PRIMARY, CK_INFO };

static void draw_cell(int32_t slot, int32_t x, int32_t y, const cell_t *c, int compact)
{
    uint32_t key = 2166136261u;
    uint16_t lit, face;
    int32_t w = CELL_W, h = compact ? COMPACT_H : CELL_H;
    key = hash_str(key, c->name);
    key = hash_str(key, c->value);
    key = hash_add(key, (uint32_t)(c->norm >> 7));
    key = hash_add(key, (uint32_t)(c->marker < 0 ? 1000 : c->marker >> 11) + ((uint32_t)c->kind << 12));
    key = hash_add(key, (uint32_t)c->sel + 2u * c->edit + 4u * c->asking + 8u * ui.part + 16u * compact);
    if (ui.cell_key[slot] == key)
        return;
    ui.cell_key[slot] = key;
    lit = c->edit ? UC_CORAL : accent_of(ui.part);
    cv_begin((uint32_t)w, (uint32_t)h, UC_BG);
    if (c->kind == CK_BUTTON || c->kind == CK_PRIMARY) {
        face = c->asking ? UC_CORAL : c->kind == CK_PRIMARY ? ux_mix(UC_BG, accent_of(ui.part), 34) : UC_PANEL;
        if (!compact) {
            const char *sp = c->value;
            uint16_t ink = c->asking ? UC_BG : UC_TEXT;
            while (*sp && *sp != ' ')
                sp++;
            cv_rrect(0, 12, w, 42, 8, face, UC_BG);
            if (*sp && text_w(&AF_S, c->value) > w - 6) {         /* a long label breaks onto two lines */
                char first[16];
                uint32_t n = (uint32_t)(sp - c->value);
                if (n > 15u)
                    n = 15u;
                memcpy(first, c->value, n);
                first[n] = 0;
                cv_text_in(0, 12 + 8, w, &AF_S, first, ink, face);
                cv_text_in(0, 12 + 23, w, &AF_S, sp + 1, ink, face);
            } else {
                cv_text_in(0, 12 + CAP_IN(S, 42), w, &AF_S, c->value, ink, face);
            }
        } else {
            cv_rrect(0, 0, w, h, 8, face, UC_BG);
            cv_text_in(0, CAP_IN(S, h), w, &AF_S, c->value, c->asking ? UC_BG : UC_TEXT, face);
        }
        cv_blit((uint32_t)x, (uint32_t)y);
        return;
    }
    if (c->sel)
        cv_rrect(0, 0, w, h, 8, UC_PANEL, UC_BG);
    if (c->edit)
        cv_rr_outline(0, 0, w, h, 8, UC_CORAL, c->sel ? UC_PANEL : UC_BG, UC_BG);
    {
        uint16_t under = c->sel ? UC_PANEL : UC_BG;
        cv_text_in(0, 0, w, &AF_S, c->name, c->sel ? UC_TEXT : UC_MUTED, under);
        if (compact) {
            char v[28];
            text_fit(v, sizeof v, c->value, &AF_S, w - 4);
            cv_text_in(0, h - 15, w, &AF_S, v, c->edit ? UC_CORAL : UC_TEXT, under);
        } else {
            switch (c->kind) {
            case CK_DIAL: draw_dial(w / 2, 33, c->norm, 0, lit, c->marker); break;
            case CK_BIDIAL: draw_dial(w / 2, 33, c->norm, 1, lit, c->marker); break;
            case CK_CHOICE: {
                int32_t ic = icon_of(c->value);
                if (ic >= 0)
                    draw_icon(w / 2, 33, (uint32_t)ic, lit);
                else
                    draw_choice_dots(w / 2, 33, c->count, c->index, lit);
                break;
            }
            case CK_DEST: {
                int off = str_eq(c->value, "Off"), d;
                for (d = 0; d < 5; d++)
                    cv_rect(w / 2 - 24 + d * 8, 32, 2, 2, off ? UC_UNLIT : ux_mix(under, lit, 30 + 15 * d));
                cv_dot(w / 2 + 22, 33, off ? 2 : 4, off ? UC_UNLIT : lit);
                break;
            }
            case CK_NUMBER:
                cv_text_in(0, text_w(&AF_L, c->value) <= w - 2 ? 24 : 28, w, text_w(&AF_L, c->value) <= w - 2 ? &AF_L : &AF_M, c->value, lit, under);
                break;
            default: break;
            }
            if (c->kind != CK_NUMBER) {
                char v[28];
                text_fit(v, sizeof v, c->value, &AF_S, w - 2);
                cv_text_in(0, h - 15, w, &AF_S, v, c->edit ? UC_CORAL : c->sel ? UC_TEXT : ux_mix(UC_MUTED, UC_TEXT, 40), under);
            }
        }
    }
    cv_blit((uint32_t)x, (uint32_t)y);
}

/* ------------------------------------------------------ cell models --- */
static int pg_has_picture(uint32_t pg)
{
    if (gr_ambient(ui.part) && pg == PG_VOICE)
        return 0;
    return pg == PG_GRAIN || pg == PG_VOICE || pg == PG_FX || pg == PG_STEPS;
}

static const uint8_t *page_list(uint32_t pg, uint32_t *n)
{
    if (gr_ambient(ui.part)) {
        if (pg == PG_GRAIN) {
            *n = sizeof PL_AOSC;
            return PL_AOSC;
        }
        if (pg == PG_VOICE) {
            *n = sizeof PL_AVOICE;
            return PL_AVOICE;
        }
    }
    switch (pg) {
    case PG_GRAIN: *n = sizeof PL_GRAIN; return PL_GRAIN;
    case PG_VOICE: *n = sizeof PL_VOICE; return PL_VOICE;
    case PG_FX: *n = sizeof PL_FX; return PL_FX;
    case PG_MOD: *n = sizeof PL_MOD; return PL_MOD;
    case PG_SEQ: *n = sizeof PL_SEQ; return PL_SEQ;
    default: *n = 0; return 0;
    }
}

enum { GC_TYPE, GC_LENGTH, GC_RUN, GC_STORE, GC_FIRST };

static uint32_t ui_ncells(uint32_t pg)
{
    uint32_t n;
    if (gr_ambient(ui.part) && (pg == PG_GEN || pg == PG_SAMPLES))
        return 0;
    if (pg == PG_GEN)
        return GC_FIRST + GR_GENTYPES[gr_gen[ui.part].type].n;
    if (pg == PG_STEPS)
        return 8;
    if (pg == PG_TOOLS)
        return 12;
    page_list(pg, &n);
    return n;
}

static uint32_t ui_nbanks(uint32_t pg)
{
    uint32_t n = ui_ncells(pg);
    return n ? (n + 3u) / 4u : 1u;
}

static void cell_from_info(cell_t *c, const gr_pinfo_t *i, uint32_t norm)
{
    c->name = i->name;
    gr_info_fmt(c->value, i, norm);
    c->norm = (int32_t)norm;
    c->marker = -1;
    c->kind = i->unit == GR_U_CHOICE ? CK_CHOICE : i->unit == GR_U_DEST ? CK_DEST : i->unit == GR_U_CHAN ? CK_NUMBER :
              (i->mn < 0 && i->mx > 0 ? CK_BIDIAL : CK_DIAL);
    c->count = (uint8_t)(i->steps + 1u);
    c->index = (uint8_t)gr_info_index(i, norm);
}

#include "gr_ui_seq.c"
#include "gr_ui_amb.c"

static void describe(uint32_t pg, uint32_t i, cell_t *c)
{
    uint32_t n;
    memset(c, 0, sizeof *c);
    c->marker = -1;
    gr_dest_tab = gr_dest_of(ui.part);
    if (pg == PG_STEPS) {
        describe_steps(i, c);
        return;
    }
    if (pg == PG_TOOLS) {
        describe_tools(i, c);
        return;
    }
    if (pg == PG_GEN) {
        const gr_gen_t *g = &gr_gen[ui.part];
        const gr_gentype_t *t = &GR_GENTYPES[g->type];
        if (i == GC_TYPE) {
            c->name = "Type";
            str_cpy(c->value, t->name, sizeof c->value);
            c->kind = CK_INFO;
        } else if (i == GC_LENGTH) {
            cell_from_info(c, &GR_GENLEN, g->length);
            c->kind = CK_INFO;
        } else if (i == GC_RUN) {
            c->name = "PLAY";
            str_cpy(c->value, "Generate", sizeof c->value);
            c->kind = CK_PRIMARY;
        } else if (i == GC_STORE) {
            c->name = "SAVE";
            str_cpy(c->value, "Store", sizeof c->value);
            c->kind = CK_BUTTON;
        } else {
            cell_from_info(c, &t->p[i - GC_FIRST], g->norm[g->type][i - GC_FIRST]);
        }
        return;
    }
    {
        const uint8_t *l = page_list(pg, &n);
        uint32_t p = l[i];
        cell_from_info(c, &GR_PINFO[p], gr_norm(ui.part, p));
        if (gr_ambient(ui.part))
            amb_fix_cell(p, c);
        if (gr_pt[ui.part].off[p] != 0 && !gr_is_global(p))
            c->marker = gr_pt[ui.part].vnorm[p];
        else if (gr_is_global(p)) {
            int32_t off = gr_pt[0].off[p] + gr_pt[1].off[p] + gr_pt[2].off[p] + gr_pt[3].off[p];
            if (off)
                c->marker = clampi((int32_t)gr_norm(0, p) + off, 0, 65535);
        }
    }
}

static void turn_norm(const gr_pinfo_t *i, uint16_t *norm, int32_t d, int32_t accel)
{
    int32_t step = i->steps ? 65535 / i->steps : 328, n = (int32_t)*norm + d * (i->steps ? 1 : accel) * step;
    if (i->steps) {                                   /* whole steps: from the snapped value */
        int32_t idx = (int32_t)gr_info_index(i, *norm) + d;
        n = idx <= 0 ? 0 : idx >= (int32_t)i->steps ? 65535 : (int32_t)((uint32_t)idx * 65535u / i->steps);
    }
    *norm = (uint16_t)clampi(n, 0, 65535);
}

static void ui_turn_cell(uint32_t pg, uint32_t cell, int32_t d, int32_t accel)
{
    if (pg == PG_STEPS) {
        turn_steps(cell, d, accel);
        return;
    }
    if (pg == PG_TOOLS) {
        turn_tools(cell, d, accel);
        return;
    }
    if (pg == PG_GEN) {
        gr_gen_t *g = &gr_gen[ui.part];
        if (cell == GC_TYPE) {
            int32_t t = (int32_t)g->type + d;
            g->type = (uint8_t)(t < 0 ? GEN_COUNT - 1 : t >= GEN_COUNT ? 0 : t);
            ui.bank = 0;
        } else if (cell == GC_LENGTH) {
            turn_norm(&GR_GENLEN, &g->length, d, accel);
        } else if (cell >= GC_FIRST) {
            turn_norm(&GR_GENTYPES[g->type].p[cell - GC_FIRST], &g->norm[g->type][cell - GC_FIRST], d, accel);
        }
        return;
    }
    {
        uint32_t n;
        const uint8_t *l = page_list(pg, &n);
        uint16_t v;
        if (cell >= n)
            return;
        v = (uint16_t)gr_norm(ui.part, l[cell]);
        turn_norm(&GR_PINFO[l[cell]], &v, d, accel);
        gr_set_norm(ui.part, l[cell], v);
        seq_record_knob(ui.part, l[cell], v);                /* recording: the move becomes a gliding lock */
    }
}

/* ------------------------------------------------------------ status --- */
static void draw_status(void)
{
    uint32_t key = 2166136261u, load = gr_st.load, dots, grains = gr_st.grains;
    char det[40], cnt[8];
    uint16_t dcol = UC_MUTED, acc = accent_of(ui.part);
    const gr_sample_t *s = &gr_pt[ui.part].smp;
    uint32_t midi_on = (uint32_t)(fm1_ms - ui.last_midi_ms) < 120u;
    int32_t x;
    if (ui.midi_seen != gr_midi_activity) {
        ui.midi_seen = gr_midi_activity;
        ui.last_midi_ms = fm1_ms;
        midi_on = 1;
    }
    if (ui.notice_ms && (uint32_t)(fm1_ms - ui.notice_ms) > 2200u)
        ui.notice_ms = 0;
    if (ui.notice_ms) {
        str_cpy(det, ui.notice, sizeof det);
        dcol = UC_TEXT;
    } else if (gr_pt[ui.part].smp_state != SMP_NORMAL) {
        str_cpy(det, "Working...", sizeof det);
        dcol = acc;
    } else if (ui.page == PG_STEPS || ui.page == PG_TOOLS || ui.page == PG_SEQ) {
        det[0] = 0;
        if (ui.page != PG_SEQ)
            str_cpy(det, "Cycle ", sizeof det);
        seq_cycle_text(det + str_len(det), sizeof det - str_len(det), ui.part);
    } else if (gr_ambient(ui.part)) {
        amb_detail(det, sizeof det);
    } else {
        str_cpy(det, s->frames ? s->name : "No sound", sizeof det);
    }
    dots = load >= 243u ? 6u : load * 6u / 243u;
    key = hash_str(key, det);
    key = hash_add(key, ui.page + 16u * ui.part + 64u * dots + 1024u * grains + 65536u * midi_on + (load > 217u ? 1u << 20 : 0));
    key = hash_add(key, (uint32_t)seq.playing + 2u * seq.recording + 4u * seq.armed + 8u * seq.counting + 16u * seq.beat_flash + 32u * seq.count_beat + 512u * seq.delaying + 1024u * seq.delay_sec);
    if (key == ui.status_key)
        return;
    ui.status_key = key;
    cv_begin(240, STATUS_H, UC_BG);
    cv_dot(11, 10, 8, acc);
    cv_text_in(3, 10 - 6 + 1, 17, &AF_S, ui.part == 0u ? "1" : ui.part == 1u ? "2" : ui.part == 2u ? "3" : "4", UC_BG, acc);
    x = cv_text(24, 3, &AF_M, gr_ambient(ui.part) && ui.page == PG_GRAIN ? "Oscillator" : PAGE_TITLE[ui.page], UC_TEXT) + 8;
    cv_text_fit(x, 4, &AF_S, det, dcol, UC_BG, 136 - x);
    fmt_int(cnt, (int32_t)grains);
    cv_text_r(204 - 6, 4, &AF_S, cnt, UC_MUTED, UC_BG);
    if (midi_on)
        cv_dot(142, 10, 2, UC_CORAL);
    draw_transport_mark(162, 11, acc);
    for (x = 0; x < 6; x++)
        cv_dot(206 + x * 6, 11, 2, x < (int32_t)dots ? (load > 217u ? UC_WARN : acc) : UC_UNLIT);
    cv_blit(0, 0);
}

static void draw_page_dots(void)
{
    int32_t i, x0 = 120 - (PG_COUNT - 1) * 5;
    cv_begin(240, 8, UC_BG);
    for (i = 0; i < PG_COUNT; i++)
        cv_dot(x0 + i * 10, 4, i == ui.page ? 2 : 1, i == ui.page ? accent_of(ui.part) : UC_UNLIT);
    cv_blit(0, DOTS_Y - 3);
}

/* ---------------------------------------------------------- pictures --- */
static uint16_t pic_mix(uint16_t a, uint16_t b, int32_t pct) { return ux_mix(a, b, clampi(pct, 0, 100)); }

static void picture_grain(void)
{
    const gr_part_t *pt = &gr_pt[ui.part];
    int32_t x, y, mid = PIC_H / 2, pos = (int32_t)(pt->vnorm[P_POSITION] * 239u >> 16), half = pt->val[P_SPREAD] * 12 / 100;
    uint16_t acc = accent_of(ui.part);
    uint32_t i;
    cv_begin(240, PIC_H, UC_BG);
    if (pt->smp.frames) {
        for (x = 1; x < 240; x += 3) {
            int32_t h = pt->smp.peaks[x] * (mid - 3) / 255, dist = x > pos ? x - pos : pos - x, in;
            dist = dist > 240 - dist ? 240 - dist : dist;
            in = dist <= half + 2;
            for (y = 0; y <= h; y += 3) {
                int32_t fade = h > 0 ? 100 - 45 * y / h : 100;
                uint16_t col = pic_mix(UC_BG, in ? acc : UC_UNLIT, fade);
                cv_rect(x, mid - y, 2, 2, col);
                if (y)
                    cv_rect(x, mid + y, 2, 2, col);
            }
        }
    } else {
        cv_text_in(0, CAP_IN(S, PIC_H), 240, &AF_S, "No sound yet: press EDIT, then PLAY", UC_MUTED, UC_BG);
    }
    for (y = 1; y < PIC_H; y += 4)
        cv_rect(pos, y, 1, 2, UC_TEXT);
    for (i = 0; i < NGRAIN; i++) {
        uint16_t v = gr_st.gview[i];
        if (v == GVIEW_NONE || gr_st.gpart[i] != ui.part)
            continue;
        x = (int32_t)((uint32_t)v * 239u / 65534u);
        y = 6 + (int32_t)((i * 37u) % (PIC_H - 12));
        cv_dot(x, y, 4, pic_mix(UC_BG, acc, 28));
        cv_dot(x, y, 2, pic_mix(acc, UC_TEXT, 50));
    }
    cv_blit(0, STATUS_H + 4);
}

static void picture_env(void)
{
    uint32_t a = gr_norm(ui.part, P_ATTACK), r = gr_norm(ui.part, P_RELEASE), lv = gr_norm(ui.part, P_LEVEL);
    uint32_t key = 1u + a + (r << 16) + (lv << 8) + ui.part * 77u;
    int32_t left = 8, right = 232, base = PIC_H - 4, aend = left + (int32_t)(a * 110u >> 16) + 3, rstart = right - (int32_t)(r * 130u >> 16) - 3;
    int32_t peak = (PIC_H - 10) * (25 + 75 * (int32_t)lv / 65536) / 100, x, y;
    uint16_t acc = accent_of(ui.part);
    if (key == ui.pic_key)
        return;
    ui.pic_key = key;
    cv_begin(240, PIC_H, UC_BG);
    for (x = left; x < right; x += 3) {
        int32_t shape, h;
        if (x < aend) {
            int32_t t = (x - left) * 4096 / (aend - left);
            shape = (t * t / 4096) * (3 * 4096 - 2 * t) / 4096;
        } else if (x < rstart) {
            shape = 4096;
        } else {
            int32_t t = (x - rstart) * 4096 / (right - rstart);
            shape = (4096 - t) * (4096 - t) / 4096;
        }
        h = shape * peak / 4096;
        for (y = 0; y <= h; y += 3) {
            int crest = y + 3 > h;
            int32_t fade = h > 0 ? 28 + 72 * y / h : 28;
            cv_rect(x, base - y, 2, 2, crest ? acc : pic_mix(UC_BG, acc, fade * 6 / 10));
        }
    }
    cv_blit(0, STATUS_H + 4);
}

static void picture_echo(void)
{
    int32_t time = (int32_t)gr_norm(0, P_DLYTIME), fb = gr_real(0, P_DLYFB), decay = (int32_t)gr_norm(0, P_REVDECAY);
    int32_t dsend = gr_real(ui.part, P_DLYSEND), rsend = gr_real(ui.part, P_REVSEND);
    uint32_t key = 2166136261u;
    uint16_t acc = accent_of(ui.part);
    int32_t upper = PIC_H / 2 - 13, lower = PIC_H / 2 + 13, i, cloud_end = 90 + (decay * 150 >> 16) , x, step, level, side = 0;
    uint32_t seed = 0x2F6E2B1u;
    uint16_t cloud;
    key = hash_add(key, (uint32_t)time >> 8);
    key = hash_add(key, (uint32_t)fb + (uint32_t)(decay >> 8) * 1024u + (uint32_t)dsend * 31u + (uint32_t)rsend * 977u + ui.part);
    key |= 1u;
    if (key == ui.pic_key)
        return;
    ui.pic_key = key;
    cv_begin(240, PIC_H, UC_BG);
    cloud = ux_mix(ux_mix(UC_BG, UC_UNLIT, 70), acc, clampi(15 + rsend / 20, 0, 100));
    for (i = 0; i < 420; i++) {
        int32_t t, sy;
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        t = (int32_t)(seed & 1023u);
        sy = (int32_t)((seed >> 10) & 1023u) - 512;
        x = 8 + (t * t >> 10) * (cloud_end - 8) / 1024;
        if (x < 238)
            cv_rect(x, PIC_H / 2 + sy * (PIC_H - 8) / 1024 * (100 - 60 * t / 1024) / 100, 2, 2, ux_mix(cloud, UC_BG, t * 100 / 1024));
    }
    step = 16 + (time * 46 >> 16);
    level = 70 + 30 * dsend / 1000;
    cv_dot(10, PIC_H / 2, 5, UC_TEXT);
    for (x = 10 + step; x < 234 && level > 5; x += step) {
        int32_t r = 1 + level * 6 / 100;
        uint16_t lit = dsend > 2 ? pic_mix(UC_BG, acc, 25 + 75 * level / 100) : UC_UNLIT;
        cv_dot(x, side ? lower : upper, r, lit);
        side ^= 1;
        level = level * (35 + fb * 70 / 1000) / 100;
    }
    cv_blit(0, STATUS_H + 4);
}

/* ------------------------------------------------------------ pages --- */
static void draw_cells(uint32_t pg)
{
    uint32_t n = ui_ncells(pg), i, now = fm1_ms;
    int32_t top = pg_has_picture(pg) ? PIC_BOTTOM : pg == PG_GEN ? STATUS_H + 4 : STATUS_H + 4;
    for (i = 0; i < n; i++) {
        cell_t c;
        int compact = pg == PG_GEN && i < 4u;
        int32_t x, y;
        describe(pg, i, &c);
        c.sel = (i / 4u) == ui.bank;
        c.edit = c.sel && (now - ui.turned[i % 4u]) < 500u;
        if (pg == PG_GEN && i < 4u) {
            x = 1 + (int32_t)i * (CELL_W + 2);
            y = top;
        } else if (pg == PG_GEN) {
            x = 1 + (int32_t)((i - 4u) % 4u) * (CELL_W + 2);
            y = top + COMPACT_PITCH + (int32_t)((i - 4u) / 4u) * ROW_PITCH;
        } else {
            x = 1 + (int32_t)(i % 4u) * (CELL_W + 2);
            y = top + (int32_t)(i / 4u) * ROW_PITCH;
        }
        if (pg == PG_GEN && i < 4u)
            c.sel = ui.bank == 0;
        else if (pg == PG_GEN)
            c.sel = (i / 4u) == ui.bank;
        draw_cell((int32_t)i, x, y, &c, compact);
    }
}

static void draw_gen_wave(void)
{
    const gr_part_t *pt = &gr_pt[ui.part];
    uint32_t key = pt->smp.frames + ui.part * 5u + 1u + (uint32_t)pt->smp.peaks[100] * 65536u + (uint32_t)pt->smp.peaks[20] * 7u;
    int32_t top = STATUS_H + 4 + COMPACT_PITCH + 2 * ROW_PITCH + 2, h = DOTS_Y - 6 - top, x, y, mid;
    if (key == ui.pic_key)
        return;
    ui.pic_key = key;
    mid = h / 2;
    cv_begin(240, (uint32_t)h, UC_BG);
    for (x = 1; x < 240; x += 3) {
        int32_t pk = pt->smp.frames ? pt->smp.peaks[x] * mid / 255 : 0;
        for (y = 0; y <= pk; y += 2) {
            cv_pset(x, mid - y, ux_mix(UC_BG, accent_of(ui.part), 34));
            cv_pset(x, mid + y, ux_mix(UC_BG, accent_of(ui.part), 34));
        }
    }
    cv_blit(0, (uint32_t)top);
}

static void draw_presets(void)
{
    uint32_t key = ui.saved_mask * 31u + ui.slot * 1000003u + ui.confirm * 77u + 1u, i;
    int32_t y = 124;
    cell_t c;
    if (key != ui.pic_key) {
        uint16_t acc = accent_of(ui.part);
        ui.pic_key = key;
        cv_begin(240, 96, UC_BG);
        for (i = 0; i < NPRESET; i++) {
            int32_t cx = 15 + (int32_t)(i % 8u) * 30, cy = 22 + (int32_t)(i / 8u) * 40, filled = (ui.saved_mask >> i) & 1;
            char num[4];
            if (i == ui.slot) {
                cv_dot(cx, cy, 14, filled ? acc : UC_PANEL);
                if (!filled)
                    cv_ring(cx, cy, 14, acc);
                fmt_int(num, (int32_t)i + 1);
                cv_text_in(cx - 10, cy - 6, 20, &AF_S, num, filled ? UC_BG : acc, filled ? acc : UC_PANEL);
            } else {
                cv_dot(cx, cy, filled ? 7 : 3, filled ? ux_mix(UC_BG, acc, 34) : UC_UNLIT);
            }
        }
        cv_blit(0, STATUS_H + 4);
    }
    memset(&c, 0, sizeof c);
    c.marker = -1;
    c.name = "Preset";
    str_cpy(c.value, "", sizeof c.value);
    fmt_int(c.value, (int32_t)ui.slot + 1);
    str_cpy(c.value + str_len(c.value), ((ui.saved_mask >> ui.slot) & 1) ? " saved" : " empty", 10);
    c.kind = CK_INFO;
    c.sel = 1;
    c.edit = (fm1_ms - ui.turned[0]) < 500u;
    draw_cell(0, 1, y, &c, 1);
    c.sel = 0;
    c.edit = 0;
    c.name = "PLAY";
    str_cpy(c.value, "Load", sizeof c.value);
    c.kind = CK_PRIMARY;
    draw_cell(1, 61, y, &c, 1);
    c.name = "SAVE";
    str_cpy(c.value, ui.confirm == 1 ? "Overwrite?" : "Save", sizeof c.value);
    c.kind = CK_BUTTON;
    c.asking = ui.confirm == 1;
    draw_cell(2, 121, y, &c, 1);
    c.name = "REC";
    str_cpy(c.value, ui.confirm == 2 ? "Sure?" : "Reset all", sizeof c.value);
    c.asking = ui.confirm == 2;
    draw_cell(3, 181, y, &c, 1);
    if (ui.list_key != 1u) {
        ui.list_key = 1u;
        cv_begin(240, 70, UC_BG);
        cv_text_in(0, 0, 240, &AF_S, "PRESETS knob: slot   PLAY: load", UC_MUTED, UC_BG);
        cv_text_in(0, 18, 240, &AF_S, "SAVE: save (twice to overwrite)", UC_MUTED, UC_BG);
        cv_text_in(0, 36, 240, &AF_S, "REC: reset all (twice)", UC_MUTED, UC_BG);
        cv_text_in(0, 56, 240, &AF_S, "A preset holds both parts and sounds.", UC_UNLIT, UC_BG);
        cv_blit(0, (uint32_t)(y + COMPACT_H + 12));
    }
}

static void draw_info(void)
{
    uint32_t key = (gr_st.cap * 256u + gr_st.load) * 3u + (uint32_t)(fm1_ms >> 10) + gr_st.grains * 7919u + gr_midi_activity, i;
    char b[40], t[16];
    static const char *const L[6] = {"CPU", "Grains", "Cap", "Skipped", "Notes", "MIDI messages"};
    uint32_t v[6];
    if (key == ui.pic_key)
        return;
    ui.pic_key = key;
    v[0] = gr_st.load * 100u / 255u;
    v[1] = gr_st.grains;
    v[2] = gr_st.cap;
    v[3] = gr_st.skipped;
    v[4] = (uint32_t)gr_st.notes[0] + gr_st.notes[1];
    v[5] = gr_midi_activity;
    cv_begin(240, 52, UC_BG);
    cv_text(8, 2, &AF_M, "Mini-Grain", UC_TEXT);
    cv_text(8, 26, &AF_S, "A granular synthesizer for the FM-1", UC_MUTED);
    cv_blit(0, STATUS_H + 4);
    cv_begin(240, 96, UC_BG);
    for (i = 0; i < 6u; i++) {
        cv_text(8, (int32_t)i * 16, &AF_S, L[i], UC_MUTED);
        fmt_int(t, (int32_t)v[i]);
        str_cpy(b, t, sizeof b);
        if (i == 0u)
            str_cpy(b + str_len(b), " %", 4);
        cv_text_r(232, (int32_t)i * 16, &AF_S, b, UC_TEXT, UC_BG);
    }
    cv_blit(0, STATUS_H + 60);
    cv_begin(240, 20, UC_BG);
    cv_text(8, 2, &AF_S, FELUCCA_VERSION, UC_UNLIT);
    cv_blit(0, STATUS_H + 160);
}

static void draw_samples(void)
{
    uint32_t key = 2166136261u, i, half;
    uint16_t acc = accent_of(ui.part);
    key = hash_add(key, smp_used + ui.sel * 4099u + ui.part * 77u + ui.confirm * 131u);
    key = hash_add(key, (uint32_t)gr_src_kind[0] + gr_src_slot[0] * 8u + gr_src_kind[1] * 64u + gr_src_slot[1] * 512u);
    if (key == ui.list_key)
        return;
    ui.list_key = key;
    for (half = 0; half < 2u; half++) {
        cv_begin(240, 4u * 19u, UC_BG);
        for (i = half * 4u; i < half * 4u + 4u; i++) {
            int32_t y = (int32_t)(i - half * 4u) * 19;
            uint32_t used = (smp_used >> i) & 1u, cur = gr_src_kind[ui.part] == 1 && gr_src_slot[ui.part] == i;
            char num[6], dur[12], nm[20];
            smp_hdr_t h;
            if (i == ui.sel)
                cv_rrect(2, y, 224, 18, 7, ui.confirm == 3 ? UC_CORAL : UC_PANEL, UC_BG);
            if (cur)
                cv_dot(13, y + 9, 3, acc);
            fmt_int(num, (int32_t)i + 1);
            cv_text(22, y + 2, &AF_S, num, used ? UC_TEXT : UC_UNLIT);
            if (used && smp_hdr_read(i, &h) == 0) {
                text_fit(nm, sizeof nm, h.name, &AF_S, 120);
                cv_text(46, y + 2, &AF_S, nm, cur ? acc : (i == ui.sel ? UC_TEXT : UC_MUTED));
                gr_fix(dur, (int32_t)(h.frames * 1000u / (h.rate ? h.rate : 1u)), 1000, 1, 0);
                gr_cat(dur, " s");
                cv_text_r(218, y + 2, &AF_S, dur, UC_MUTED, i == ui.sel ? UC_PANEL : UC_BG);
            } else {
                cv_text(46, y + 2, &AF_S, ui.confirm == 3 && i == ui.sel ? "Erase?" : "empty", UC_UNLIT);
            }
        }
        cv_blit(0, STATUS_H + 4u + half * 76u);
    }
    cv_begin(240, 24, UC_BG);
    cv_text_in(0, 2, 240, &AF_S, "K1: pick   PLAY: load into the part   REC: erase", UC_MUTED, UC_BG);
    cv_blit(0, STATUS_H + 4u + 160u);
}

/* ------------------------------------------------------------- draw --- */
static void ui_draw(void)
{
    if (ui.force || ui.page != ui.last_page || ui.part != ui.last_part) {
        lcd_fill(0, 0, 240, 240, UC_BG);
        memset(ui.cell_key, 0, sizeof ui.cell_key);
        ui.status_key = ui.pic_key = ui.list_key = 0;
        ui.force = 0;
        ui.last_page = ui.page;
        ui.last_part = ui.part;
        draw_page_dots();
    }
    draw_status();
    switch (ui.page) {
    case PG_GRAIN:
        if (gr_ambient(ui.part))
            picture_wave();
        else
            picture_grain();
        draw_cells(ui.page);
        break;
    case PG_VOICE:
        if (!gr_ambient(ui.part))
            picture_env();
        draw_cells(ui.page);
        break;
    case PG_FX:
        picture_echo();
        draw_cells(ui.page);
        break;
    case PG_MOD:
    case PG_SEQ:
    case PG_TOOLS:
        draw_cells(ui.page);
        break;
    case PG_STEPS:
        picture_roll();
        draw_cells(ui.page);
        break;
    case PG_GEN:
        if (gr_ambient(ui.part))
            draw_not_granular("Generate");
        else {
            draw_cells(ui.page);
            draw_gen_wave();
        }
        break;
    case PG_SAMPLES:
        if (gr_ambient(ui.part))
            draw_not_granular("Samples");
        else
            draw_samples();
        break;
    case PG_PRESETS: draw_presets(); break;
    default: draw_info(); break;
    }
}

/* the theme: the FM-1 UI's text and blending code takes its tokens from the palette tables; Mini-Grain draws with
 * its own colours (UC_*), so only the blending mode matters here (colour, flat) */
static void ui_theme_init(void)
{
    palette_set(0);
    ux.mono = 0;
    ux.style = ST_FLAT;
}
