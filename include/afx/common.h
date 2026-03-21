#ifndef AFX_COMMON_H
#define AFX_COMMON_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define AICAF_MAGIC     0xA1CAF100
#define AICAF_VERSION   1
#define AICA_BASE_FREQ  44100.0f
#define AICA_TOTAL_RAM  (2 * 1024 * 1024)

#define AFX_ALIGN4(v)  (((v) + 3u) & ~3u)
#define AFX_ALIGN32(v) (((v) + 31u) & ~31u)

/* Section IDs (ASCII 4CC stored in little-endian uint32) */
#define AFX_SECT_FLOW 0x574F4C46u /* 'FLOW' */
#define AFX_SECT_SDES 0x53454453u /* 'SDES' */
#define AFX_SECT_SDAT 0x54414453u /* 'SDAT' */
#define AFX_SECT_DSPM 0x4D505344u /* 'DSPM' */
#define AFX_SECT_DSPC 0x43505344u /* 'DSPC' */
#define AFX_SECT_META 0x4154454Du /* 'META' */

/* Sample format codes */
#define AFX_FMT_PCM16   0
#define AFX_FMT_PCM8    1
#define AFX_FMT_ADPCM   3

/* Loop mode codes */
#define AFX_LOOP_NONE   0
#define AFX_LOOP_FWD    1
#define AFX_LOOP_BIDIR  2


/* SH4 -> ARM7 Command IDs */
#define AICAF_CMD_NONE      0
#define AICAF_CMD_PLAY      1
#define AICAF_CMD_STOP      2
#define AICAF_CMD_PAUSE     3
#define AICAF_CMD_VOLUME    4  /* arg0 = music volume (0-255) */
#define AICAF_CMD_SEEK      5  /* arg0 = target position in ms */



#pragma pack(push, 1)

/* .afx file header */
typedef struct {
    uint32_t magic;             /* AICAF_MAGIC */
    uint32_t version;           /* AICAF_VERSION */
    uint32_t section_count;     /* Number of sections immediately following this header */
    uint32_t total_ticks;       /* Total song duration in ms */
    uint32_t flags;             /* Reserved */
} afx_header_t;

/* Section table entry */
typedef struct {
    uint32_t id;                /* AFX_SECT_* */
    uint32_t offset;            /* file-relative byte offset */
    uint32_t size;              /* section size in bytes */
    uint32_t count;             /* entry count */
    uint32_t align;             /* alignment */
    uint32_t flags;             /* reserved */
} afx_section_entry_t;

/* Sample descriptor (32 bytes) */
typedef struct {
    uint32_t source_id;     /* hash of originating WAV */
    uint8_t  gm_program;    /* GM program number */
    uint8_t  format;        /* AFX_FMT_* */
    uint8_t  loop_mode;     /* AFX_LOOP_* */
    uint8_t  root_note;     /* MIDI note of recorded pitch */
    int8_t   fine_tune;     /* cents offset from root_note (-100..+100) */
    uint8_t  reserved[3];
    uint32_t sample_off;    /* byte offset into sample data blob */
    uint32_t sample_size;   /* byte size of encoded sample data */
    uint32_t loop_start;    /* loop start byte offset relative to sample_off */
    uint32_t loop_end;      /* loop end byte offset relative to sample_off */
    uint32_t sample_rate;   /* original sample rate in Hz */
} afx_sample_desc_t;

/* Flow command (8 bytes) */
typedef struct {
    uint32_t timestamp;     /* Absolute time in ms */
    uint8_t  slot;
    uint8_t  reg;
    uint16_t pad;
    uint32_t value;
} afx_cmd_t;

/* IPC Command (16 bytes) */
typedef struct {
    uint32_t cmd;
    uint32_t arg0;
    uint32_t arg1;
    uint32_t arg2;
} afx_ipc_cmd_t;

#pragma pack(pop)

/* Memory Map Constants */
#define AFX_MEM_CLOCKS          (AICA_TOTAL_RAM - 32)
#define AFX_IPC_QUEUE_SZ        0x0400

/* Register Offsets */
#define AICA_REG_BASE    0x00800000

/* Resolve a file-relative offset to an absolute SPU address. */
static inline uint32_t afx_resolve_file_offset(uint32_t afx_base, uint32_t relative_offset) {
    return afx_base + relative_offset;
}

#endif /* AFX_COMMON_H */
