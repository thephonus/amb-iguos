/* SPDX-License-Identifier: GPL-3.0-only
 * The MASTER pot, read before the audio starts (so the first block plays at its level) and followed in the main
 * loop. master_knob: the pot x16, smoothed per UI frame. The gain is a square law, 4096 = unity. */
static int32_t master_knob = 512 * 16;
static int32_t master_of_pot(uint32_t a) { return (int32_t)((a * a) >> 8) > 4096 ? 4096 : (int32_t)((a * a) >> 8); }
static void master_poll(void)
{
    int32_t a = fm1_adc_read(FM1_ADC_MASTER);
    if (a >= 0) {
        master_knob += (a * 16 - master_knob) / 8;
        gr_master_q12 = master_of_pot((uint32_t)(master_knob / 16));
    }
}
/* power-on, before audio_init: the knob as it stands, no glide */
static void master_boot(void)
{
    uint32_t i;
    (void)fm1_adc_read(FM1_ADC_MASTER);                 /* (the first conversion after the pin setup: dropped) */
    for (i = 0; i < 3u; i++) {
        int32_t a = fm1_adc_read(FM1_ADC_MASTER);
        if (a >= 0) {
            master_knob = a * 16;
            gr_master_q12 = master_of_pot((uint32_t)a);
            return;
        }
    }
}
