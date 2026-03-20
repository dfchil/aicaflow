#ifndef AICAF_H
#define AICAF_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define AICAF_MAGIC     0xA1CAF200
#define AICAF_VERSION   1
#define AICA_BASE_FREQ 44100.0f

#define AFX_ALIGN4(v) (((v) + 3u) & ~3u)

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
    uint32_t sample_base;      /* song_base + SDAT section offset */
    uint32_t flow_ptr;         /* song_base + FLOW section offset */
    uint32_t flow_count;       /* FLOW section entry count */
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

/* Section IDs (ASCII 4CC stored in little-endian uint32) */
#define AFX_SECT_FLOW 0x574F4C46u /* 'FLOW' */
#define AFX_SECT_SDES 0x53454453u /* 'SDES' */
#define AFX_SECT_SDAT 0x54414453u /* 'SDAT' */
#define AFX_SECT_DSPM 0x4D505344u /* 'DSPM' */
#define AFX_SECT_DSPC 0x43505344u /* 'DSPC' */
#define AFX_SECT_META 0x4154454Du /* 'META' */

/* .afx file header — 8 x uint32 = 32 bytes */
typedef struct {
    uint32_t magic;             /* AICAF_MAGIC = 0xA1CAF200 */
    uint32_t version;           /* AICAF_VERSION = 1 */
    uint32_t header_size;       /* sizeof(afx_header_t) */
    uint32_t section_count;     /* number of afx_section_entry_t entries */
    uint32_t section_table_off; /* byte offset to section table */
    uint32_t section_table_size;/* bytes occupied by section table */
    uint32_t total_ticks;       /* total song duration in ms */
    uint32_t flags;             /* reserved for future use */
} afx_header_t;

/* Section table entry — 6 x uint32 = 24 bytes */
typedef struct {
    uint32_t id;                /* AFX_SECT_* */
    uint32_t offset;            /* file-relative byte offset */
    uint32_t size;              /* section size in bytes */
    uint32_t count;             /* entry count when section is an array */
    uint32_t align;             /* section alignment, usually 4 */
    uint32_t flags;             /* reserved */
} afx_section_entry_t;

/*
 * Sample descriptor — 32 bytes (packed).
 *
 * SA_HI / SA_LO flow commands in the stream store the full blob-local byte offset
 * (equal to sample_off here).  The ARM7 driver adds
 * sample_base = (file_load_addr + SDAT section offset) once at PLAY time and
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
} afx_cmd_t;

static inline const afx_section_entry_t *afx_find_section(const afx_header_t *hdr,
                                                          uint32_t section_id) {
    const afx_section_entry_t *tab =
        (const afx_section_entry_t *)((const uint8_t *)hdr + hdr->section_table_off);
    for (uint32_t i = 0; i < hdr->section_count; i++) {
        if (tab[i].id == section_id) return &tab[i];
    }
    return NULL;
}

/* Scale an AICA total-level register value by a global music volume.
 * TL is inverted: 0=loudest, 255=silent. volume is 0..255. */
static inline uint32_t afx_scale_total_level(uint32_t tl, uint32_t volume) {
    uint32_t x = (255u - (tl & 0xFFu)) * (volume & 0xFFu); /* 0..65025 */
    uint32_t scaled = (x + 1u + (x >> 8)) >> 8;            /* exact floor(x/255) for 16-bit x */
    return 255u - scaled;
}

/* Return the index of the first flow command whose timestamp is >= target_tick. */
static inline uint32_t afx_cmd_lower_bound_by_tick(const afx_cmd_t *stream,
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

/* SH4-side runtime state for firmware upload and dynamic allocator tracking. */
typedef struct {
    uint32_t dynamic_base;
    uint32_t dynamic_cursor;
} aica_state_t;

const aica_state_t *afx_get_state(void);

/* SH4-side AICA RAM dynamic allocator/upload helpers */
void afx_mem_reset(uint32_t dynamic_base);
uint32_t afx_mem_alloc(uint32_t size, uint32_t align);
bool afx_mem_write(uint32_t spu_addr, const void *src, uint32_t size);
uint32_t afx_upload_afx(const void *afx_data, uint32_t afx_size);
bool afx_upload_and_init_firmware(const void *fw_data,
                                  uint32_t fw_size);


#endif
