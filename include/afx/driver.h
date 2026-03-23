#ifndef AFX_DRIVER_H
#define AFX_DRIVER_H

#include "common.h"

#pragma pack(push, 1)

/* ARM7 Player State (Relocated near IPC region; size depends on fields below) */
typedef struct {
    uint32_t afx_base;         /* Absolute SPU address of uploaded .afx */
    uint32_t flow_ptr;         /* afx_base + FLOW section offset */
    uint32_t flow_size;        /* FLOW section byte size */
    uint32_t flow_count;       /* FLOW section entry count */
    uint32_t flow_idx;         /* Current flow byte offset from flow_ptr */
    uint32_t next_event_tick;  /* Timestamp of next command */
    uint32_t is_playing;       /* Status flag */
    uint32_t loop_count;       /* Loop iteration count */
    /* Small reserved stack space to allow for caller-save registers / small spills */
    uint32_t stack_canary;     /* Overflow protection word (0xDEADBEEF) */
    uint32_t mini_stack[64];
    /* Per-volume TL scaling LUT to keep execute path multiply-free. */
    uint8_t tl_scale_lut[256];
    uint32_t tl_lut_volume;
} afx_player_state_t;

/* IPC Status (32-byte aligned high-RAM block; address derived by AFX_IPC_STATUS_ADDR) - 32 bytes */
typedef struct {
    uint32_t magic;         /* AICAF_MAGIC */
    uint32_t arm_status;    /* 0=Idle, 1=Playing, 2=Paused, 3=Error */
    uint32_t current_tick;  /* Current playback tick */
    uint32_t flow_pos;      /* Offset into flow-command stream */
    uint32_t volume;        /* Music volume (0-255) */
    uint32_t q_head;        /* SH4 producer index */
    uint32_t q_tail;        /* ARM7 consumer index */
    uint32_t reserved;
} afx_ipc_status_t;

/* Guard memory-map critical struct sizes at compile time. */
_Static_assert(sizeof(afx_ipc_status_t) == 32u, "afx_ipc_status_t size changed; update memory layout/docs");
_Static_assert(sizeof(afx_player_state_t) == 552u, "afx_player_state_t size changed; update memory layout/docs");

#pragma pack(pop)

/* Memory Map Addresses */
#define AFX_IPC_STATUS_ADDR     ((AFX_MEM_CLOCKS - sizeof(afx_ipc_status_t)) & ~31)
#define AFX_IPC_CMD_QUEUE_ADDR  (AFX_IPC_STATUS_ADDR - AFX_IPC_QUEUE_SZ)
#define AFX_PLAYER_STATE_ADDR   ((AFX_IPC_CMD_QUEUE_ADDR - sizeof(afx_player_state_t)) & ~31)

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
        uint32_t cmd_size = 6 + (cmd_num_vals * 2);
        // Commands are 4-byte aligned in the stream
        cmd_size = (cmd_size + 3) & ~3;
        
        curr_ptr += cmd_size;
        curr_idx++;
    }
    return curr_ptr;
}

#endif /* AFX_DRIVER_H */
