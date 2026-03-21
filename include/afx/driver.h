#ifndef AFX_DRIVER_H
#define AFX_DRIVER_H

#include "common.h"

#pragma pack(push, 1)

/* ARM7 Player State (Relocated near IPC region; size depends on fields below) */
typedef struct {
    uint32_t afx_base;         /* Absolute SPU address of uploaded .afx */
    uint32_t flow_ptr;         /* afx_base + FLOW section offset */
    uint32_t flow_count;       /* FLOW section entry count */
    uint32_t flow_idx;         /* Current flow command index */
    uint32_t next_event_tick;  /* Timestamp of next command */
    uint32_t is_playing;       /* Status flag */
    uint32_t loop_count;       /* Loop iteration count */
    /* Scratch slots to avoid stack locals in hot loops */
    uint32_t q_tail_latch;
    uint32_t q_cmd;
    uint32_t q_arg0;
    uint32_t q_arg1;
    uint32_t q_arg2;
    uint32_t seek_target;
    uint32_t flow_stream_ptr;
    uint32_t current_hw;
    uint32_t hw_delta;
    uint32_t reg_ptr;
    uint32_t reg_val;
    uint32_t resolved_addr;
    uint32_t next_cmd_ptr;
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
_Static_assert(sizeof(afx_player_state_t) == 340u, "afx_player_state_t size changed; update memory layout/docs");

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

static inline uint32_t afx_cmd_lower_bound_by_tick(const afx_cmd_t *stream, uint32_t count, uint32_t target_tick) {
    uint32_t lo = 0;
    uint32_t hi = count;
    while (lo < hi) {
        uint32_t mid = lo + ((hi - lo) >> 1);
        if (stream[mid].timestamp < target_tick) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

#endif /* AFX_DRIVER_H */
