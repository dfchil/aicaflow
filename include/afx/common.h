#ifndef AFX_COMMON_H
#define AFX_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Memory Map Constants */
#define AFX_MEM_CLOCKS (AICA_TOTAL_RAM - 32)

/* Register Offsets */
#define AICA_REG_BASE 0x00800000

#define AICAF_MAGIC 0xA1CAF100
#define AICAF_VERSION 1
#define AICA_BASE_FREQ 44100.0f
#define AICA_TOTAL_RAM (2 * 1024 * 1024)

#define AFX_ALIGN4(v) (((v) + 3u) & ~3u)
#define AFX_ALIGN32(v) (((v) + 31u) & ~31u)

/* Section IDs (ASCII 4CC stored in little-endian uint32) */
#define AFX_SECT_SDES 0x53454453u /* 'SDES' sample descriptors */
#define AFX_SECT_SDAT 0x54414453u /* 'SDAT' sample data */
#define AFX_SECT_DSPM 0x4D505344u /* 'DSPM' DSP code */
#define AFX_SECT_DSPC 0x43505344u /* 'DSPC' DSP configuration */

/* Sample format codes */
#define AFX_FMT_PCM16 0
#define AFX_FMT_PCM8 1
#define AFX_FMT_ADPCM 3

/* Loop mode codes */
#define AFX_LOOP_NONE 0
#define AFX_LOOP_FWD 1
#define AFX_LOOP_BIDIR 2

#define AFX_META_VERSION 1u

/* File-level flags (afx_header_t.flags) */
#define AFX_FILE_FLAG_EXTERNAL_SAMPLE_ADDRS 0x00000001u

/* AICA hardware Sound Generator Latch registers (common reg area).
 * Each bit corresponds to a slot (0-63). Hardware SETS a bit when a
 * non-looping sample's PCM + EG fully decays to silence.
 * Bit is cleared automatically when KEYON is written to that slot. */
#define AICA_SGLT_LO_ADDR 0x00802810u /* Slots  0-31 completion flags */
#define AICA_SGLT_HI_ADDR 0x00802814u /* Slots 32-63 completion flags */

/* Flow is_playing state values */
#define AFX_FLOW_STOPPED 0u /* Not playing, channels free */
#define AFX_FLOW_PLAYING 1u /* Actively stepping commands */
#define AFX_FLOW_PAUSED 2u  /* Command step paused, channels held */
#define AFX_FLOW_RETIRED 3u /* Commands done, waiting for HW channels to go silent */
#define AFX_FLOW_AVAILABLE 4u /* Available for new flow (not in retired list) */

/* SH4 -> ARM7 Command IDs */
#define AICAF_CMD_NONE 0
#define AICAF_CMD_PLAY_FLOW 1   /* arg0 = flow state SPU addr */
#define AICAF_CMD_STOP_FLOW 2   /* arg0 = flow state SPU addr */
#define AICAF_CMD_PAUSE_FLOW 3  /* arg0 = flow state SPU addr */
#define AICAF_CMD_RESUME_FLOW 4 /* arg0 = flow state SPU addr */
#define AICAF_CMD_VOLUME 5      /* arg0 = music volume (0-255) */
#define AICAF_CMD_SEEK_FLOW \
  6 /* arg0 = flow state SPU addr, arg1 = target tick */
#define AICAF_CMD_RETIRE_FLOW \
  7 /* arg0 = flow state SPU addr (flow self-retires at end) */

/* .afx file header */
typedef struct {
  uint32_t magic;      /* AICAF_MAGIC */
  uint32_t version;    /* AICAF_VERSION */
  uint32_t flow_offset; /* File-relative offset to flow commands (MANDATORY) */
  uint32_t flow_size;   /* Size of flow section in bytes */
  uint32_t total_ticks; /* Total song duration in ms */
  uint8_t
      section_count; /* Number of OPTIONAL sections following this header */
  uint8_t
      required_channels; /* Peak channels needed by song playback, mandatory */
  uint16_t flags;        /* Reserved */
} afx_header_t;

_Static_assert(sizeof(afx_header_t) == 24u, "afx_header_t size updated");

/* Section table entry */
typedef struct {
  uint32_t id;     /* AFX_SECT_* */
  uint32_t offset; /* file-relative byte offset */
  uint32_t size;   /* section size in bytes */
  uint32_t count;  /* entry count */
  uint32_t align;  /* alignment */
  uint32_t flags;  /* reserved */
} afx_section_entry_t;

/* Sample descriptor*/
typedef struct {
  uint32_t source_id; /* hash of originating WAV */
  uint8_t gm_program; /* GM program number */
  uint8_t format;     /* AFX_FMT_* */
  uint8_t loop_mode;  /* AFX_LOOP_* */
  uint8_t root_note;  /* MIDI note of recorded pitch */
  int8_t fine_tune;   /* cents offset from root_note (-100..+100) */
  uint8_t reserved[3];
  uint32_t sample_off;  /* byte offset into sample data blob */
  uint32_t sample_size; /* byte size of encoded sample data */
  uint32_t loop_start;  /* loop start byte offset relative to sample_off */
  uint32_t loop_end;    /* loop end byte offset relative to sample_off */
  uint32_t sample_rate; /* original sample rate in Hz */
} afx_sample_desc_t;

#pragma pack(push, 1)
/* Flow command (6 bytes + payload) */
typedef struct {
  uint32_t timestamp; /* Absolute time in ms */
  uint16_t pack;
  uint16_t values[];
} afx_cmd_t;
_Static_assert(sizeof(afx_cmd_t) == 6u, "afx_cmd_t must be 6 bytes header");

#define AFX_CMD_GET_SLOT(cmd)   ((cmd)->pack & 0x3F)
#define AFX_CMD_GET_OFFSET(cmd) (((cmd)->pack >> 6) & 0x1F)
#define AFX_CMD_GET_LENGTH(cmd) (((cmd)->pack >> 11) & 0x1F)

#define AFX_CMD_SET_SLOT(cmd, slot)   (cmd)->pack = ((cmd)->pack & ~0x3F) | ((slot) & 0x3F)
#define AFX_CMD_SET_OFFSET(cmd, off)  (cmd)->pack = ((cmd)->pack & ~(0x1F << 6)) | (((off) & 0x1F) << 6)
#define AFX_CMD_SET_LENGTH(cmd, len)  (cmd)->pack = ((cmd)->pack & ~(0x1F << 11)) | (((len) & 0x1F) << 11)

/* IPC Command (16 bytes) */
typedef struct {
  uint32_t cmd;
  uint32_t arg0;
  uint32_t arg1;
  uint32_t arg2;
} afx_ipc_cmd_t;

#pragma pack(pop)

#endif /* AFX_COMMON_H */
