#ifndef AICAF_H
#define AICAF_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define AICAF_MAGIC     0xA1CAF200
#define AICAF_VERSION   2
#define AICA_BASE_FREQ 44100.0f

/* Sample format codes (stored in afx_sample_desc_t.format) */
#define AFX_FMT_PCM16   0
#define AFX_FMT_PCM8    1
#define AFX_FMT_ADPCM   3  /* Yamaha 4-bit ADPCM */

/* Loop mode codes (stored in afx_sample_desc_t.loop_mode) */
#define AFX_LOOP_NONE   0
#define AFX_LOOP_FWD    1
#define AFX_LOOP_BIDIR  2
#define AICA_TOTAL_RAM (2 * 1024 * 1024)

#define AFX_MEM_CLOCKS      (AICA_TOTAL_RAM - 32)

#pragma pack(push, 1)

typedef struct {
    uint32_t song_base;        /* absolute SPU address of uploaded .afx */
    uint32_t sample_base;      /* song_base + header.sample_data_off */
    uint32_t flow_ptr;         /* song_base + header.flow_data_off */
    uint32_t flow_count;       /* header.flow_data_size (flow-command count) */
    uint32_t flow_idx;
    uint32_t next_event_tick;
    uint32_t is_playing;
    uint32_t loop_count;
} afx_player_state_t;

typedef struct {
    uint32_t cmd;
    uint32_t arg0;
    uint32_t arg1;
    uint32_t arg2;
} afx_ipc_cmd_t;

typedef struct {
    uint32_t magic;
    uint32_t arm_status;    /* 0=Idle, 1=Playing, 2=Paused, 3=Error */
    uint32_t current_tick;  /* Current playback tick */
    uint32_t flow_pos;      /* Offset into flow-command stream */
    uint32_t volume;        /* Music volume 0-255 (255=full). Set via AICAF_CMD_VOLUME. */
    uint32_t q_head;        /* SH4 producer index */
    uint32_t q_tail;        /* ARM7 consumer index */
    uint32_t reserved;
} afx_ipc_status_t;

#define AFX_IPC_STATUS_ADDR ((AFX_MEM_CLOCKS - sizeof(afx_ipc_status_t)) & ~31)
/* Command queue is the primary SH4<->ARM7 transport and is always reserved. */
#define AFX_IPC_QUEUE_SZ    0x0400
#define AFX_IPC_CMD_QUEUE_ADDR (AFX_IPC_STATUS_ADDR - AFX_IPC_QUEUE_SZ)
#define AFX_PLAYER_STATE_ADDR ((AFX_IPC_CMD_QUEUE_ADDR - sizeof(afx_player_state_t)) & ~31)

#define AFX_IPC_QUEUE_CAPACITY (AFX_IPC_QUEUE_SZ / sizeof(afx_ipc_cmd_t))

/* SH4 -> ARM7 Command IDs */
#define AICAF_CMD_NONE      0
#define AICAF_CMD_PLAY      1
#define AICAF_CMD_STOP      2
#define AICAF_CMD_PAUSE     3
#define AICAF_CMD_VOLUME    4  /* cmd_arg = new music volume (0-255) */
#define AICAF_CMD_SEEK      5  /* cmd_arg = target position in ms; driver binary-searches flow commands */

/* .afx header — 13 x uint32 = 52 bytes */
typedef struct {
    uint32_t magic;             /* AICAF_MAGIC = 0xA1CAF200 */
    uint32_t version;           /* AICAF_VERSION = 2 */
    uint32_t sample_data_off;   /* byte offset to raw sample blob */
    uint32_t sample_data_size;  /* total byte size of sample blob */
    uint32_t sample_desc_off;   /* byte offset to afx_sample_desc_t table */
    uint32_t sample_desc_count; /* number of afx_sample_desc_t entries */
    uint32_t flow_data_off;     /* byte offset to afx_flow_cmd_t array */
    uint32_t flow_data_size;    /* number of afx_flow_cmd_t entries */
    uint32_t dsp_mpro_off;      /* byte offset to DSP microprogram (0 = none) */
    uint32_t dsp_mpro_size;     /* byte size of DSP microprogram */
    uint32_t dsp_coef_off;      /* byte offset to DSP coefficients (0 = none) */
    uint32_t dsp_coef_size;     /* byte size of DSP coefficients */
    uint32_t total_ticks;       /* total song duration in ms */
} afx_header_t;

/*
 * Sample descriptor — 32 bytes (packed).
 *
 * SA_HI / SA_LO flow commands in the stream store the full blob-local byte offset
 * (equal to sample_off here).  The ARM7 driver adds
 * sample_base = (file_load_addr + sample_data_off) once at PLAY time and
 * extracts the appropriate bits per register write.  The precompiler never
 * needs to know the runtime load address; the driver never performs
 * per-event address arithmetic.
 */
typedef struct {
    uint32_t source_id;     /* hash of originating WAV file (for info tools) */
    uint8_t  gm_program;   /* GM program number */
    uint8_t  format;       /* AFX_FMT_* */
    uint8_t  loop_mode;    /* AFX_LOOP_* */
    uint8_t  root_note;    /* MIDI note of recorded pitch (default 60 = C4) */
    int8_t   fine_tune;    /* cents offset from root_note (-100..+100) */
    uint8_t  reserved[3];
    uint32_t sample_off;   /* byte offset into sample data blob */
    uint32_t sample_size;  /* byte size of encoded sample data */
    uint32_t loop_start;   /* loop start byte offset relative to sample_off */
    uint32_t loop_end;     /* loop end byte offset relative to sample_off (0 = end) */
    uint32_t sample_rate;  /* original sample rate in Hz */
} afx_sample_desc_t;

typedef struct {
    uint32_t timestamp; /* Absolute time in ms */
    uint8_t  slot;
    uint8_t  reg;
    uint16_t pad;
    uint32_t value;
} afx_flow_cmd_t;

/* Scale an AICA total-level register value by a global music volume.
 * TL is inverted: 0=loudest, 255=silent. volume is 0..255. */
static inline uint32_t afx_scale_total_level(uint32_t tl, uint32_t volume) {
    uint32_t x = (255u - (tl & 0xFFu)) * (volume & 0xFFu); /* 0..65025 */
    uint32_t scaled = (x + 1u + (x >> 8)) >> 8;            /* exact floor(x/255) for 16-bit x */
    return 255u - scaled;
}

/* Return the index of the first flow command whose timestamp is >= target_tick. */
static inline uint32_t afx_flow_cmd_lower_bound_by_tick(const afx_flow_cmd_t *stream,
                                                         uint32_t count,
                                                         uint32_t target_tick) {
    uint32_t lo = 0;
    uint32_t hi = count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (stream[mid].timestamp < target_tick) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

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

/* SH4-side host API (implemented in src/driver/aica_host_api.c) */
bool afx_init(void);
void afx_play(uint32_t song_spu_addr);
void afx_stop(void);
void afx_pause(void);
uint32_t afx_get_tick(void);
void afx_set_volume(uint8_t vol);
bool afx_is_playing(void);
void afx_seek(uint32_t tick_ms);

/* SH4-side AICA RAM dynamic allocator/upload helpers */
void afx_mem_reset(uint32_t dynamic_base);
uint32_t afx_mem_alloc(uint32_t size, uint32_t align);
bool afx_mem_write(uint32_t spu_addr, const void *src, uint32_t size);
uint32_t afx_upload_afx(const void *afx_data, uint32_t afx_size);
bool afx_upload_and_init_firmware(const void *fw_data,
                                  uint32_t fw_size,
                                  uint32_t fw_spu_addr,
                                  uint32_t *out_dynamic_base);


#endif
