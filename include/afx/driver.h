#ifndef AFX_DRIVER_H
#define AFX_DRIVER_H

#include "common.h"

#pragma pack(push, 1)

/* Flow-level flags (afx_flow_state_t.flags) */
#define AFX_FLOW_FLAG_SAMPLE_ADDRS_ABSOLUTE 0x00000001u

/* Flow State (one per active .afx stream; linked into active-list) */
typedef struct afx_flow_state {
    /* Doubly-linked list pointers */
    uint32_t prev_ptr;         /* SPU address of previous flow (0 if head) */
    uint32_t next_ptr;         /* SPU address of next flow (0 if tail) */
    
    /* Stream data */
    uint32_t afx_base;         /* Absolute SPU address of uploaded .afx */
    uint32_t flow_ptr;         /* afx_base + FLOW section offset */
    uint32_t flow_size;        /* FLOW section byte size */
    uint32_t flow_count;       /* FLOW section entry count */
    uint32_t flow_idx;         /* Current flow command index */
    uint32_t next_event_tick;  /* Timestamp of next command */
    
    /* Playback control */
    uint32_t is_playing;       /* AFX_FLOW_STOPPED/PLAYING/PAUSED/DRAINING */
    uint32_t loop_count;       /* Loop iteration count */
    uint32_t tl_scale_lut_ptr; /* Optional 256-byte TL LUT in AICA RAM (0=disabled) */
    uint64_t assigned_channels; /* Bitmask of channels assigned to this flow */
    uint32_t required_channels; /* Flow-local channels needed by this file */
    uint8_t channel_map[64];   /* File-local slot -> assigned hardware channel */
    uint32_t flags;            /* Reserved for future use */
} afx_flow_state_t;

/* Global Driver State (runtime + stack canary). */
typedef struct {
    uint32_t active_flows_head;  /* SPU address of first active flow (0 if empty) */
    uint32_t active_flows_tail;  /* SPU address of last active flow (0 if empty) */
    uint32_t stack_canary;       /* Stack overflow detection (0xDEADBEEF) */
    uint32_t mini_stack[64];     /* Small execution stack */
} afx_driver_state_t;

/* IPC Control Block (32 bytes, high-RAM) */
typedef struct {
    uint32_t magic;         /* AICAF_MAGIC */
    uint32_t arm_status;    /* 0=Idle, 1=Playing, 3=Error */
    uint32_t current_tick;  /* Current global playback tick (shared by all flows) */
    uint32_t volume;        /* Global music volume (0-255) */
    uint32_t q_head;        /* SH4 producer index */
    uint32_t q_tail;        /* ARM7 consumer index */
    uint32_t completed_flow_addr;   /* Last flow SPU addr completed by ARM7 */
    uint32_t completed_flow_seq;    /* Completion sequence number (monotonic) */
} afx_ipc_control_t;

/* Guard struct sizes at compile time. */
_Static_assert(sizeof(afx_ipc_control_t) == 32u, "afx_ipc_control_t size changed");
_Static_assert(sizeof(afx_flow_state_t) <= 128u, "afx_flow_state_t exceeds 128 bytes");
_Static_assert(sizeof(afx_driver_state_t) == 268u, "afx_driver_state_t size changed");

#pragma pack(pop)

/* Memory Map Addresses */
#define AFX_IPC_CONTROL_ADDR    ((AFX_MEM_CLOCKS - sizeof(afx_ipc_control_t)) & ~31)
#define AFX_IPC_CMD_QUEUE_ADDR  (AFX_IPC_CONTROL_ADDR - AFX_IPC_QUEUE_SZ)
#define AFX_DRIVER_STATE_ADDR   ((AFX_IPC_CMD_QUEUE_ADDR - sizeof(afx_driver_state_t)) & ~31)
#define AFX_FLOW_POOL_START     ((AFX_DRIVER_STATE_ADDR - 8192) & ~31)  /* Pool for flow states */

#define AFX_IPC_QUEUE_CAPACITY  (AFX_IPC_QUEUE_SZ / sizeof(afx_ipc_cmd_t))

#define AICA_REG_SA_HI    0x00
#define AICA_REG_SA_LO    0x01
#define AICA_REG_LSA      0x02
#define AICA_REG_LEA      0x03
#define AICA_REG_D2R_D1R  0x07
#define AICA_REG_EGH_RR   0x08
#define AICA_REG_AR_SR    0x09
#define AICA_REG_LNK_DL   0x0A
#define AICA_REG_FNS_OCT  0x0C
#define AICA_REG_TOT_LVL  0x0D
#define AICA_REG_PAN_VOL  0x0E

/* Inline Driver Utilities */

static inline uint32_t afx_scale_total_level(uint32_t tl, uint32_t volume) {
    uint32_t x = (255u - (tl & 0xFFu)) * (volume & 0xFFu);
    uint32_t scaled = (x + 1u + (x >> 8)) >> 8;
    return 255u - scaled;
}

static inline uint32_t afx_cmd_lower_bound_by_offset(const uint8_t *stream, uint32_t size, uint32_t count, uint32_t target_tick) {
    /* Variable-length streams cannot be binary searched by offset directly.
       We skip ahead based on the command size. */
    uint32_t curr_ptr = 0;
    uint32_t curr_idx = 0;
    while (curr_idx < count && curr_ptr < size) {
        const afx_cmd_t *cmd = (const afx_cmd_t *)(stream + curr_ptr);
        if (cmd->timestamp >= target_tick) return curr_ptr;
        
        uint32_t cmd_num_vals = cmd->length;
        uint32_t cmd_size = 6 + (cmd_num_vals << 1);
        // Commands are 4-byte aligned in the stream
        cmd_size = (cmd_size + 3) & ~3;
        
        curr_ptr += cmd_size;
        curr_idx++;
    }
    return curr_ptr;
}

#endif /* AFX_DRIVER_H */
