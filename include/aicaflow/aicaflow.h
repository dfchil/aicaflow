#ifndef AICAF_H
#define AICAF_H

#include <stdint.h>
#include <stddef.h>

#define AICAF_MAGIC 0xA1CAF100
#define AICA_BASE_FREQ 44100.0f
#define AICA_TOTAL_RAM (2 * 1024 * 1024)

#define AICAFLOW_IPC_QUEUE_SZ    0x2000
#define AICAFLOW_MEM_CLOCKS      (AICA_TOTAL_RAM - 32)
#define AICAFLOW_IPC_STATUS_ADDR ((AICAFLOW_MEM_CLOCKS - AICAFLOW_IPC_QUEUE_SZ) & ~31)

#pragma pack(push, 1)

typedef struct {
    uint32_t magic;
    uint32_t arm_status;    /* 0=Idle, 1=Playing, 2=Paused, 3=Error */
    uint32_t current_tick;  /* Current playback tick */
    uint32_t stream_pos;    /* Offset into opcode stream */
    uint32_t volume;        /* Global volume 0-255 */
    uint32_t cmd;           /* Last command processed by ARM */
    uint32_t cmd_arg;       /* Argument for last command */
    uint32_t reserved;
} afx_ipc_status_t;

/* SH4 -> ARM7 Command IDs */
#define AICAF_CMD_NONE   0
#define AICAF_CMD_PLAY   1
#define AICAF_CMD_STOP   2
#define AICAF_CMD_PAUSE  3
#define AICAF_CMD_VOLUME 4

#define AICAFLOW_IPC_CMD_QUEUE_ADDR ((AICAFLOW_IPC_STATUS_ADDR + sizeof(afx_ipc_status_t) + 31) & ~31)
#define AICAFLOW_PLAYER_STATE_ADDR  ((AICA_TOTAL_RAM - 0x4000) & ~31)

typedef struct {
    uint32_t wave_ram_addr;
    uint32_t size;
    uint8_t  format;
} afx_sample_entry_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t sample_data_off;
    uint32_t sample_data_size;
    uint32_t stream_data_off;
    uint32_t stream_data_size;
    uint32_t dsp_mpro_off;
    uint32_t dsp_coef_off;
    uint32_t total_ticks;
} afx_header_t;

typedef struct {
    uint32_t timestamp; /* Absolute time in ms */
    uint8_t  slot;
    uint8_t  reg;
    uint16_t pad;
    uint32_t value;
} afx_opcode_t;

#pragma pack(pop)

/* Common Hardware Offsets */
#define AICA_REG_BASE    0x00800000
#define AICA_SLOT_SIZE   0x80

/* Register Offsets within a Slot (8-bit addressable) */
#define AICA_REG_CTL      0x00 /* 15:CTL, 14:KYON, 13:OFFS, 12:LPSH, 11:PCMS, 10-8:LPCTL, 7-0:PPAD */
#define AICA_REG_SA_HI    0x01 /* Sample Start Address HI */
#define AICA_REG_SA_LO    0x02 /* Sample Start Address LO */
#define AICA_REG_LSA_HI   0x03 /* Loop Start Address HI */
#define AICA_REG_LSA_LO   0x04 /* Loop Start Address LO */
#define AICA_REG_LEA_HI   0x05 /* Loop End Address HI */
#define AICA_REG_LEA_LO   0x06 /* Loop End Address LO */
#define AICA_REG_D2R_D1R  0x07 /* Decay 2 Rate, Decay 1 Rate */
#define AICA_REG_EGH_RR   0x08 /* EG Hold, Release Rate */
#define AICA_REG_AR_SR    0x09 /* Attack Rate, Sustain Rate */
#define AICA_REG_LNK_DL   0x0A /* Release Link, Decay Level */
#define AICA_REG_FNS_OCT  0x0C /* Frequency Number (10 bits), Octave (4 bits) */
#define AICA_REG_TOT_LVL  0x0D /* Total Level (TL) */
#define AICA_REG_PAN_VOL  0x0E /* Pan, Volume */

uint32_t aica_get_reg_addr(uint8_t slot, uint8_t reg);
uint32_t aica_pitch_convert(float f);
float midi_note_to_freq(uint8_t note);

#endif
