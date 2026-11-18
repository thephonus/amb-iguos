#ifndef DEMO_AUDIO
#define DEMO_AUDIO stdout
#endif
static int16_t demo_smp[NGPART][SMP_FRAMES + 1];

static void setn(uint32_t part, uint32_t p, int32_t n) { gr_set_norm(part, p, n < 0 ? 0 : n > 65535 ? 65535 : n); }
static void setf(uint32_t part, uint32_t p, double f) { setn(part, p, (int32_t)(f * 65535.0)); }
static void gen_sound(uint32_t part, uint32_t type)
{
    gr_gen[part].type = (uint8_t)type;
    gr_sample_begin(part);
    str_cpy(gr_pt[part].smp.name, GR_GENTYPES[type].name, 16);
    gr_sample_end(part, gr_gen_render(part, demo_smp[part]), GEN_RATE);
}
#ifndef DEMO_HOOK
#define DEMO_HOOK(b, blocks) ((void)0)
#endif
static uint32_t demo_block;                 /* blocks since the scene began */
static void emit(uint32_t blocks, void (*tick)(uint32_t b))
{
    uint32_t b, k;
    for (b = 0; b < blocks; b++) {
        int32_t l[128], r[128];
        int16_t o[256];
        if (tick) tick(b);
        DEMO_HOOK(demo_block, blocks);
        amb_poll();
        seq_advance();
        gr_render(l, r);
        for (k = 0; k < 128; k++) { o[2 * k] = (int16_t)l[k]; o[2 * k + 1] = (int16_t)r[k]; }
        fwrite(o, 2, 256, DEMO_AUDIO);
        demo_block++;
    }
}
static uint32_t secs(double s) { return (uint32_t)(s * 44100.0 / 128.0); }
static void fresh(void)
{
    uint32_t p;
    gr_params_defaults();
    seq_init();
    for (p = 0; p < NPART; p++) { gr_all_sound_off(p); setf(p, P_DLYSEND, 0.12); setf(p, P_REVSEND, 0.35); }
    seq_clear_track(0); seq_clear_track(1); seq_clear_track(2); seq_clear_track(3);
}

typedef struct { double t; uint32_t part, note, on; } ev_t;
static const ev_t *evp; static uint32_t nevp;
static void play_events(uint32_t b)
{
    uint32_t i;
    for (i = 0; i < nevp; i++)
        if (secs(evp[i].t) == b) { if (evp[i].on) gr_note_on(evp[i].part, evp[i].note, 100); else gr_note_off(evp[i].part, evp[i].note); }
}
#define EV(t, p, n, on) {t, p, n, on}

/* ---- one generator: a short phrase on part 1 */
static void tick_gen(uint32_t b) { play_events(b); }
static void scene_gen(uint32_t type)
{
    static ev_t e[16]; uint32_t n = 0;
    uint32_t notes[3] = {57, 60, 64}, i;
    fresh();
    gen_sound(0, type);
    setf(0, P_REVSEND, 0.3); setf(0, P_DLYSEND, 0.2);
    for (i = 0; i < 3; i++) { e[n++] = (ev_t)EV(0.05 + 0.7 * i, 0, notes[i], 1); e[n++] = (ev_t)EV(0.05 + 0.7 * i + 1.6, 0, notes[i], 0); }
    evp = e; nevp = n;
    emit(secs(4.2), tick_gen);
}

/* ---- granular, a progression with the position and the scan moving */
static void tick_gran(uint32_t b)
{
    double t = (double)b * 128.0 / 44100.0;
    static const uint8_t ch[4][3] = {{57, 60, 64}, {53, 57, 60}, {48, 55, 64}, {55, 59, 62}};
    uint32_t beat = (uint32_t)(t / 3.5), k;
    if (b == 0 || (double)(b - 1) * 128.0 / 44100.0 / 3.5 < (double)beat && t / 3.5 >= (double)beat) {
        if (beat < 4) {
            if (beat) for (k = 0; k < 3; k++) gr_note_off(0, ch[beat - 1][k]);
            for (k = 0; k < 3; k++) gr_note_on(0, ch[beat][k], 90);
        } else if (beat == 4) for (k = 0; k < 3; k++) gr_note_off(0, ch[3][k]);
    }
    setf(0, P_POSITION, 0.15 + 0.6 * (t / 14.0));
    setf(0, P_SCAN, 0.5 + 0.2 * sin(t * 0.7));
    setf(0, P_SIZE, 0.35 + 0.25 * sin(t * 0.5));
    setf(0, P_SPREAD, 0.1 + 0.4 * (t / 14.0));
}
static void scene_granular(void)
{
    fresh();
    gen_sound(0, 2);
    setf(0, P_ATTACK, 0.45); setf(0, P_RELEASE, 0.6); setf(0, P_REVSEND, 0.45);
    emit(secs(15.0), tick_gran);
}

/* ---- drone and pad: held chords, the wave morphing */
static void tick_amb(uint32_t b)
{
    double t = (double)b * 128.0 / 44.1e3 / 1.0;
    uint32_t part = 2 + (uint32_t)(evp == 0), k;
    (void)part;
    play_events(b);
    for (k = 2; k < 4; k++) {
        setf(k, P_AHARM, 0.5 + 0.5 * sin(t * 0.35 + (double)k));
        setf(k, P_ADETUNE, 0.25 + 0.2 * sin(t * 0.21));
    }
}
static void scene_drone(void)
{
    static const ev_t e[] = {EV(0.1, 2, 45, 1), EV(0.1, 2, 52, 1), EV(0.2, 2, 57, 1), EV(0.2, 2, 60, 1), EV(5.0, 2, 45, 0), EV(5.0, 2, 52, 0), EV(5.0, 2, 57, 0), EV(5.0, 2, 60, 0),
        EV(5.2, 2, 41, 1), EV(5.2, 2, 48, 1), EV(5.3, 2, 57, 1), EV(5.3, 2, 64, 1), EV(10.5, 2, 41, 0), EV(10.5, 2, 48, 0), EV(10.5, 2, 57, 0), EV(10.5, 2, 64, 0)};
    fresh();
    evp = e; nevp = sizeof e / sizeof e[0];
    setf(2, P_REVSEND, 0.5);
    emit(secs(16.0), tick_amb);
}
static void scene_pad(void)
{
    static const ev_t e[] = {EV(0.1, 3, 57, 1), EV(0.1, 3, 64, 1), EV(0.2, 3, 72, 1), EV(5.0, 3, 57, 0), EV(5.0, 3, 64, 0), EV(5.0, 3, 72, 0),
        EV(5.2, 3, 53, 1), EV(5.2, 3, 60, 1), EV(5.3, 3, 69, 1), EV(10.5, 3, 53, 0), EV(10.5, 3, 60, 0), EV(10.5, 3, 69, 0)};
    fresh();
    evp = e; nevp = sizeof e / sizeof e[0];
    gr_set_norm(3, P_AWAVE, 20000);
    setf(3, P_REVSEND, 0.55);
    emit(secs(16.0), tick_amb);
}

/* ---- the sequencer: all four parts, a gliding lock on the drone's harmonics, long steps and gates over 100 % */
static void put_chord(uint32_t part, uint32_t i, uint32_t a, uint32_t b, uint32_t c)
{
    seq_step_t st; step_blank(&st); st.time = STT_NOTE; st.notes = a | b << 8 | c << 16 | 0xFF000000u; st.vel = 100; seq_set(part, i, &st);
}
static void tick_seq(uint32_t b) { (void)b; }
static void scene_seq(void)
{
    static const uint8_t arp[16] = {57, 0, 60, 64, 0, 67, 69, 0, 64, 0, 60, 62, 0, 64, 67, 0};
    uint32_t i;
    fresh();
    gen_sound(0, 2); gen_sound(1, 7);
    gr_set_norm(0, P_TEMPO, gr_norm_of_real(P_TEMPO, 96));
    gr_set_norm(0, P_SEQCLICK, 0);
    setf(0, P_ATTACK, 0.2); setf(0, P_RELEASE, 0.5);
    /* part 1: an arpeggio at 1/16 */
    gr_set_norm(0, P_SEQLEN, 65535 * 15 / 63); gr_set_norm(0, P_SEQDIV, 65535 * 14 / 16);
    for (i = 0; i < 16; i++) if (arp[i]) { seq_step_t st; step_blank(&st); st.time = STT_NOTE; st.notes = 0xFFFFFF00u | arp[i]; st.vel = 70 + (i & 3) * 10; seq_set(0, i, &st); }
    seq_set_lock_g(0, 0, P_POSITION, 8000, 1); seq_set_lock_g(0, 8, P_POSITION, 52000, 1);
    /* part 3: the drone, a chord a bar, gate 130 %, the harmonics gliding over the four bars */
    gr_set_norm(2, P_SEQLEN, 65535 * 3 / 63); gr_set_norm(2, P_SEQDIV, 65535 * 4 / 16);
    gr_set_norm(2, P_SEQGATE, gr_norm_of_real(P_SEQGATE, 130));
    put_chord(2, 0, 45, 52, 0xFF); put_chord(2, 1, 41, 48, 0xFF); put_chord(2, 2, 48, 55, 0xFF); put_chord(2, 3, 43, 50, 0xFF);
    seq_set_lock_g(2, 0, P_AHARM, 5000, 1); seq_set_lock_g(2, 2, P_AHARM, 60000, 1);
    /* part 4: the pad, two bars a step */
    gr_set_norm(3, P_SEQLEN, 65535 * 1 / 63); gr_set_norm(3, P_SEQDIV, 65535 * 3 / 16);
    gr_set_norm(3, P_SEQGATE, gr_norm_of_real(P_SEQGATE, 100));
    put_chord(3, 0, 69, 76, 0xFF); put_chord(3, 1, 65, 72, 0xFF);
    gr_set_norm(3, P_AWAVE, 20000);
    setf(0, P_LEVEL, 0.55); setf(2, P_LEVEL, 0.7); setf(3, P_LEVEL, 0.6);
    setf(0, P_REVSEND, 0.4); setf(3, P_REVSEND, 0.5); setf(2, P_REVSEND, 0.5);
    seq.play_req = 1;
    emit(secs(22.0), tick_seq);
    seq.stop_req = 1;
    emit(secs(5.0), 0);
}


/* ---------------------------------------------------------------- video 2 --- */
static double tsec(uint32_t b) { return (double)b * 128.0 / 44100.0; }
static double smooth_ramp(double t, double t0, double d) { double x = (t - t0) / d; x = x < 0 ? 0 : x > 1 ? 1 : x; return x * x * (3 - 2 * x); }

/* one held chord on the drone (part 3) or the pad (part 4), a parameter changing every few seconds */
static uint32_t demo_amb_part;
static void tick_ambdemo(uint32_t b)
{
    double t = tsec(b);
    uint32_t p = demo_amb_part;
    if (b == 0) { uint32_t k; static const uint8_t dn[3] = {45, 52, 57}, pn[3] = {57, 64, 72};
        for (k = 0; k < 3; k++) gr_note_on(p, p == 2 ? dn[k] : pn[k], 100); }
    setf(p, P_AHARM, 0.5 + 0.45 * sin(t * 0.45));
    if (p == 2) {
        gr_set_norm(p, P_ASTRUCT, t < 6 ? 0 : 65535);
        if (t >= 12) gr_set_norm(p, P_AVOICE, 65535);
        if (t >= 12) setf(p, P_AUNIDET, 0.2 + 0.5 * smooth_ramp(t, 12, 6));
        setf(p, P_ADETUNE, t < 12 ? 0.2 + 0.4 * sin(t * 0.3) * sin(t * 0.3) : 0.35);
        if (t >= 18) { setf(p, P_AMODDEPTH, 0.85); gr_set_norm(p, P_AMODSHAPE, t < 21 ? 65535 * 6 / 23 : 65535 * 14 / 23); setf(p, P_AMODRATE, 0.5); }
    } else {
        gr_set_norm(p, P_ASTRUCT, t < 6 ? 0 : 65535);
        if (t >= 12) gr_set_norm(p, P_AVOICE, 65535);
        if (t >= 12) setf(p, P_AUNIDET, 0.15 + 0.5 * smooth_ramp(t, 12, 6));
        setf(p, P_ADETUNE, 0.25 + 0.2 * sin(t * 0.25));
        if (t >= 18) { setf(p, P_AMODDEPTH, 0.8); gr_set_norm(p, P_AMODSHAPE, 65535 * 3 / 23); setf(p, P_AMODRATE, 0.4); }
        if (t >= 8 && t < 8.1) gr_set_norm(p, P_AWAVE, 22000);
    }
}
static void scene_ambdemo(uint32_t p)
{
    fresh();
    demo_amb_part = p;
    gr_set_norm(p, P_AATK, 12000);
    setf(p, P_REVSEND, 0.5);
    emit(secs(25.0), tick_ambdemo);
}

static int song_va;                       /* 1: part 1 plays a long generated VA sound instead of the pluck */
/* ---- the song: A minor, 60 BPM, a bar is four seconds; the four parts come in one after the other */
static void tick_song(uint32_t b)
{
    double t = tsec(b), fade = 1.0 - 0.0 * t;
    static int stopped, wave2;
    if (b == 0) { stopped = 0; wave2 = 0; gr_note_on(1, 57, 100); gr_note_on(1, 64, 100); }
    if (t > 84.0 && !stopped) { stopped = 1; seq.stop_req = 1; gr_note_off(1, 57); gr_note_off(1, 64); }
    setf(2, P_LEVEL, 0.85 * smooth_ramp(t, 0, 10) * fade);
    setf(3, P_LEVEL, 0.7 * smooth_ramp(t, 16, 10) * fade);
    setf(1, P_LEVEL, 0.55 * smooth_ramp(t, 32, 10) * fade);
    setf(0, P_LEVEL, 0.62 * smooth_ramp(t, 48, 8) * fade);
    setf(2, P_CUTOFF, 0.55 + 0.4 * smooth_ramp(t, 0, 28));
    setf(2, P_ADETUNE, 0.22 + 0.15 * sin(t * 0.13));
    setf(3, P_AMODDEPTH, 0.25 + 0.3 * smooth_ramp(t, 40, 20));
    if (t > 56 && !wave2) { wave2 = 1; gr_set_norm(3, P_AWAVE, 21000); }
    setf(1, P_POSITION, 0.2 + 0.6 * (0.5 + 0.5 * sin(t * 0.06)));
    setf(1, P_SCAN, 0.5 + 0.12 * sin(t * 0.11));
    setf(1, P_SIZE, 0.45 + 0.15 * sin(t * 0.17));
    setf(0, P_DLYSEND, 0.30 + 0.2 * smooth_ramp(t, 64, 12));
    setf(0, P_POSITION, 0.3 + 0.4 * (0.5 + 0.5 * sin(t * 0.2)));
    if (song_va) setf(0, P_LEVEL, 0.5 * smooth_ramp(t, 48, 8));
    {   double e = smooth_ramp(t, 84, 6);                       /* the reverb opens as it ends */
        setf(3, P_REVSEND, 0.5 + 0.35 * e); setf(2, P_REVSEND, 0.5 + 0.3 * e); setf(0, P_REVSEND, 0.4 + 0.4 * e); }
}
static void scene_song(void)
{
    static const uint8_t arp[2][16] = {{69, 0, 72, 0, 0, 76, 0, 79, 0, 0, 76, 0, 72, 0, 0, 0}, {69, 0, 0, 72, 0, 74, 0, 0, 76, 0, 0, 72, 0, 69, 0, 0}};
    static const uint8_t drn[4][2] = {{45, 52}, {41, 48}, {48, 55}, {43, 50}};
    static const uint8_t pad[4][2] = {{69, 76}, {65, 72}, {67, 76}, {62, 71}};
    uint32_t i;
    fresh();
    if (song_va) {                                           /* VA, the full two seconds, the filter closing slowly over them */
        gr_gen[0].length = 65535;
        gr_gen[0].norm[0][4] = 30000;                        /* osc mix */
        gr_gen[0].norm[0][6] = 22000;                        /* reso */
        gr_gen[0].norm[0][7] = 58000;                        /* filter envelope */
        gen_sound(0, 0);
    } else
        gen_sound(0, 2);
    gen_sound(1, 5);
    gr_set_norm(0, P_TEMPO, gr_norm_of_real(P_TEMPO, 60));
    gr_set_norm(0, P_SEQCLICK, 0);
    /* part 1: a sparse pluck, an eighth a step, two bars of pattern then two more */
    gr_set_norm(0, P_SEQLEN, 65535 * 31 / 63); gr_set_norm(0, P_SEQDIV, 65535 * 11 / 16);
    for (i = 0; i < 32; i++) if (arp[i >> 4][i & 15]) { seq_step_t st; step_blank(&st); st.time = STT_NOTE; st.notes = 0xFFFFFF00u | arp[i >> 4][i & 15]; st.vel = 62 + (i * 7 % 30); seq_set(0, i, &st); }
    setf(0, P_ATTACK, 0.2); setf(0, P_RELEASE, 0.6); setf(0, P_DLYTIME, 0.4); setf(0, P_DLYFB, 0.55);
    if (song_va) {                                           /* long grains, a long release; the size locks glide between two long values */
        setf(0, P_RELEASE, 0.75); setf(0, P_ATTACK, 0.35); setf(0, P_DENSITY, 0.5); setf(0, P_SPREAD, 0.3);
        seq_set_lock_g(0, 0, P_SIZE, 46000, 1); seq_set_lock_g(0, 16, P_SIZE, 60000, 1);
    } else {
        seq_set_lock_g(0, 0, P_SIZE, 20000, 1); seq_set_lock_g(0, 16, P_SIZE, 48000, 1);
    }
    /* part 2: a texture, held all the way (the notes in tick_song) */
    setf(1, P_ATTACK, 0.7); setf(1, P_RELEASE, 0.8); setf(1, P_REVSEND, 0.55);
    /* part 3 and 4: four chords of four bars, the harmonics gliding over the cycle */
    for (i = 0; i < 2; i++) {
        uint32_t part = 2 + i, c;
        gr_set_norm(part, P_SEQLEN, 65535 * 3 / 63); gr_set_norm(part, P_SEQDIV, 65535 * 1 / 16);
        gr_set_norm(part, P_SEQGATE, gr_norm_of_real(P_SEQGATE, 106));
        for (c = 0; c < 4; c++) { const uint8_t *n = i ? pad[c] : drn[c]; put_chord(part, c, n[0], n[1], 0xFF); }
        seq_set_lock_g(part, 0, P_AHARM, i ? 40000 : 6000, 1); seq_set_lock_g(part, 2, P_AHARM, i ? 8000 : 56000, 1);
    }
    gr_set_norm(3, P_AWAVE, 9000);
    gr_set_norm(2, P_AATK, 30000); gr_set_norm(3, P_AATK, 28000);
    setf(2, P_REVSEND, 0.5); setf(3, P_REVSEND, 0.5); setf(1, P_DLYSEND, 0.2);
    setf(0, P_LEVEL, 0); setf(1, P_LEVEL, 0); setf(2, P_LEVEL, 0); setf(3, P_LEVEL, 0);
    seq.play_req = 1;
    emit(secs(104.0), tick_song);
}

/* ---- how the granular engine sounds: one held chord, the parameters moving one after the other */
static void tick_gransh(uint32_t b)
{
    double t = tsec(b);
    static int g2, g3;
    if (b == 0) { g2 = g3 = 0; gr_note_on(0, 57, 95); gr_note_on(0, 64, 95); gr_note_on(0, 69, 90); }
    if (t > 50.0 && !g3) { g3 = 1; gr_note_off(0, 57); gr_note_off(0, 64); gr_note_off(0, 69); }
    if (t > 44.0 && !g2) { g2 = 1; gen_sound(0, 1); }
    /* position: slow sweep through the sound */
    if (t < 6) { setf(0, P_POSITION, 0.05 + 0.9 * smooth_ramp(t, 0, 6)); }
    /* size: from tiny grains to long ones */
    else if (t < 12) { setf(0, P_POSITION, 0.45); setf(0, P_SIZE, 0.08 + 0.82 * smooth_ramp(t, 6, 6)); }
    /* density: sparse to a cloud */
    else if (t < 18) { setf(0, P_SIZE, 0.55); setf(0, P_DENSITY, 0.12 + 0.75 * smooth_ramp(t, 12, 6)); }
    /* spread and jitter */
    else if (t < 24) { setf(0, P_SPREAD, 0.05 + 0.8 * smooth_ramp(t, 18, 3)); setf(0, P_JITTER, 0.3 + 0.65 * smooth_ramp(t, 21, 3)); }
    /* pitch randomness */
    else if (t < 29) { setf(0, P_PITCHRND, 0.5 * smooth_ramp(t, 24, 3)); }
    /* scan: the position moves by itself */
    else if (t < 34) { setf(0, P_PITCHRND, 0.5 * (1 - smooth_ramp(t, 29, 1.5))); setf(0, P_SCAN, 0.5 + 0.28 * smooth_ramp(t, 29, 2)); }
    /* reverse grains */
    else if (t < 38) { setf(0, P_REVERSE, 0.7 * smooth_ramp(t, 34, 2)); }
    /* filter and LFO */
    else if (t < 44) {
        gr_set_norm(0, P_FTYPE, 65535 * 2 / 4);              /* Res LP */
        setf(0, P_RESO, 0.35);
        gr_set_norm(0, P_L1DEST, 65535 * 1 / 28);
        setf(0, P_L1DEPTH, 0.5 + 0.35 * smooth_ramp(t, 38, 4)); setf(0, P_L1RATE, 0.35);
        setf(0, P_CUTOFF, 0.62 + 0.12 * sin(t * 0.8));
    }
    else { setf(0, P_REVERSE, 0.15); setf(0, P_CUTOFF, 0.85); setf(0, P_L1DEPTH, 0.2); setf(0, P_SCAN, 0.55); }
}
static void scene_gransh(void)
{
    fresh();
    gr_gen[0].length = 65535; gr_gen[0].norm[0][4] = 30000; gr_gen[0].norm[0][6] = 22000; gr_gen[0].norm[0][7] = 58000;
    gen_sound(0, 0);
    setf(0, P_ATTACK, 0.4); setf(0, P_RELEASE, 0.75); setf(0, P_SIZE, 0.35); setf(0, P_DENSITY, 0.5); setf(0, P_SPREAD, 0.05);
    setf(0, P_REVSEND, 0.4); setf(0, P_DLYSEND, 0.15); setf(0, P_LEVEL, 0.8);
    gr_set_norm(0, P_SCAN, 32768);
    emit(secs(60.0), tick_gransh);
}
