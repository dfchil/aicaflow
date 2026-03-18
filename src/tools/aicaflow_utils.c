#include <math.h>
#include <stdint.h>
#include <aicaflow/aicaflow.h>

/**
 * AICA Flow Math Utilities (Standard C99/C11)
 * Decoupled from header to avoid multiple definitions.
 */

uint32_t aica_get_reg_addr(uint8_t slot, uint8_t reg) {
    return (uint32_t)(0x00800000 + (slot * 0x80) + reg);
}

uint32_t aica_pitch_convert(float f) {
    float ratio = f / AICA_BASE_FREQ;
    float oct_f = log2f(ratio);
    int8_t oct = (int8_t)floorf(oct_f);
    
    float oct_pow = powf(2.0f, (float)oct);
    uint32_t fns = (uint32_t)((1024.0f * (f / (AICA_BASE_FREQ * oct_pow))) - 1024.0f);
    
    if (oct < -8) oct = -8;
    if (oct > 7)  oct = 7;
    if (fns > 1023) fns = 1023;

    return (uint32_t)(((oct & 0xF) << 11) | (fns & 0x3FF));
}

float midi_note_to_freq(uint8_t note) {
    return 440.0f * powf(2.0f, (float)(note - 69) / 12.0f);
}
