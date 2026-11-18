/* SPDX-License-Identifier: GPL-3.0-only
 * The screens of amb-iguos, on the FM-1's 240 x 240 panel. Every screen is built the same way:
 *
 *   status bar   0 .. 23    the layer (A sand, B aqua, D lilac), the page, the scale, DERIVA, FREEZE, the battery
 *   picture     24 .. 151   what the section is about, drawn large and alive (the sound's cloud on ESCENA)
 *   page dots  152 .. 159   which page of the section (SELECT, or the section's button again)
 *   four dials 160 .. 239   exactly what K1 .. K4 turn, nothing else
 *
 * Each button of the panel opens the section printed on it (HOME ESCENA, EDIT FUENTE, SCL ESCALA, ENV VOZ, LFO
 * MOVIMIENTO, FX ESPACIO, SEQ DERIVA, GLO SISTEMA, SAVE PRESETS); PLAY starts the Deriva, REC freezes, ARP holds the
 * notes. The drawing primitives (dots, rounded rectangles, anti-aliased text) are Mini-Grain's / Felucca's. */

enum { SC_ESCENA, SC_FUENTE, SC_ESCALA, SC_VOZ, SC_MOV, SC_ESPACIO, SC_DERIVA, SC_SISTEMA, SC_PRESETS, SC_COUNT };
#define PG_COUNT SC_COUNT

#define STATUS_H 24
#define PIC_Y 24
#define PIC_H 128
#define DOTS_Y 152
#define CELL_Y 160
#define CELL_W 60
#define CELL_H 80

static const char *const SC_TITLE[SC_COUNT] = {"Escena", "Fuente", "Escala", "Voz", "Movimiento", "Espacio", "Deriva",
                                               "Sistema", "Presets"};

/* a cell: what one dial turns */
enum { CL_NONE, CL_P, CL_GTYPE, CL_GLEN, CL_GP, CL_SLOT, CL_LOAD };
typedef struct { uint8_t kind, arg; } cl_t;

static struct {
    uint8_t page, part, sub[SC_COUNT], slot;
    uint8_t confirm;                     /* 1: SAVE asked to overwrite */
    uint32_t turned[4];                  /* ms of each knob's last turn */
    uint32_t confirm_ms, notice_ms;
    char notice[32];
    uint32_t cell_key[4], status_key, pic_key, dots_key, last_midi_ms, midi_seen;
    uint8_t force, last_page, last_part, last_sub;
    uint16_t saved_mask;                 /* preset slots in use (bit per slot) */
    uint32_t regen_ms;                   /* a FUENTE setting changed: the sound is remade at this time (0 = no) */
    uint8_t audition_part, uboot;        /* uboot: seconds left before the update mode (main.c) */
    uint32_t audition_ms, audition_note;
    uint32_t frame;                      /* frames drawn: the pictures' clock */
    uint32_t chord;                      /* (main.c) */
    int8_t arm_cell;                     /* (main.c) */
} ui;

/* the battery (master.c, further down the compilation unit) */
static int32_t batt_raw;
static int32_t batt_level(void);
static int32_t batt_pct(void);

static void ui_notice(const char *s)
{
    str_cpy(ui.notice, s, sizeof ui.notice);
    ui.notice_ms = fm1_ms ? fm1_ms : 1u;
}

/* ------------------------------------------------------- primitives --- */
static inline uint16_t px_get(int32_t x, int32_t y)
{
    y += cv_oy;
    if ((uint32_t)x >= cv_w || (uint32_t)y >= cv_h)
        return cv_bg;
    return (uint16_t)(((cv_px[y * (int32_t)cv_w + x] >> 8) & 0xFFu) | ((cv_px[y * (int32_t)cv_w + x] & 0xFFu) << 8));
}

/* an anti-aliased dot of radius r */
static void cv_dot(int32_t cx, int32_t cy, int32_t r, uint16_t c)
{
    int32_t x, y, inner = r * r - r, outer = r * r + r;
    for (y = cy - r - 1; y <= cy + r + 1; y++)
        for (x = cx - r - 1; x <= cx + r + 1; x++) {
            int32_t d2 = (x - cx) * (x - cx) + (y - cy) * (y - cy);
            if (x < 0 || x >= (int32_t)cv_w || d2 >= outer)
                continue;
            if (d2 <= inner)
                cv_pset(x, y, c);
            else
                cv_pset(x, y, ux_mix(px_get(x, y), c, (outer - d2) * 100 / (2 * r)));
        }
}

/* a soft glow: a disc that fades into what is under it */
static void cv_glow(int32_t cx, int32_t cy, int32_t r, uint16_t c, int32_t pct)
{
    int32_t x, y, r2 = r * r;
    for (y = cy - r; y <= cy + r; y++)
        for (x = cx - r; x <= cx + r; x++) {
            int32_t d2 = (x - cx) * (x - cx) + (y - cy) * (y - cy);
            if (d2 >= r2 || x < 0 || x >= (int32_t)cv_w)
                continue;
            cv_pset(x, y, ux_mix(px_get(x, y), c, pct * (r2 - d2) / r2));
        }
}

static uint32_t hash_add(uint32_t h, uint32_t v) { return (h ^ v) * 16777619u; }
static uint32_t hash_str(uint32_t h, const char *s)
{
    while (*s)
        h = hash_add(h, (uint8_t)*s++);
    return h;
}

#define UC_ICE RGB(0x9F, 0xD8, 0xFF)          /* the FREEZE */
static inline uint16_t accent_of(uint32_t part) { return part == 0u ? UC_SAND : part == 1u ? UC_AQUA : UC_LILAC; }
static inline char layer_letter(uint32_t part) { return part == 0u ? 'A' : part == 1u ? 'B' : 'D'; }

/* ------------------------------------------------------------- dials --- */
#define DIAL_DOTS 17
#define DIAL_R 19
static int8_t dial_dx[DIAL_DOTS], dial_dy[DIAL_DOTS];
static void dial_init(void)
{
    int32_t i;
    for (i = 0; i < DIAL_DOTS; i++) {                /* 270 degrees, the gap at the bottom */
        uint32_t ph = (uint32_t)(0x60000000u + (uint32_t)(0xC0000000u / (DIAL_DOTS - 1)) * (uint32_t)i);
        int32_t c = sin32(ph + 0x40000000u), s = sin32(ph);
        dial_dx[i] = (int8_t)((c * DIAL_R + (c < 0 ? -16384 : 16384)) >> 15);
        dial_dy[i] = (int8_t)((s * DIAL_R + (s < 0 ? -16384 : 16384)) >> 15);
    }
}

/* norm 0..65535; bipolar: lit from the top; marker: where an LFO, a macro or the Deriva has moved it, or -1 */
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
            amount = clampi((b - a) * 100 / (2 * slot), 0, 100);
        }
        cv_dot(cx + dial_dx[i], cy + dial_dy[i], amount > 50 ? 3 : 1, ux_mix(UC_UNLIT, lit, amount));
    }
    if (bipolar)
        cv_rect(cx, cy - DIAL_R - 6, 1, 3, UC_MUTED);
    {   /* the pointer */
        uint32_t ph = 0x60000000u + (uint32_t)(((uint64_t)(uint32_t)norm * 0xC0000000u) >> 16);
        cv_dot(cx + ((sin32(ph + 0x40000000u) * 11) >> 15), cy + ((sin32(ph) * 11) >> 15), 2, lit);
    }
    if (marker >= 0) {
        uint32_t ph = 0x60000000u + (uint32_t)(((uint64_t)(uint32_t)marker * 0xC0000000u) >> 16);
        cv_dot(cx + ((sin32(ph + 0x40000000u) * (DIAL_R - 6)) >> 15), cy + ((sin32(ph) * (DIAL_R - 6)) >> 15), 2, UC_TEXT);
    }
}

/* a choice: its options as a row of dots (a bar with a marker when there are many) */
static void draw_choice_dots(int32_t cx, int32_t cy, int32_t count, int32_t index, uint16_t lit)
{
    int32_t gap = count > 8 ? 4 : count > 5 ? 7 : 10, x0 = cx - (count - 1) * gap / 2, i;
    if (count > 13) {
        cv_rect(cx - 24, cy, 48, 1, UC_UNLIT);
        cv_dot(cx - 24 + index * 48 / (count - 1), cy, 3, lit);
        return;
    }
    for (i = 0; i < count; i++)
        cv_dot(x0 + i * gap, cy, i == index ? 3 : 1, i == index ? lit : UC_UNLIT);
}

/* ------------------------------------------------------------- pages --- */
static const uint8_t PG_MACRO[4] = {P_MACRO1, P_MACRO2, P_MACRO3, P_MACRO4};
static const uint8_t PG_GRAIN1[4] = {P_POSITION, P_SIZE, P_DENSITY, P_SPREAD};
static const uint8_t PG_GRAIN2[4] = {P_SCAN, P_JITTER, P_PITCHRND, P_PAN};
static const uint8_t PG_OSC1[4] = {P_AWAVE, P_AHARM, P_ASTRUCT, P_APITCH};
static const uint8_t PG_OSC2[4] = {P_ADETUNE, P_ABAL, P_AMODRATE, P_AMODDEPTH};
static const uint8_t PG_OSC3[4] = {P_AMODSHAPE, P_AUNIDET, P_APAN, 255};
static const uint8_t PG_SCALE[4] = {P_KEY, P_MODE, P_KEYSNAP, 255};
static const uint8_t PG_VOICE1[4] = {P_ATTACK, P_RELEASE, P_TRANSPOSE, P_LEVEL};
static const uint8_t PG_VOICE2[4] = {P_REVERSE, P_LATCH, P_MIDICH, P_VOLUME};
static const uint8_t PG_AVOICE1[4] = {P_AATK, P_AREL, P_TRANSPOSE, P_LEVEL};
static const uint8_t PG_AVOICE2[4] = {P_AVOICE, P_AGLIDE, P_LATCH, P_MIDICH};
static const uint8_t PG_FILTER[4] = {P_FTYPE, P_CUTOFF, P_RESO, P_DRIVE};
static const uint8_t PG_LFO1[4] = {P_L1WAVE, P_L1RATE, P_L1DEPTH, P_L1DEST};
static const uint8_t PG_LFO2[4] = {P_L2WAVE, P_L2RATE, P_L2DEPTH, P_L2DEST};
static const uint8_t PG_REV[4] = {P_REVSEND, P_REVDECAY, P_REVDAMP, P_SHIMMER};
static const uint8_t PG_DLY[4] = {P_SHINT, P_DLYSEND, P_DLYTIME, P_DLYFB};
static const uint8_t PG_DLY2[4] = {P_DLYDAMP, P_VOLUME, 255, 255};
static const uint8_t PG_DRIFT1[4] = {P_DRATE, P_DPROB, P_DLEN, P_DCHORD};
static const uint8_t PG_DRIFT2[4] = {P_DOCT, P_DRANGE, P_DLAYERS, P_DDRONE};
static const uint8_t PG_DRIFT3[4] = {P_DEVOL, P_DESPD, 255, 255};

/* the name of a page, for the status bar */
static const char *ui_page_name(uint32_t sc, uint32_t sub, uint32_t part)
{
    int amb = gr_ambient(part);
    switch (sc) {
    case SC_ESCENA: return sub == 0u ? "Escena" : amb ? (sub == 1u ? "Oscilador" : "Oscilador 2") : (sub == 1u ? "Grano" : "Grano 2");
    case SC_FUENTE: return amb ? "Fuente" : sub == 0u ? "Fuente" : sub == 1u ? "Fuente 2" : "Fuente 3";
    case SC_VOZ: return sub == 0u ? "Voz" : "Voz 2";
    case SC_MOV: return sub == 0u ? "Filtro" : sub == 1u ? "LFO 1" : "LFO 2";
    case SC_ESPACIO: return sub == 0u ? "Reverb" : sub == 1u ? "Shimmer y delay" : "Delay 2";
    case SC_DERIVA: return sub == 0u ? "Deriva" : sub == 1u ? "Deriva: notas" : "Deriva: evoluci\363n";
    default: return SC_TITLE[sc];
    }
}

static void put4(cl_t *c, const uint8_t *p)
{
    uint32_t i;
    for (i = 0; i < 4u; i++) {
        c[i].kind = p[i] == 255 ? CL_NONE : CL_P;
        c[i].arg = p[i];
    }
}

/* the four cells of a page of a section for the layer; returns how many pages the section has */
static uint32_t ui_cells(uint32_t sc, uint32_t sub, uint32_t part, cl_t *c)
{
    int amb = gr_ambient(part);
    uint32_t i;
    for (i = 0; i < 4u; i++)
        c[i].kind = CL_NONE;
    switch (sc) {
    case SC_ESCENA:
        if (sub == 0u)
            put4(c, PG_MACRO);
        else if (amb)
            put4(c, sub == 1u ? PG_OSC2 : PG_OSC3);
        else
            put4(c, sub == 1u ? PG_GRAIN1 : PG_GRAIN2);
        return 3u;
    case SC_FUENTE:
        if (amb) {
            put4(c, PG_OSC1);
            return 1u;
        }
        {
            const gr_gen_t *g = &gr_gen[part];
            uint32_t n = GR_GENTYPES[g->type].n, pages;
            if (gr_src_kind[part] == 1u) {           /* a stored sample: only the choice */
                c[0].kind = CL_GTYPE;
                return 1u;
            }
            pages = n <= 2u ? 1u : 1u + (n - 2u + 3u) / 4u;
            if (sub == 0u) {
                c[0].kind = CL_GTYPE;
                c[1].kind = CL_GLEN;
                for (i = 0; i < 2u && i < n; i++) {
                    c[2u + i].kind = CL_GP;
                    c[2u + i].arg = (uint8_t)i;
                }
            } else {
                for (i = 0; i < 4u; i++) {
                    uint32_t k = 2u + (sub - 1u) * 4u + i;
                    if (k < n) {
                        c[i].kind = CL_GP;
                        c[i].arg = (uint8_t)k;
                    }
                }
            }
            return pages;
        }
    case SC_ESCALA: put4(c, PG_SCALE); return 1u;
    case SC_VOZ: put4(c, amb ? (sub ? PG_AVOICE2 : PG_AVOICE1) : (sub ? PG_VOICE2 : PG_VOICE1)); return 2u;
    case SC_MOV: put4(c, sub == 0u ? PG_FILTER : sub == 1u ? PG_LFO1 : PG_LFO2); return 3u;
    case SC_ESPACIO: put4(c, sub == 0u ? PG_REV : sub == 1u ? PG_DLY : PG_DLY2); return 3u;
    case SC_DERIVA: put4(c, sub == 0u ? PG_DRIFT1 : sub == 1u ? PG_DRIFT2 : PG_DRIFT3); return 3u;
    case SC_PRESETS:
        c[0].kind = CL_SLOT;
        c[1].kind = CL_LOAD;
        return 1u;
    default: return 1u;
    }
}

static uint32_t ui_npages(uint32_t sc)
{
    cl_t c[4];
    return ui_cells(sc, 0, ui.part, c);
}

/* -------------------------------------------------------------- cells --- */
typedef struct {
    const char *name;
    char value[28];
    int32_t norm, marker;
    uint8_t kind;                    /* CK_* */
    uint8_t count, index, edit, empty;
} cell_t;
enum { CK_DIAL, CK_BIDIAL, CK_CHOICE, CK_ACTION, CK_BIG };

static void cell_from_info(cell_t *c, const gr_pinfo_t *i, uint32_t norm)
{
    c->name = i->name;
    gr_info_fmt(c->value, i, norm);
    c->norm = (int32_t)norm;
    c->marker = -1;
    c->kind = i->unit == GR_U_CHOICE || i->unit == GR_U_DEST ? CK_CHOICE : i->unit == GR_U_CHAN ? CK_BIG :
              (i->mn < 0 && i->mx > 0 ? CK_BIDIAL : CK_DIAL);
    c->count = (uint8_t)(i->steps + 1u);
    c->index = (uint8_t)gr_info_index(i, norm);
}

/* the stored samples that exist, in order: the n-th of them, or -1 */
static int32_t ui_nth_slot(uint32_t n)
{
    uint32_t s;
    for (s = 0; s < NSLOT; s++)
        if ((smp_used >> s) & 1u) {
            if (n == 0u)
                return (int32_t)s;
            n--;
        }
    return -1;
}
static uint32_t ui_nslots(void)
{
    uint32_t s, n = 0;
    for (s = 0; s < NSLOT; s++)
        n += (smp_used >> s) & 1u;
    return n;
}

static void describe(const cl_t *cl, cell_t *c)
{
    uint32_t part = ui.part;
    memset(c, 0, sizeof *c);
    c->marker = -1;
    gr_dest_tab = gr_dest_of(part);
    switch (cl->kind) {
    case CL_P: {
        uint32_t p = cl->arg;
        cell_from_info(c, &GR_PINFO[p], gr_norm(part, p));
        if (p == P_AWAVE) {
            uint32_t w = gr_index(part, P_AWAVE);
            str_cpy(c->value, AMB_WAVE_NAMES[w & 31u], sizeof c->value);
            c->kind = CK_CHOICE;
            c->count = 32;
            c->index = (uint8_t)w;
        } else if (p == P_ASTRUCT) {
            str_cpy(c->value, gr_index(part, P_ASTRUCT) ? "Drone 2" : "Drone 1", sizeof c->value);
        }
        if (!gr_is_global(p) && gr_pt[part].off[p] != 0)
            c->marker = gr_pt[part].vnorm[p];
        else if (gr_is_global(p)) {
            int32_t off = gr_pt[0].off[p] + gr_pt[1].off[p] + gr_pt[2].off[p];
            if (off)
                c->marker = clampi((int32_t)gr_norm(0, p) + off, 0, 65535);
        }
        break;
    }
    case CL_GTYPE: {
        const gr_gen_t *g = &gr_gen[part];
        c->name = "Sonido";
        c->kind = CK_CHOICE;
        c->count = (uint8_t)(GEN_COUNT + ui_nslots());
        if (gr_src_kind[part] == 1u) {
            smp_hdr_t h;
            uint32_t s, n = 0;
            for (s = 0; s < gr_src_slot[part]; s++)
                n += (smp_used >> s) & 1u;
            c->index = (uint8_t)(GEN_COUNT + n);
            if (smp_hdr_read(gr_src_slot[part], &h) == 0)
                str_cpy(c->value, h.name, sizeof c->value);
            else
                str_cpy(c->value, "Muestra", sizeof c->value);
        } else {
            c->index = g->type;
            str_cpy(c->value, GR_GENTYPES[g->type].name, sizeof c->value);
        }
        break;
    }
    case CL_GLEN:
        cell_from_info(c, &GR_GENLEN, gr_gen[part].length);
        break;
    case CL_GP: {
        const gr_gen_t *g = &gr_gen[part];
        cell_from_info(c, &GR_GENTYPES[g->type].p[cl->arg], g->norm[g->type][cl->arg]);
        break;
    }
    case CL_SLOT:
        c->name = "Ranura";
        c->kind = CK_BIG;
        fmt_int(c->value, (int32_t)ui.slot + 1);
        break;
    case CL_LOAD:
        c->name = "Cargar";
        c->kind = CK_ACTION;
        str_cpy(c->value, ((ui.saved_mask >> ui.slot) & 1) ? "gira >" : "vac\355a", sizeof c->value);
        c->empty = !((ui.saved_mask >> ui.slot) & 1);
        break;
    default:
        c->empty = 1;
        c->name = "";
        break;
    }
}

static void draw_cell(int32_t slot, const cell_t *c)
{
    uint32_t key = 2166136261u;
    uint16_t lit = c->edit ? UC_CORAL : accent_of(ui.part);
    int32_t w = CELL_W, h = CELL_H, x = slot * CELL_W;
    char v[28];
    key = hash_str(key, c->name);
    key = hash_str(key, c->value);
    key = hash_add(key, (uint32_t)(c->norm >> 8));
    key = hash_add(key, (uint32_t)(c->marker < 0 ? 999 : c->marker >> 10) + ((uint32_t)c->kind << 12));
    key = hash_add(key, (uint32_t)c->edit + 2u * c->empty + 8u * ui.part + 64u * c->index);
    if (ui.cell_key[slot] == key)
        return;
    ui.cell_key[slot] = key;
    cv_begin((uint32_t)w, (uint32_t)h, UC_BG);
    if (c->empty && c->kind != CK_ACTION) {
        cv_dot(w / 2, 40, 1, UC_UNLIT);
        cv_blit((uint32_t)x, CELL_Y);
        return;
    }
    if (c->edit)
        cv_rrect(1, 0, w - 2, h - 1, 9, UC_PANEL, UC_BG);
    {
        uint16_t under = c->edit ? UC_PANEL : UC_BG;
        text_fit(v, sizeof v, c->name, &AF_S, w - 4);
        cv_text_in(0, 2, w, &AF_S, v, c->edit ? UC_TEXT : UC_MUTED, under);
        switch (c->kind) {
        case CK_DIAL: draw_dial(w / 2, 40, c->norm, 0, lit, c->marker); break;
        case CK_BIDIAL: draw_dial(w / 2, 40, c->norm, 1, lit, c->marker); break;
        case CK_CHOICE: draw_choice_dots(w / 2, 40, c->count, c->index, lit); break;
        case CK_BIG:
            cv_text_in(0, 24, w, &AF_L, c->value, lit, under);
            cv_blit((uint32_t)x, CELL_Y);
            return;
        case CK_ACTION: {
            uint16_t face = c->empty ? UC_PANEL : ux_mix(UC_BG, accent_of(ui.part), 40);
            cv_rrect(6, 26, w - 12, 30, 10, face, under);
            cv_text_in(6, 26 + CAP_IN(S, 30), w - 12, &AF_S, c->value, c->empty ? UC_UNLIT : UC_TEXT, face);
            cv_blit((uint32_t)x, CELL_Y);
            return;
        }
        default: break;
        }
        if (text_w(&AF_M, c->value) <= w - 4)         /* the value: big if it fits, else smaller */
            cv_text_in(0, h - 21, w, &AF_M, c->value, c->edit ? UC_CORAL : UC_TEXT, under);
        else {
            text_fit(v, sizeof v, c->value, &AF_S, w - 4);
            cv_text_in(0, h - 19, w, &AF_S, v, c->edit ? UC_CORAL : UC_TEXT, under);
        }
    }
    cv_blit((uint32_t)x, CELL_Y);
}

static void draw_cells(void)
{
    cl_t cl[4];
    uint32_t i, now = fm1_ms;
    ui_cells(ui.page, ui.sub[ui.page], ui.part, cl);
    for (i = 0; i < 4u; i++) {
        cell_t c;
        describe(&cl[i], &c);
        c.edit = cl[i].kind != CL_NONE && (now - ui.turned[i]) < 600u;
        draw_cell((int32_t)i, &c);
    }
}

static void draw_page_dots(void)
{
    uint32_t n = ui_npages(ui.page), sub = ui.sub[ui.page], key = n * 16u + sub + 256u * ui.part + 1u, i;
    int32_t x0;
    if (key == ui.dots_key)
        return;
    ui.dots_key = key;
    cv_begin(240, 8, UC_BG);
    x0 = 120 - ((int32_t)n - 1) * 7;
    if (n > 1u)
        for (i = 0; i < n; i++)
            cv_dot(x0 + (int32_t)i * 14, 4, i == sub ? 3 : 1, i == sub ? accent_of(ui.part) : UC_UNLIT);
    cv_blit(0, DOTS_Y);
}

/* ------------------------------------------------------------ status --- */
static void draw_battery(int32_t x, int32_t y)
{
    int32_t lv = batt_level(), i;
    uint16_t edge = lv == 0 ? UC_WARN : UC_MUTED;
    if (lv == 4) {                                    /* USB power: a plug */
        cv_rect(x + 1, y + 3, 8, 6, UC_MUTED);
        cv_rect(x + 3, y, 1, 3, UC_MUTED);
        cv_rect(x + 6, y, 1, 3, UC_MUTED);
        cv_rect(x + 4, y + 9, 2, 3, UC_MUTED);
        cv_text(x + 11, y - 1, &AF_S, "USB", UC_MUTED);
        return;
    }
    cv_rrect(x, y, 22, 12, 3, edge, UC_BG);
    cv_rrect(x + 1, y + 1, 20, 10, 2, UC_BG, edge);
    cv_rect(x + 22, y + 4, 2, 4, edge);
    for (i = 0; i < 3; i++)
        if (lv > i)
            cv_rect(x + 3 + i * 6, y + 3, 4, 6, lv == 1 ? UC_CORAL : UC_TEXT);
}

static void draw_status(void)
{
    uint32_t key = 2166136261u, midi_on;
    char det[40], sc[20];
    uint16_t acc = accent_of(ui.part);
    if (ui.midi_seen != gr_midi_activity) {
        ui.midi_seen = gr_midi_activity;
        ui.last_midi_ms = fm1_ms;
    }
    midi_on = (uint32_t)(fm1_ms - ui.last_midi_ms) < 120u;
    if (ui.notice_ms && (uint32_t)(fm1_ms - ui.notice_ms) > 2200u)
        ui.notice_ms = 0;
    if (ui.notice_ms)
        str_cpy(det, ui.notice, sizeof det);
    else if (gr_pt[ui.part].smp_state != SMP_NORMAL || ui.regen_ms)
        str_cpy(det, "Generando\205", sizeof det);
    else
        str_cpy(det, ui_page_name(ui.page, ui.sub[ui.page], ui.part), sizeof det);
    str_cpy(sc, GR_CH_KEY[gr_index(0, P_KEY)], sizeof sc);
    gr_cat(sc, " ");
    {
        char m[8];
        str_cpy(m, GR_CH_MODE[gr_index(0, P_MODE)], 6);                 /* "Mixol" */
        gr_cat(sc, m);
    }
    key = hash_str(key, det);
    key = hash_str(key, sc);
    key = hash_add(key, ui.part + 4u * dr_on + 8u * gr_freeze + 16u * midi_on + 32u * (uint32_t)(batt_level() + 1) +
                            256u * (ui.notice_ms != 0) + 512u * ((ui.frame >> 4) & 1u) * dr_on);
    if (key == ui.status_key)
        return;
    ui.status_key = key;
    cv_begin(240, STATUS_H, UC_BG);
    {   /* the layer */
        char l[2] = {layer_letter(ui.part), 0};
        cv_rrect(3, 3, 20, 18, 6, acc, UC_BG);
        cv_text_in(3, 3 + CAP_IN(M, 18), 20, &AF_M, l, UC_BG, acc);
    }
    cv_text_fit(29, 3, &AF_M, det, ui.notice_ms ? UC_CORAL : UC_TEXT, UC_BG, (dr_on || gr_freeze) ? 102 : 124);
    cv_text_r(dr_on || gr_freeze ? 176 : 204, 6, &AF_S, sc, UC_MUTED, UC_BG);
    if (dr_on) {                                       /* the Deriva: a play triangle that breathes */
        uint16_t c = ((ui.frame >> 4) & 1u) ? UC_CORAL : ux_mix(UC_BG, UC_CORAL, 55);
        int32_t i;
        for (i = 0; i < 7; i++)
            cv_rect(181 + i, 6 + i / 2, 1, 12 - (i / 2) * 2, c);
    }
    if (gr_freeze) {                                   /* the Freeze: a snowflake */
        cv_rect(192, 11, 11, 1, UC_ICE);
        cv_rect(197, 6, 1, 11, UC_ICE);
        cv_line(193, 7, 201, 15, UC_ICE);
        cv_line(201, 7, 193, 15, UC_ICE);
    }
    if (midi_on)
        cv_dot(206, 20, 1, UC_CORAL);
    draw_battery(212, 6);
    cv_blit(0, 0);
}

/* ---------------------------------------------------------- pictures --- */
/* All pictures are drawn in two strips of 64 rows (the canvas holds 240 x 96): a strip function draws as if the canvas
 * were the whole picture, cv_oy shifts it to its rows. */
static void pic_begin(int32_t top)
{
    cv_begin(240, 64, UC_BG);
    cv_oy = -top;
}
static void pic_end(int32_t top)
{
    cv_oy = 0;
    cv_blit(0, (uint32_t)(PIC_Y + top));
}

static const char *const NOTE_NAMES[12] = {"C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"};
static void note_name(char *d, uint32_t n)
{
    str_cpy(d, NOTE_NAMES[n % 12u], 4);
    fmt_int(d + str_len(d), (int32_t)n / 12 - 1);
}

/* ESCENA: the sound as a cloud. Each grain that plays is a glowing point at its place in its sound (x) on its
 * layer's band; the drone is a slow wave through the middle; the FREEZE turns it all to ice. The waveform of the
 * selected layer lies faintly behind, the Deriva's notes are written at the bottom. */
static void scene_strip(int32_t top)
{
    uint32_t i, t = ui.frame;
    int32_t x, y;
    pic_begin(top);
    {
        const gr_part_t *pt = &gr_pt[ui.part < NGPART ? ui.part : 0];
        if (pt->smp.frames)
            for (x = 2; x < 240; x += 4) {
                int32_t h = pt->smp.peaks[x] * 50 / 255;
                for (y = 64 - h; y <= 64 + h; y += 4)
                    cv_pset(x, y, ux_mix(UC_BG, UC_UNLIT, 55));
            }
    }
    if (gr_st.notes[NGPART]) {
        int32_t amp = 10 + (int32_t)(gr_pt[NGPART].vnorm[P_AHARM] >> 12);
        uint16_t c = gr_freeze ? UC_ICE : UC_LILAC;
        for (x = 0; x < 240; x += 2) {
            uint32_t ph = (uint32_t)x * 0x01000000u + t * 0x00600000u;
            int32_t yy = 64 + ((sin32(ph) * amp) >> 15) + ((sin32(ph * 3u + t * 0x00900000u) * 5) >> 15);
            cv_rect(x, yy, 2, 2, ux_mix(UC_BG, c, 70));
            cv_pset(x, yy + 3, ux_mix(UC_BG, c, 25));
        }
    }
    for (i = 0; i < NGRAIN; i++) {
        uint16_t v = gr_st.gview[i], c;
        uint32_t part = gr_st.gpart[i];
        int32_t band;
        if (v == GVIEW_NONE)
            continue;
        x = (int32_t)((uint32_t)v * 239u / 65534u);
        band = part == 0u ? 38 : 90;
        y = band + (int32_t)((i * 2654435761u) >> 27) - 16 + ((sin32((t << 25) + i * 0x1F000000u) * 4) >> 15);
        c = gr_freeze ? UC_ICE : accent_of(part);
        cv_glow(x, y, 9, c, 30);
        cv_dot(x, y, 2, ux_mix(c, UC_TEXT, 40));
    }
    if (dr_on && top >= 64) {
        uint32_t n = 0, k;
        char line[40], nm[6];
        line[0] = 0;
        for (k = 0; k < DR_NOTES && n < 5u; k++)
            if (dr_n[k].left) {
                note_name(nm, dr_n[k].note);
                if (n)
                    gr_cat(line, "  ");
                gr_cat(line, nm);
                n++;
            }
        cv_text(6, 110, &AF_S, n ? line : "\205", UC_MUTED);
    }
    if (gr_freeze && top == 0)
        cv_text_r(234, 4, &AF_S, "FREEZE", UC_ICE, UC_BG);
    if (!gr_pt[0].smp.frames && !gr_pt[1].smp.frames && top == 0)
        cv_text_in(0, 24, 240, &AF_S, "EDIT: elige un sonido", UC_MUTED, UC_BG);
    pic_end(top);
}

/* FUENTE: the layer's sound, big, its name over it. The drone: one cycle of its wave. */
static void source_strip(int32_t top)
{
    uint32_t part = ui.part;
    uint16_t acc = accent_of(part);
    int32_t x, y, mid = 70;
    pic_begin(top);
    if (!gr_ambient(part)) {
        const gr_part_t *pt = &gr_pt[part];
        char nm[28];
        if (gr_src_kind[part] == 1u)
            str_cpy(nm, pt->smp.name, sizeof nm);
        else
            str_cpy(nm, GR_GENTYPES[gr_gen[part].type].name, sizeof nm);
        for (x = 1; x < 240 && pt->smp.frames; x += 3) {
            int32_t h = pt->smp.peaks[x] * 44 / 255;
            for (y = 0; y <= h; y += 3) {
                uint16_t col = ux_mix(UC_BG, acc, 100 - 60 * y / (h + 1));
                cv_rect(x, mid - y, 2, 2, col);
                if (y)
                    cv_rect(x, mid + y, 2, 2, col);
            }
        }
        cv_text_in(0, 4, 240, &AF_M, nm, UC_TEXT, UC_BG);
        if (gr_src_kind[part] != 1u)
            cv_text_in(0, 116, 240, &AF_S, "Se rehace al dejar de girar", UC_UNLIT, UC_BG);
    } else {
        const amb_part_t *a = &amb[part - NGPART];
        int32_t hp = (int32_t)gr_pt[part].vnorm[P_AHARM] * 3, f = hp >> 16, fr = hp & 0xFFFF;
        for (x = 1; x < 240; x += 2) {
            uint32_t k = (uint32_t)x * AMB_N / 240u;
            int32_t s = a->tab[a->cur][f][k] + (int32_t)(((int64_t)(a->tab[a->cur][f < 3 ? f + 1 : 3][k] - a->tab[a->cur][f][k]) * fr) >> 16);
            int32_t v = s * 46 / 32768, d = v < 0 ? -1 : 1;
            for (y = 0; y * d <= v * d; y += d * 2)
                cv_rect(x, mid - y, 2, 2, ux_mix(UC_BG, acc, 100 - (y < 0 ? -y : y) * 50 / 47));
        }
        cv_text_in(0, 4, 240, &AF_M, AMB_WAVE_NAMES[gr_index(part, P_AWAVE) & 31u], UC_TEXT, UC_BG);
    }
    pic_end(top);
}

/* ESCALA: twelve semitones from the tonic, the scale lit and numbered, the tonic in coral, and its name */
static void scale_strip(int32_t top)
{
    static const uint8_t BLACK[12] = {0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0};
    uint32_t key = gr_index(0, P_KEY), mode = gr_index(0, P_MODE), n = GR_MODE_N[mode], i, k;
    char name[32];
    pic_begin(top);
    str_cpy(name, GR_CH_KEY[key], sizeof name);
    gr_cat(name, " ");
    gr_cat(name, GR_CH_MODE[mode]);
    cv_text_in(0, 4, 240, &AF_L, name, UC_TEXT, UC_BG);
    for (i = 0; i < 12u; i++) {
        uint32_t note = (key + i) % 12u, in = 0, deg = 0;
        int32_t x = 6 + (int32_t)i * 19, h = BLACK[note] ? 46 : 62;
        uint16_t c, face;
        for (k = 0; k < n; k++)
            if (GR_MODE_DEG[mode][k] == i) {
                in = 1;
                deg = k + 1u;
            }
        c = !in ? (BLACK[note] ? UC_PANEL : UC_UNLIT) : i == 0u ? UC_CORAL : UC_SAND;
        face = in ? ux_mix(UC_BG, c, 85) : c;
        cv_rrect(x, 50, 16, h, 4, face, UC_BG);
        cv_text_in(x - 2, 50 + 62 + 2, 20, &AF_S, NOTE_NAMES[note], in ? UC_TEXT : UC_UNLIT, UC_BG);
        if (in) {
            char d[4];
            fmt_int(d, (int32_t)deg);
            cv_text_in(x, 50 + h - 17, 16, &AF_S, d, UC_BG, face);
        }
    }
    pic_end(top);
}

/* VOZ: the envelope */
static void env_strip(int32_t top)
{
    uint32_t part = ui.part, pa = gr_ambient(part) ? P_AATK : P_ATTACK, pr = gr_ambient(part) ? P_AREL : P_RELEASE;
    uint32_t a = gr_norm(part, pa), r = gr_norm(part, pr), lv = gr_norm(part, P_LEVEL);
    int32_t left = 10, right = 230, base = 118, aend = left + (int32_t)(a * 90u >> 16) + 3, rstart = right - (int32_t)(r * 110u >> 16) - 3;
    int32_t peak = 80 * (25 + 75 * (int32_t)lv / 65536) / 100, x, y;
    uint16_t acc = accent_of(part);
    pic_begin(top);
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
        for (y = 0; y <= h; y += 3)
            cv_rect(x, base - y, 2, 2, y + 3 > h ? acc : ux_mix(UC_BG, acc, (28 + 72 * y / (h + 1)) * 6 / 10));
    }
    {
        char t[24];
        gr_fmt(t, part, pa);
        cv_text(left, 8, &AF_S, t, UC_MUTED);
        gr_fmt(t, part, pr);
        cv_text_r(right, 8, &AF_S, t, UC_MUTED, UC_BG);
    }
    pic_end(top);
}

/* MOVIMIENTO: a sketch of the filter's response and the two LFOs moving */
static void mov_strip(int32_t top)
{
    uint32_t part = ui.part, ft = gr_index(part, P_FTYPE), k;
    int32_t cut = (int32_t)(gr_pt[part].vnorm[P_CUTOFF] * 200u >> 16) + 20, reso = (int32_t)(gr_norm(part, P_RESO) >> 11), x;
    uint16_t acc = accent_of(part);
    pic_begin(top);
    for (x = 10; x < 230; x += 2) {
        int32_t d = x - cut, y;
        if (ft == 0u)
            y = 30;
        else if (ft == 4u)
            y = d < 0 ? 30 - d / 3 : 30;
        else if (ft == 3u)
            y = 30 + absi(d) / 3;
        else
            y = d > 0 ? 30 + d / 2 : 30;
        if (ft && absi(d) < 8)
            y -= reso * (8 - absi(d)) / 8;
        y = clampi(y, 6, 62);
        cv_rect(x, y, 2, 2, acc);
        if (y < 62)
            cv_rect(x, y + 3, 2, 62 - y - 3, ux_mix(UC_BG, acc, 14));
    }
    for (k = 0; k < 2u; k++) {
        int32_t v = gr_st.lfo[part][k], yy = 84 + (int32_t)k * 26;
        uint32_t dest = gr_index(part, k ? P_L2DEST : P_L1DEST);
        cv_rect(10, yy, 220, 1, UC_UNLIT);
        cv_text(10, yy - 18, &AF_S, k ? "LFO 2" : "LFO 1", UC_MUTED);
        if (dest) {
            if (v >= 0)
                cv_rect(120, yy, v / 300, 1, acc);
            else
                cv_rect(120 + v / 300, yy, -v / 300, 1, acc);
            cv_dot(120 + v / 300, yy, 4, acc);
        }
    }
    pic_end(top);
}

/* ESPACIO: the echoes, the reverb's cloud and the shimmer's sparks rising */
static void space_strip(int32_t top)
{
    int32_t time = (int32_t)gr_norm(0, P_DLYTIME), fb = gr_real(0, P_DLYFB), decay = (int32_t)gr_norm(0, P_REVDECAY);
    int32_t dsend = gr_real(ui.part, P_DLYSEND), rsend = gr_real(ui.part, P_REVSEND), shim = gr_pt[0].val[P_SHIMMER];
    uint16_t acc = accent_of(ui.part), cloud;
    int32_t i, x, step, level, side = 0, cloud_end = 90 + (decay * 140 >> 16);
    uint32_t seed = 0x2F6E2B1u;
    pic_begin(top);
    cloud = ux_mix(ux_mix(UC_BG, UC_UNLIT, 70), gr_freeze ? UC_ICE : acc, clampi(15 + rsend / 20, 0, 100));
    for (i = 0; i < 700; i++) {
        int32_t t, sy;
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        t = (int32_t)(seed & 1023u);
        sy = (int32_t)((seed >> 10) & 1023u) - 512;
        x = 8 + (t * t >> 10) * (cloud_end - 8) / 1024;
        if (x < 238)
            cv_rect(x, 64 + sy * 110 / 1024 * (100 - 60 * t / 1024) / 100, 2, 2, ux_mix(cloud, UC_BG, t * 100 / 1024));
    }
    if (shim > 20)
        for (i = 0; i < 24; i++) {
            uint32_t h = (uint32_t)i * 2654435761u;
            int32_t sx = 20 + (int32_t)(h >> 24) * 200 / 256, sy = 120 - (int32_t)((ui.frame * (1u + (h & 3u)) + (h >> 8)) % 120u);
            if (i < shim / 40)
                cv_dot(sx, sy, 1, ux_mix(UC_BG, UC_TEXT, 50 + (int32_t)(h & 31u)));
        }
    step = 16 + (time * 46 >> 16);
    level = 70 + 30 * dsend / 1000;
    cv_dot(10, 64, 5, UC_TEXT);
    for (x = 10 + step; x < 234 && level > 5; x += step) {
        cv_dot(x, side ? 80 : 48, 1 + level * 6 / 100, dsend > 2 ? ux_mix(UC_BG, acc, 25 + 75 * level / 100) : UC_UNLIT);
        side ^= 1;
        level = level * (35 + fb * 70 / 1000) / 100;
    }
    pic_end(top);
}

/* DERIVA: the walk as a roll that scrolls: the last notes (pitch up, the last 40 s), what sounds now glowing */
static void drift_strip(int32_t top)
{
    uint32_t now = dr_clock, i, nh = dr_hist_n, span = 345u * 40u;
    int32_t lo = sc_note(0, gr_real(0, P_DOCT)), hi = sc_note(gr_real(0, P_DRANGE) * (int32_t)GR_MODE_N[gr_index(0, P_MODE)] + 6,
                                                                   gr_real(0, P_DOCT));
    pic_begin(top);
    for (i = 0; i < 4u; i++)
        cv_rect(0, 16 + (int32_t)i * 30, 240, 1, ux_mix(UC_BG, UC_UNLIT, 40));
    for (i = 0; i < DR_HIST && i < nh; i++) {
        uint32_t h = (nh - 1u - i) % DR_HIST, age = now - dr_hist[h].t, len = dr_hist[h].len;
        int32_t x0 = 236 - (int32_t)((uint64_t)age * 236u / span), x1, y;
        uint16_t c = accent_of(dr_hist[h].part);
        int sounding = age < len;
        if (age > span + len)
            continue;
        x1 = sounding ? 236 : 236 - (int32_t)((uint64_t)(age - len) * 236u / span);
        if (x0 < 0)
            x0 = 0;
        y = clampi(120 - (int32_t)(dr_hist[h].note - lo) * 104 / (hi > lo ? hi - lo : 1), 8, 122);
        if (x1 >= x0)
            cv_rect(x0, y - 2, x1 - x0 + 1, 5, ux_mix(UC_BG, c, sounding ? 90 : 45));
        if (sounding)
            cv_glow(x1, y, 8, c, 50);
    }
    if (!dr_on)
        cv_text_in(0, 54, 240, &AF_M, "PLAY: empezar la deriva", UC_MUTED, UC_BG);
    pic_end(top);
}

/* SISTEMA: the battery, the load, the MIDI channels, the version */
static void system_strip(int32_t top)
{
    char b[32];
    int32_t lv = batt_level(), pct = batt_pct();
    pic_begin(top);
    cv_text(10, 2, &AF_M, "amb-iguos", UC_TEXT);
    cv_text(10, 22, &AF_S, FELUCCA_VERSION, UC_UNLIT);
    cv_text(10, 46, &AF_S, "Bater\355a", UC_MUTED);
    if (lv == 4)
        str_cpy(b, "con USB", sizeof b);
    else if (pct < 0)
        str_cpy(b, "\205", sizeof b);
    else {
        fmt_int(b, pct);
        gr_cat(b, " %");
        if (lv == 0)
            gr_cat(b, "  carga ya");
    }
    cv_text(96, 44, &AF_M, b, lv == 0 ? UC_WARN : UC_TEXT);
    cv_text(10, 70, &AF_S, "CPU", UC_MUTED);
    fmt_int(b, (int32_t)(gr_st.load * 100u / 255u));
    gr_cat(b, " %   granos ");
    fmt_int(b + str_len(b), gr_st.grains);
    cv_text(96, 68, &AF_M, b, UC_TEXT);
    cv_text(10, 94, &AF_S, "MIDI A/B/D", UC_MUTED);
    fmt_int(b, (int32_t)gr_index(0, P_MIDICH) + 1);
    gr_cat(b, " / ");
    fmt_int(b + str_len(b), (int32_t)gr_index(1, P_MIDICH) + 1);
    gr_cat(b, " / ");
    fmt_int(b + str_len(b), (int32_t)gr_index(2, P_MIDICH) + 1);
    cv_text(96, 92, &AF_M, b, UC_TEXT);
    cv_text(10, 114, &AF_S, "OCT- y OCT+ 5 s: actualizar", UC_UNLIT);
    pic_end(top);
}

/* PRESETS: sixteen slots, the saved ones marked */
static void presets_strip(int32_t top)
{
    uint32_t i;
    uint16_t acc = accent_of(ui.part);
    pic_begin(top);
    for (i = 0; i < NPRESET; i++) {
        int32_t cx = 30 + (int32_t)(i % 4u) * 60, cy = 18 + (int32_t)(i / 4u) * 26, filled = (ui.saved_mask >> i) & 1;
        char num[4];
        fmt_int(num, (int32_t)i + 1);
        if (i == ui.slot) {
            cv_rrect(cx - 26, cy - 11, 52, 22, 8, filled ? acc : UC_PANEL, UC_BG);
            cv_text_in(cx - 26, cy - 11 + CAP_IN(M, 22), 52, &AF_M, num, filled ? UC_BG : acc, filled ? acc : UC_PANEL);
        } else {
            cv_text_in(cx - 26, cy - 11 + CAP_IN(S, 22), 52, &AF_S, num, filled ? UC_TEXT : UC_UNLIT, UC_BG);
            if (filled)
                cv_dot(cx + 14, cy, 2, ux_mix(UC_BG, acc, 60));
        }
    }
    cv_text_in(0, 114, 240, &AF_S, ui.confirm == 1 ? "SAVE otra vez: sobrescribir" : "SAVE: guardar aqu\355",
               ui.confirm == 1 ? UC_CORAL : UC_MUTED, UC_BG);
    pic_end(top);
}

/* a key per picture: the moving ones change it with the clock, the others with what they show */
static uint32_t pic_key(void)
{
    uint32_t k = 2166136261u, p = ui.part, i;
    k = hash_add(k, ui.page + 16u * p + 256u * ui.sub[ui.page] + 4096u * gr_freeze + 8192u * dr_on);
    switch (ui.page) {
    case SC_ESCENA: return hash_add(k, ui.frame);
    case SC_MOV: return hash_add(k, ui.frame >> 1);
    case SC_DERIVA: return hash_add(k, dr_clock >> 6);
    case SC_ESPACIO:
        for (i = 0; i < P_COUNT; i++)
            k = hash_add(k, gr_norm(p, i));
        return gr_pt[0].val[P_SHIMMER] > 20 ? hash_add(k, ui.frame >> 1) : k;
    case SC_SISTEMA: return hash_add(k, (fm1_ms >> 10) + gr_st.load * 7u + (uint32_t)batt_raw * 131u);
    case SC_PRESETS: return hash_add(k, ui.saved_mask + 65536u * ui.slot + ui.confirm * 77u);
    case SC_FUENTE:
        k = hash_add(k, gr_pt[p].smp.frames + gr_pt[p].smp.peaks[60] * 7u + gr_pt[p].smp.peaks[180] * 131u + gr_src_kind[p] * 3u);
        k = hash_add(k, gr_gen[p].type + (p >= NGPART ? gr_pt[p].vnorm[P_AHARM] * 3u + gr_norm(p, P_AWAVE) + amb[p - NGPART].built : 0u));
        return k;
    default:
        for (i = 0; i < P_COUNT; i++)
            k = hash_add(k, gr_norm(p, i));
        return k;
    }
}

static void draw_picture(void)
{
    uint32_t key = pic_key();
    int32_t top;
    if (key == ui.pic_key)
        return;
    ui.pic_key = key;
    for (top = 0; top < PIC_H; top += 64) {
        switch (ui.page) {
        case SC_ESCENA: scene_strip(top); break;
        case SC_FUENTE: source_strip(top); break;
        case SC_ESCALA: scale_strip(top); break;
        case SC_VOZ: env_strip(top); break;
        case SC_MOV: mov_strip(top); break;
        case SC_ESPACIO: space_strip(top); break;
        case SC_DERIVA: drift_strip(top); break;
        case SC_SISTEMA: system_strip(top); break;
        default: presets_strip(top); break;
        }
    }
}

/* ------------------------------------------------------------- draw --- */
static void ui_draw(void)
{
    ui.frame++;
    if (ui.force || ui.page != ui.last_page || ui.part != ui.last_part || ui.sub[ui.page] != ui.last_sub) {
        lcd_fill(0, 0, 240, 240, UC_BG);
        memset(ui.cell_key, 0, sizeof ui.cell_key);
        ui.status_key = ui.pic_key = ui.dots_key = 0;
        ui.force = 0;
        ui.last_page = ui.page;
        ui.last_part = ui.part;
        ui.last_sub = ui.sub[ui.page];
    }
    draw_status();
    draw_picture();
    draw_page_dots();
    draw_cells();
}

/* the FM-1 UI's text and blending code takes its tokens from the palette tables; amb-iguos draws with its own
 * colours (UC_*), so only the blending mode matters here (colour, flat) */
static void ui_theme_init(void)
{
    palette_set(0);
    ux.mono = 0;
    ux.style = ST_FLAT;
}
