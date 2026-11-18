/* SPDX-License-Identifier: GPL-3.0-only
 * Mini-Grain: a granular synthesizer for the M-Vave FM-1.
 * The engine, parameters, effects, generators and screens follow the Granular Synth for ESP32-S3; the platform
 * (HAL, USB, MIDI, flash storage, update entry) is Felucca / Mini-Tone by Leo Kuroshita (@kurogedelic), GPL-3.0.
 *
 * Core types and sizes. The FM-1 has no FPU and the build forbids library calls, so everything is fixed point. */
#include <stdint.h>

/* ------------------------------------------------------------ sizes --- */
#define NPART 4                  /* independent sounds, one per MIDI channel: two granular parts, a drone and a pad */
#define NGPART 2                 /* parts 1 and 2 are granular; part 3 is the drone, part 4 the pad */
enum { KIND_GRAIN, KIND_DRONE, KIND_PAD };
#define AMB_V 4u                 /* notes the drone or the pad sounds at once */
#define AMB_BUDGET 16u           /* oscillators a drone or pad part runs at most (unison copies count) */
#define NNOTE 8                  /* simultaneous notes per part */
#define NGRAIN 28                /* grain pool shared by both parts (the cap adapts to the CPU load) */
#define BLK 128                  /* frames per render block (= HALF_FRAMES) */
#define HALF_FRAMES BLK
#define SMP_FRAMES 43008         /* frames of sample memory per granular part (84 KiB each) */
#define GEN_RATE 22050           /* the generators render at this rate: 2 s fit in a part */
#define ROOT_NOTE 60             /* the MIDI note that plays a sample at its original pitch */
#define NPRESET 16
#define NBANK_MAX 3              /* pages have at most 3 banks of 4 dials */

/* ------------------------------------------------------- parameters --- */
enum { GR_L, GR_E };
enum { GR_U_PCT, GR_U_MS, GR_U_HZ, GR_U_PERSEC, GR_U_SPEED, GR_U_SEMI, GR_U_PLAIN, GR_U_BIPOLAR, GR_U_CHOICE,
       GR_U_DEST, GR_U_CHAN, GR_U_NUM, GR_U_PAN };

typedef struct {
    const char *name;
    int32_t mn, mx;              /* real values x mul */
    uint16_t def;                /* default norm */
    int32_t lg;                  /* log2(max/min), Q16 (GR_E) */
    int32_t mul;
    uint16_t steps;              /* 0 = continuous */
    uint8_t cc;                  /* 255 = none */
    uint8_t curve, unit;
    const char *const *labels;
    const char *sfx;             /* after the number */
} gr_pinfo_t;

typedef struct { const char *name; const gr_pinfo_t *p; uint8_t n, sustained; } gr_gentype_t;

#include "grain_gen.h"           /* tables, P_* and the parameter table (tools/gen_grain.py) */

typedef struct {
    uint16_t norm[P_COUNT];      /* 0..65535 */
    int32_t real[P_COUNT];       /* x mul */
} gr_pset_t;

/* ------------------------------------------------------------ sound --- */
typedef struct {                 /* the sample of a part */
    int16_t *data;               /* SMP_FRAMES + 1 (the last is the first again: a loop) */
    uint32_t frames;             /* 0 = none */
    uint32_t rate;               /* Hz */
    uint8_t peaks[240];          /* the picture (0..255) */
    uint8_t kind;                /* 0 none, 1 generated, 2 stored sample */
    char name[16];
} gr_sample_t;

/* what the screen reads, written by the audio code */
typedef struct {
    volatile uint8_t load;       /* CPU %, 0..255 */
    volatile uint8_t grains, cap;
    volatile uint8_t notes[NPART];
    volatile int16_t lfo[NPART][2];          /* -32767..32767 */
    volatile uint16_t gview[NGRAIN];         /* position 0..65534 or 0xFFFF */
    volatile uint8_t gpart[NGRAIN];
    volatile uint32_t skipped;
} gr_stats_t;

#define GVIEW_NONE 0xFFFFu

/* ---------------------------------------------------------- colours --- */
/* a night desert: deep plum ground, warm sand for part 1, cool aqua for part 2, coral while editing */
#define UC_BG RGB(0x1B, 0x17, 0x26)
#define UC_PANEL RGB(0x2B, 0x25, 0x40)
#define UC_UNLIT RGB(0x4A, 0x42, 0x63)
#define UC_TEXT RGB(0xF4, 0xEE, 0xE3)
#define UC_MUTED RGB(0xA7, 0x9F, 0xB8)
#define UC_SAND RGB(0xF2, 0xC8, 0x79)
#define UC_AQUA RGB(0x6F, 0xDA, 0xD3)
#define UC_CORAL RGB(0xFF, 0x8A, 0x70)
#define UC_WARN RGB(0xFF, 0x6F, 0x7F)
#define UC_LILAC RGB(0xB7, 0x9C, 0xFF)          /* part 3, the drone */
#define UC_MINT RGB(0x9B, 0xE2, 0x8A)           /* part 4, the pad */

