/* SPDX-License-Identifier: GPL-3.0-only
 * amb-iguos: an ambient granular synthesizer for the M-Vave FM-1 (JieLi AC79 / WL82), built on Mini-Grain.
 * One compilation unit (the HAL is header-only). Each file uses what the files above it define, so the order
 * matters. The granular engine, parameters and screens follow the Granular Synth for ESP32-S3; the platform
 * (HAL, USB-MIDI / USB audio, TRS MIDI, flash storage, the M-UPGRADE entry) is Felucca / Mini-Tone by Leo Kuroshita
 * (@kurogedelic, Hügelton Instruments), GPL-3.0. */

/* ---------------------------------------------------- build options --- */
/* tools/build.py: GRAIN_<NAME>=0 / 1 in the environment overrides these (the old FELUCCA_ names still work). */
#ifndef FELUCCA_FLASH
#define FELUCCA_FLASH 1          /* flash driver + storage.c: the saved state, presets, stored samples */
#endif
#ifndef FELUCCA_OTA
#define FELUCCA_OTA 1            /* the M-UPGRADE update entry; needs FELUCCA_FLASH */
#endif
#ifndef FELUCCA_OTA_DRYRUN
#define FELUCCA_OTA_DRYRUN 0
#endif
#ifndef FELUCCA_CDC
#define FELUCCA_CDC 0            /* USB serial console (not in Mini-Grain yet) */
#endif
#ifndef FELUCCA_CDC_DEFAULT
#define FELUCCA_CDC_DEFAULT 0
#endif
#ifndef FELUCCA_UAC
#define FELUCCA_UAC 1            /* USB audio: the master output to the computer, 44.1 or 48 kHz */
#endif
#ifndef FELUCCA_UAC_TONE
#define FELUCCA_UAC_TONE 0
#endif
#ifndef FELUCCA_UART
#define FELUCCA_UART 1           /* TRS MIDI IN on UART1 / PH8 */
#endif
#ifndef FELUCCA_ID
#define FELUCCA_ID "FM-1_946"    /* package identity (build.py: the .fwsc marker string) */
#endif
#ifndef FELUCCA_VERSION
#define FELUCCA_VERSION "amb-iguos 0.1"
#endif
#if FELUCCA_OTA && !FELUCCA_FLASH
#error "FELUCCA_OTA needs FELUCCA_FLASH"
#endif

/* ------------------------------------------------------------- HAL --- */
#include <stdint.h>
#include "fm1_time.h"
#include "fm1_sys.h"
#include "fm1_irq.h"
#include "fm1_guard.h"
#include "fm1_input.h"
#include "fm1_timer.h"
#include "fm1_audio.h"
#include "fm1_adc.h"
#include "fm1_lcd_hw.h"

/* ----------------------------------------------------------- system --- */
#define RING_PUBLISH() __asm__ volatile("" ::: "memory")   /* slot store before the index update */
static volatile uint32_t fm1_ms;  /* milliseconds since boot (TIMER4-based, TIMER5 ISR in main.c) */
/* boot-loop guard (main.c): two boots in a row that die in the first 30 s -> UBOOT */
#define BOOTGUARD_MAGIC 0x42475244u
struct { uint32_t magic, failed, pending; } bootguard __attribute__((section(".noinit")));
#define GR_YIELD() fm1_wdt_feed()

/* ----------------------------------------------------- base, display --- */
#include "libc.c"
#include "lcd.c"
#include "gfx.c"

/* ----------------------------------------------------------- sound --- */
#include "gr_core.h"
#include "gr_params.c"
#include "gr_filter.c"
#include "gr_fx.c"
#include "gr_engine.c"
#include "gr_gen.c"
#include "gr_amb.c"
#include "gr_drift.c"                     /* amb-iguos: scale, Deriva, Freeze */

/* ------------------------------------------------------- USB, MIDI --- */
#include "usb.c"
#if FELUCCA_UART
#include "midi_uart.c"
#endif
#include "gr_seq.c"
#include "gr_midi.c"
#include "audio.c"

/* --------------------------------------- storage, update, stored samples --- */
#if FELUCCA_FLASH
#include "fm1_flash.h"
#include "storage_hw.c"
#include "storage.c"
#endif
#if FELUCCA_OTA
#include "ota.c"
#include "ota_hw.c"
#endif
#include "gr_link.c"

/* ---------------------------------------------------- UI, state, panel --- */
#include "panel.c"
#include "gr_ui.c"
#include "gr_state.c"
#include "gr_input.c"
#include "master.c"
#include "main.c"
