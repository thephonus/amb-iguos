/* SPDX-License-Identifier: GPL-3.0-only
 * The screens of the Drone and Pad parts (included by gr_ui.c). They use the same pages as the granular parts, with
 * other dials: Grain becomes the oscillator page (the wave, its harmonic position, the structure, pitch, detune,
 * balance and the MOD LFO) and Voice the envelope, the voice mode and the part's level and pan. */
static const char *amb_structure_name(uint32_t part, uint32_t idx)
{
    static const char *const N[4] = {"Drone 1", "Drone 2", "Pad 1", "Pad 2"};
    return N[(gr_kind[part] == KIND_DRONE ? 0u : 2u) + (idx & 1u)];
}

/* a cell of an ambient page: the wave and the structure get words */
static void amb_fix_cell(uint32_t p, cell_t *c)
{
    if (p == P_AWAVE) {
        uint32_t w = gr_index(ui.part, P_AWAVE);
        fmt_int(c->value, (int32_t)w + 1);
        gr_cat(c->value, " ");
        gr_cat(c->value, AMB_WAVE_NAMES[w & 31u]);
    } else if (p == P_ASTRUCT) {
        str_cpy(c->value, amb_structure_name(ui.part, gr_index(ui.part, P_ASTRUCT)), sizeof c->value);
    }
}

/* what the status bar says about the part's sound: the wave */
static void amb_detail(char *d, uint32_t n) { str_cpy(d, AMB_WAVE_NAMES[gr_index(ui.part, P_AWAVE) & 31u], n); }

/* the picture of the oscillator page: one cycle of the wave at the HARMONIC position, stippled; a fainter one moved by
 * the MOD (the Yin and Yang oscillators' positions), the position line, and the notes' phases as grains */
static void picture_wave(void)
{
    uint32_t part = ui.part, key = 2166136261u, x;
    const amb_part_t *a = &amb[part - NGPART];
    int32_t hp = (int32_t)gr_pt[part].vnorm[P_AHARM] * 3, mid = PIC_H / 2, off = 0;
    uint16_t acc = accent_of(part);
    uint32_t f, fr;
    int32_t depth = (gr_pt[part].val[P_AMODDEPTH] * 3 * 32768) / 1000, i, y;
    int16_t cyc[AMB_N], cyc2[AMB_N];
    if (gr_index(part, P_ASTRUCT) || gr_kind[part] == KIND_PAD)
        off = mulq15(depth, 32768) * 1;                                      /* the Yin / Yang spread of a full MOD */
    off = clampi(off, 0, 98304);
    for (i = 0; i < 2; i++) {
        int32_t h = clampi(i ? hp - off : hp + off, 0, 3 * 65536 - 1);
        int16_t *dst = i ? cyc2 : cyc;
        uint32_t k;
        f = (uint32_t)h >> 16;
        fr = (uint32_t)h & 0xFFFFu;
        for (k = 0; k < AMB_N; k++)
            dst[k] = (int16_t)(a->tab[a->cur][f][k] + (int32_t)(((int64_t)(a->tab[a->cur][f + 1u][k] - a->tab[a->cur][f][k]) * (int32_t)fr) >> 16));
    }
    key = hash_add(key, (uint32_t)hp + (uint32_t)off * 7u + a->built * 131u + part);
    {
        uint32_t k;
        for (k = 0; k < AMB_N; k += 5u)
            key = hash_add(key, (uint32_t)(cyc[k] + 32768));
    }
    for (i = 0; i < (int32_t)AMB_V; i++)                                    /* the sounding notes lift the key: it redraws */
        key = hash_add(key, (uint32_t)(a->v[i].ph[0][0] >> 28) + a->v[i].active * 17u);
    if (key == ui.pic_key)
        return;
    ui.pic_key = key;
    cv_begin(240, PIC_H, UC_BG);
    for (x = 1; x < 240u; x += 3u) {
        uint32_t k = x * AMB_N / 240u;
        int32_t v = cyc[k] * (mid - 4) / 32768, v2 = cyc2[k] * (mid - 4) / 32768;
        if (off) {
            y = v2 < 0 ? -1 : 1;
            for (i = 0; i * y <= v2 * y; i += y * 3)
                cv_rect((int32_t)x, mid - i, 1, 1, ux_mix(UC_BG, acc, 22));
        }
        y = v < 0 ? -1 : 1;
        for (i = 0; i * y <= v * y; i += y * 2)
            cv_rect((int32_t)x, mid - i, 2, 2, ux_mix(UC_BG, acc, 100 - (i < 0 ? -i : i) * 45 / (mid - 3)));
        cv_rect((int32_t)x, mid, 2, 1, UC_UNLIT);
    }
    cv_blit(0, STATUS_H + 4);
}

static void draw_not_granular(const char *what)
{
    uint32_t key = 7u + ui.page * 31u + ui.part;
    if (key == ui.list_key)
        return;
    ui.list_key = key;
    cv_begin(240, 80, UC_BG);
    cv_text_in(0, 14, 240, &AF_S, what, UC_TEXT, UC_BG);
    cv_text_in(0, 36, 240, &AF_S, "belongs to the granular parts (1 and 2).", UC_MUTED, UC_BG);
    cv_text_in(0, 58, 240, &AF_S, "Press ARP to pick one.", UC_MUTED, UC_BG);
    cv_blit(0, STATUS_H + 40);
}
