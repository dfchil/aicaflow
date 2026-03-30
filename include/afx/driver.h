#ifndef AFX_DRIVER_H
#define AFX_DRIVER_H

#include "common.h"

/* Flow channel map encoding */
#define AFX_FLOW_CHANNEL_MAP_ENTRIES 64
#define AFX_FLOW_CHANNEL_MAP_BITS_PER_ENTRY 6
#define AFX_FLOW_CHANNEL_MAP_INVALID ((uint32_t)0xFFFFFFFFu)
#define AFX_FLOW_CHANNEL_MAP_BYTE_SIZE \
  ((AFX_FLOW_CHANNEL_MAP_ENTRIES * AFX_FLOW_CHANNEL_MAP_BITS_PER_ENTRY) >> 3)

/* Flow pool geometry */
#define AFX_FLOW_POOL_CAPACITY 64u

/* Flow-level flags (afx_flow_state_t.flags) */
#define AFX_FLOW_FLAG_SAMPLE_ADDRS_ABSOLUTE 0x00000001u

#pragma pack(push, 1)  // Ensure no padding in these structures for direct
                       // memory mapping to SPU RAM

typedef struct afx_flow_state {
  /* Stream data */
  uint32_t afx_base;    /* Absolute SPU address of uploaded .afx */
  uint32_t flow_ptr;    /* afx_base + FLOW section offset */
  uint32_t flow_offset; /* Current flow command offset */
  uint32_t
      tick_adjust; /* Global tick offset for this flow (set at play time) */
  uint32_t next_event_tick; /* Timestamp of next command */
  uint32_t
      tl_scale_lut_ptr; /* Optional 256-byte TL LUT in AICA RAM (0=disabled) */
  struct {
    uint32_t sample_addr_mode
        : 1;             /* 0=Relative to afx_base, 1=Absolute SPU addr */
    uint32_t status : 3; /* AFX_FLOW_STOPPED/PLAYING/PAUSED/DRAINING */
    uint32_t reserved : 28;
  };
  uint32_t channel_map; /* absolute SPU address of channel map */
} afx_flow_state_t;
_Static_assert(sizeof(afx_flow_state_t) == 32u, "afx_flow_state_t != 32 bytes");

/* Global Driver State (runtime + stack canary). */
typedef struct {
  uint32_t stack_canary;   /* Stack overflow detection (0xDEADB12D) */
  uint32_t mini_stack[64]; /* Small execution stack */
  struct {
    uint32_t flow_count_active : 6; /* Number of active flows */
    uint32_t arm_status : 2;        /* 0=Idle, 1=Playing, 3=Error */
    uint32_t reserved : 24;
  };
  uint8_t chan_arenas[5]
                     [64]; /* arena[0] for 1-4 channels, arena[1] for 5-8
                              channels, arena[2] for 9-16 channels, arena[3] for
                              17-32 channels, arena[4] for 33-64 channels */
  afx_flow_state_t
      flow_states[AFX_FLOW_POOL_CAPACITY]; /* All possible simultaneously active
                                              driver state for easy access by
                                              ARM7 */
} afx_driver_state_t;

#pragma pack(pop)

/* Memory Map Addresses */
#define AFX_DRIVER_STATE_ADDR \
  ((AFX_MEM_CLOCKS - sizeof(afx_driver_state_t)) & ~31)

#define AICA_REG_SA_HI 0x00
#define AICA_REG_SA_LO 0x01
#define AICA_REG_LSA 0x02
#define AICA_REG_LEA 0x03
#define AICA_REG_ENV_AD 0x04
#define AICA_REG_ENV_DR 0x05
#define AICA_REG_PITCH 0x06
#define AICA_REG_LFO 0x07
#define AICA_REG_TOT_LVL 0x08
#define AICA_REG_PAN_VOL 0x09

#endif /* AFX_DRIVER_H */
