#ifndef AFX_DRIVER_H
#define AFX_DRIVER_H

#include "common.h"

#pragma pack(push, 1)

/* Flow channel map encoding */
#define AFX_FLOW_CHANNEL_MAP_ENTRIES 64
#define AFX_FLOW_CHANNEL_MAP_BITS_PER_ENTRY 5
#define AFX_FLOW_CHANNEL_MAP_INVALID ((uint32_t)0xFFFFFFFFu)
#define AFX_FLOW_CHANNEL_MAP_BYTE_SIZE                                         \
  ((AFX_FLOW_CHANNEL_MAP_ENTRIES * AFX_FLOW_CHANNEL_MAP_BITS_PER_ENTRY) >> 3)

/* Flow pool geometry */
#define AFX_FLOW_POOL_CAPACITY 64u

/* Flow-level flags (afx_flow_state_t.flags) */
#define AFX_FLOW_FLAG_SAMPLE_ADDRS_ABSOLUTE 0x00000001u

/* Flow State (one per active .afx stream; linked into active-list) */
typedef struct afx_flow_state {
  /* Stream data */
  uint32_t afx_base; /* Absolute SPU address of½ uploaded .afx */
  uint32_t flow_ptr; /* afx_base + FLOW section offset */
  uint32_t flow_offset;        /* Current flow command offset */
//   uint32_t flow_count;       /* FLOW section entry count */
  uint32_t tick_offset;       /* Global tick offset for this flow (set at play time) */
  uint32_t next_event_tick; /* Timestamp of next command */

  /* Playback control */
  uint32_t
      tl_scale_lut_ptr; /* Optional 256-byte TL LUT in AICA RAM (0=disabled) */
  uint8_t channel_map[AFX_FLOW_CHANNEL_MAP_BYTE_SIZE]; /* 64 entries of 5 bits
                                                          each */
  struct {
    uint32_t
        sample_addr_mode : 1; /* 0=Relative to afx_base, 1=Absolute SPU addr */
    uint32_t status : 3;      /* AFX_FLOW_STOPPED/PLAYING/PAUSED/DRAINING */

  } flags; /* Reserved for future use */
} afx_flow_state_t;

_Static_assert(sizeof(afx_flow_state_t) <= 128u,
               "afx_flow_state_t exceeds 128 bytes");
_Static_assert((AFX_FLOW_POOL_CAPACITY * sizeof(afx_flow_state_t)) <= 8192u,
               "Flow pool must fit within 8KB slot");

/* Global Driver State (runtime + stack canary). */
typedef struct {
  uint8_t flow_count_active;  /* Number of active flows at start of entries[] */
  uint8_t flow_count_retired; /* Number of retired flows after active flows */
  uint8_t unused[2];          /* Padding for 32-bit alignment */
  uint32_t stack_canary; /* Stack overflow detection (0xDEADB12D) */
  afx_flow_state_t
      flow_entries[AFX_FLOW_POOL_CAPACITY]; /* Active flow states stored
                                               directly in driver state for easy
                                               access by ARM7 */
  uint32_t mini_stack[64];                  /* Small execution stack */
  struct {
    uint32_t arm_status : 2; /* 0=Idle, 1=Playing, 3=Error */
  } flags;
} afx_driver_state_t;

// /* IPC Control Block (32 bytes, high-RAM) */
// typedef struct {
//   uint32_t magic;      /* AICAF_MAGIC */
//   uint32_t arm_status; /* 0=Idle, 1=Playing, 3=Error */
//   uint32_t
//       current_tick; /* Current global playback tick (shared by all flows) */
//   uint32_t volume;  /* Global music volume (0-255) */
//   uint32_t q_head;  /* SH4 producer index */
//   uint32_t q_tail;  /* ARM7 consumer index */
//   uint32_t completed_flow_addr; /* Last flow SPU addr completed by ARM7 */
//   uint32_t completed_flow_seq;  /* Completion sequence number (monotonic) */
// } afx_ipc_control_t;


#pragma pack(pop)

/* Memory Map Addresses */
#define AFX_DRIVER_STATE_ADDR                                                  \
  ((AFX_MEM_CLOCKS - sizeof(afx_driver_state_t)) & ~31)

#define AICA_REG_SA_HI 0x00
#define AICA_REG_SA_LO 0x01
#define AICA_REG_LSA 0x02
#define AICA_REG_LEA 0x03
#define AICA_REG_D2R_D1R 0x07
#define AICA_REG_EGH_RR 0x08
#define AICA_REG_AR_SR 0x09
#define AICA_REG_LNK_DL 0x0A
#define AICA_REG_FNS_OCT 0x0C
#define AICA_REG_TOT_LVL 0x0D
#define AICA_REG_PAN_VOL 0x0E



/**
 * Get or set the channel map entry for the given slot index. Each entry is 5
 * bits, packed into a byte array. Returns AFX_FLOW_CHANNEL_MAP_INVALID
 * (0xFFFFFFFF) if the slot index is out of bounds. Setting a value will be
 * masked to the valid bit range.
 */
static inline uint32_t
afx_channel_map_get(const volatile afx_flow_state_t *flow, uint32_t slot) {
  if (!flow || slot >= AFX_FLOW_CHANNEL_MAP_ENTRIES)
    return AFX_FLOW_CHANNEL_MAP_INVALID;

  uint32_t bit_index = (slot << 2) + slot;
  uint32_t byte_index = bit_index >> 3;
  uint32_t bit_offset = bit_index & 7u;
  uint64_t raw = 0;
  raw |= (uint64_t)flow->channel_map[byte_index];
  raw |= (uint64_t)flow->channel_map[byte_index + 1] << 8;

  return (uint32_t)((raw >> bit_offset) &
                    ((1u << AFX_FLOW_CHANNEL_MAP_BITS_PER_ENTRY) - 1u));
}


static const afx_section_entry_t *find_afx_section(const afx_header_t *hdr,
                                                    uint32_t section_id) {
    if (!hdr) return NULL;
    const afx_section_entry_t *sections = (const afx_section_entry_t *)(hdr + 1);
    for (uint32_t i = 0; i < hdr->section_count; i++) {
        if (sections[i].id == section_id) return &sections[i];
    }
    return NULL;
}


#endif /* AFX_DRIVER_H */
