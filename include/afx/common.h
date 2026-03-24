#ifndef AFX_COMMON_H
#define AFX_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AICAF_MAGIC 0xA1CAF100
#define AICAF_VERSION 1
#define AICA_BASE_FREQ 44100.0f
#define AICA_TOTAL_RAM (2 * 1024 * 1024)

#define AFX_ALIGN4(v) (((v) + 3u) & ~3u)
#define AFX_ALIGN32(v) (((v) + 31u) & ~31u)

/* Section IDs (ASCII 4CC stored in little-endian uint32) */
#define AFX_SECT_FLOW 0x574F4C46u /* 'FLOW' */
#define AFX_SECT_SDES 0x53454453u /* 'SDES' */
#define AFX_SECT_SDAT 0x54414453u /* 'SDAT' */
#define AFX_SECT_DSPM 0x4D505344u /* 'DSPM' */
#define AFX_SECT_DSPC 0x43505344u /* 'DSPC' */
#define AFX_SECT_META 0x4154454Du /* 'META' */

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
#define AFX_FLOW_DRAINING                                                      \
  3u /* Commands done, waiting for HW channels to go silent */

/* SH4 -> ARM7 Command IDs */
#define AICAF_CMD_NONE 0
#define AICAF_CMD_PLAY_FLOW                                                    \
  1 /* arg0 = flow state SPU addr */
#define AICAF_CMD_STOP_FLOW 2   /* arg0 = flow state SPU addr */
#define AICAF_CMD_PAUSE_FLOW 3  /* arg0 = flow state SPU addr */
#define AICAF_CMD_RESUME_FLOW 4 /* arg0 = flow state SPU addr */
#define AICAF_CMD_VOLUME 5      /* arg0 = music volume (0-255) */
#define AICAF_CMD_SEEK_FLOW                                                    \
  6 /* arg0 = flow state SPU addr, arg1 = target tick */
#define AICAF_CMD_RETIRE_FLOW                                                  \
  7 /* arg0 = flow state SPU addr (flow self-retires at end) */

#pragma pack(push, 1)

/* .afx file header */
typedef struct {
  uint32_t magic;   /* AICAF_MAGIC */
  uint32_t version; /* AICAF_VERSION */
  uint32_t
      section_count; /* Number of sections immediately following this header */
  uint32_t total_ticks; /* Total song duration in ms */
  uint32_t flags;       /* Reserved */
} afx_header_t;

/* Section table entry */
typedef struct {
  uint32_t id;     /* AFX_SECT_* */
  uint32_t offset; /* file-relative byte offset */
  uint32_t size;   /* section size in bytes */
  uint32_t count;  /* entry count */
  uint32_t align;  /* alignment */
  uint32_t flags;  /* reserved */
} afx_section_entry_t;

/* Sample descriptor (32 bytes) */
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

/* Optional META section payload. */
typedef struct {
  uint32_t version;            /* AFX_META_VERSION */
  uint32_t required_channels;  /* Peak channels needed by song playback */
  uint32_t reserved[2];        /* Reserved for future metadata */
} afx_meta_t;

/* Flow command (8 bytes) */
typedef struct {
  uint32_t timestamp; /* Absolute time in ms */
  struct {
    uint16_t slot : 6;   /* AICA voice slot (0-63) */
    uint16_t offset : 5; /* 0-16 */
    uint16_t length : 5; /* 1-17 num writes */
  };
  uint16_t values[];
} afx_cmd_t;

typedef struct {
  union {
    uint16_t raw;
    struct {
      uint16_t sa_high : 7; // Sample Address [22:16]. Upper 7 bits of the
                            // 23-bit wave memory address.
      uint16_t pcms : 2;    // PCM Format (0=16-bit PCM, 1=8-bit PCM, 2=ADPCM).
      uint16_t lpctl : 1;   // Loop Control (0=No Loop, 1=Forward Loop).
      uint16_t ssctl : 1;   // Source Sample Control (Wait for zero crossings on
                            // looping).
      uint16_t reserved : 3; // Reserved hardware bits, keep 0.
      uint16_t key_on : 1;   // Key On Action. Triggers Address/EG fetching when
                             // combined with key_on_ex.
      uint16_t key_on_ex : 1; // Key State flag. Must be 1 to process the key_on
                              // command. Setting key_on=0, key_on_ex=1 releases
                              // the note.
    } bits;
  } play_ctrl; // 0x00 - Play Control Register (Format, Address High,
               // Triggering)

  uint16_t sa_low : 16; // 0x04 Sample Address [15:0]
  uint16_t lsa : 16; //  0x08 - Loop Start Address relative to the sample start
                     // address (in samples, not bytes).
  uint16_t lea : 16; // 0x0C - Loop End Address relative to the sample start
                     // address (in samples, not bytes).

  union {
    uint16_t raw;
    struct {
      uint16_t ar : 5; // Attack Rate (0-31, where 31 is fastest, 0 is
                       // infinite/muted).
      uint16_t reserved1 : 1;
      uint16_t
          d1r : 5; // Decay 1 Rate. Time to drop from full to Sustain Level.
      uint16_t d2r : 5; // Decay 2 Rate. Secondary decay while key is held.
    } bits;
  } env_ad; // 0x10 - Envelope Attack/Decay Rate Register

  union {
    uint16_t raw;
    struct {
      uint16_t rr : 5;  // Release Rate (0-31). Triggered when key_on goes low.
      uint16_t dl : 5;  // Sustain Level (0-31). Target level for D1R phase.
      uint16_t krs : 4; // Key Rate Scaling. 0 [X] = Minimum scaling E [X] =
                        // Maximum scaling F[X] = Scaling OFF
      uint16_t lpslnk : 1; // Loop Sustain Level Note Lock. If set, the sustain
                           // level is locked to the note's base frequency, so
                           // higher notes have a higher sustain level.
      uint16_t reserved : 1;
    } bits;
  } env_dr; // 0x14 - Envelope Release Rate and Sustain Level Register

  union {
    uint16_t raw;
    struct {
      uint16_t
          fns : 10; // Frequency Number (Pitch interpolation fractional amount).
      uint16_t reserved1 : 1;
      uint16_t
          oct : 4; // Octave Shift (Signed integer determining the base pitch).
      uint16_t reserved2 : 1;
    } bits;
  } pitch; // 0x18 - Pitch/Frequency Register

  union {
    uint16_t raw;
    struct {
      uint16_t alfos : 3;  // Amplitude LFO Depth (Tremolo amount).
      uint16_t alfows : 2; // Amplitude LFO Waveform Shape (0=Saw, 1=Square,
                           // 2=Triangle, 3=Random).

      uint16_t plfos : 3;  // Pitch LFO Depth (Sensitivity/Vibrato amount).
      uint16_t plfows : 2; // Pitch LFO Waveform Shape (0=Saw, 1=Square,
                           // 2=Triangle, 3=Random).
      uint16_t lfof : 5;   // LFO Frequency (Speed of oscillation).
      uint16_t lfore : 1;  // LFO Reset. Resets the LFO phase to 0 when set.
    } bits;
  } lfo; // 0x1C - Low Frequency Oscillator Control

  union {
    uint16_t raw;
    struct {
      uint16_t reserved1 : 1;
      uint16_t
          isel : 4; // Specifies the mix register address for each slot when
                    // sound slot output data is input to the DSP's mix register
                    // (MIXS). Note: MIXS is the input for DSP to obtain the sum
                    // of the input for all slots.
                    // - MIXS has an area for adding per slot, and an area for
                    // keeping the interval of one sample; these areas can be
                    // allocated alternately. Hence, reading by the DSP side can
                    // be done at any step.
                    // - Input to MIXS must be set so that the sum does not
                    // overflow 0 dB. (There is no overflow protect function.)

      uint16_t tl : 8; // Total Level (Attenuation). 0 is Maximum Volume, 255 is
                       // Muted. The actual amount of attenuation is specified
                       // by placing this value in the EG value.
      uint16_t reserved2 : 1;
    } bits;
  } env_fm; // 0x20 - Envelope FM Control. Can modulate the EG rates and levels
            // of other channels.

  union {
    uint16_t raw;
    struct {
      uint16_t
          dipan : 5; // Direct Pan. 0-15 Left to Center, 16-31 Center to Right.
      uint16_t reserved : 3;
      uint16_t disdl : 4; // Specifies the send level for each slot when direct
                          // data is output to DAC.
      uint16_t
          imxl : 4; // Specifies the send level for each slot when sound slot
                    // output data is input to the DSP's mix register (MIXS).
                    // - The actual send level is determined by multiplying
                    // this value by the Total Level (TL) value of env_fm.
                    // - If disdl or imxl is set to 0, the output is not sent
                    // to that destination, but it still contributes to the
                    // MIXS sum for the DSP.
    } bits;
  } pan; // 0x24 - Channel Panning and Direct Level Control

  union {
    uint16_t raw;
    struct {
      /** Resonance data Sets Q for the FEG filter. Values from
                       -3.00 through 20.25 dB can be set. The relationships
                      between bits and gain are as follows:

 * +----------+----------+----------+----------+
 * |   DATA   | GAIN (DB)|   DATA   | GAIN (DB)|
 * +----------+----------+----------+----------+
 * | 11111    |    20.25 | 00100    |     0.00 |
 * +----------+----------+----------+----------+
 * | 11100    |    18.00 | 00011    |    -0.75 |
 * +----------+----------+----------+----------+
 * | 11000    |    15.00 | 00010    |    -1.50 |
 * +----------+----------+----------+----------+
 * | 10000    |     9.00 | 00001    |    -2.25 |
 * +----------+----------+----------+----------+
 * | 01100    |     6.00 | 00000    |    -3.00 |
 * +----------+----------+----------+----------+
 * | 01000    |     3.00 |          |          |
 * +----------+----------+----------+----------+
 * | 00110    |     1.50 |          |          |
 * +----------+----------+----------+----------+
 */
      uint16_t q : 5;
      uint16_t reserved : 11;
    } bits;
  } resonance; // 0x28 - Resonance data

  uint16_t flv0; // 0x2C - [13:0] Cutoff frequency at the time of attack start
  uint16_t flv1; // 0x30 - [13:0] Cutoff frequency at the time of attack end
                 // (decay start time)
  uint16_t flv2; // 0x34 - [13:0] Cutoff frequency at the time of decay end
                 // (sustain start time)
  uint16_t flv3; // 0x38 - [13:0] Cutoff frequency at the time of KOFF
  uint16_t flv4; // 0x3C - [13:0] Cutoff frequency after release

  union {
    uint16_t raw;
    struct {
      uint16_t reserved1 : 3;
      uint16_t far : 5; // Specifies the rate of transition of FEG in attack
                        // status. (Volume transition is increased.)
      uint16_t reserved2 : 3;

      uint16_t fd1r : 5; // Specifies the rate of transition of FEG in decay 1
                         // status. (Volume transition is decreased.)
    } bits;
  } env_feg; // 0x40 - Envelope FEG Control. Can modulate the FEG cutoff
             // frequency of other channels.
  union {
    uint16_t raw;
    struct {
      uint16_t reserved1 : 3;
      uint16_t fd2r : 5; // Specifies the rate of transition of FEG in decay 2
                         // status. (Volume transition is decreased.)
      uint16_t reserved2 : 3;

      uint16_t frr : 5; // Specifies the rate of transition of FEG in release
                        // status. (Volume transition is decreased.)
    } bits;
  } env_feg2; // 0x44 - Additional Envelope FEG Control. Can modulate the FEG
              // cutoff frequency of other channels during decay 2 and release
              // phases.
} aica_chnl_packed_t;

/* IPC Command (16 bytes) */
typedef struct {
  uint32_t cmd;
  uint32_t arg0;
  uint32_t arg1;
  uint32_t arg2;
} afx_ipc_cmd_t;

#pragma pack(pop)

/* Memory Map Constants */
#define AFX_MEM_CLOCKS (AICA_TOTAL_RAM - 32)
#define AFX_IPC_QUEUE_SZ 0x0400

/* Register Offsets */
#define AICA_REG_BASE 0x00800000

/* Resolve a file-relative offset to an absolute SPU address. */
static inline uint32_t afx_resolve_file_offset(uint32_t afx_base,
                                               uint32_t relative_offset) {
  return afx_base + relative_offset;
}

#endif /* AFX_COMMON_H */
