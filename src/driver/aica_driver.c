#include <stdint.h>
#include <stddef.h>
#include <afx/afx.h>

/**
 * AICA Flow ARM7 Core Driver (Standard C99/C11)
 * Optimized for Zero-Stack / Absolute Memory Mapping
 * Compatible with ARM GCC v8.x
 */

#define AICA_CLOCK_ADDR 0x001FFFE0 

#define AICA_HW_CLOCK      ((volatile uint32_t *)AICA_CLOCK_ADDR)
#define AICA_LAST_HW_CLOCK ((volatile uint32_t *)(AICA_CLOCK_ADDR + 4))
#define AICA_VIRTUAL_CLOCK ((volatile uint32_t *)(AICA_CLOCK_ADDR + 8))

/* AICA DSP register regions (ARM7 address space) */
#define AICA_DSP_COEF ((volatile uint32_t *)0x00801000)
#define AICA_DSP_MPRO ((volatile uint32_t *)0x00803000)

#define IPC_STATUS   ((volatile afx_ipc_status_t *)AFX_IPC_STATUS_ADDR)
#define PLAYER_STATE ((volatile afx_player_state_t *)AFX_PLAYER_STATE_ADDR)
#define IPC_QUEUE    ((volatile afx_ipc_cmd_t *)AFX_IPC_CMD_QUEUE_ADDR)

/* Per-volume TL scaling table to keep the execute path multiply-free. */
static uint8_t g_tl_scale_lut[256];
static uint32_t g_tl_lut_volume = 0xFFFFFFFFu;

static inline void rebuild_tl_scale_lut(uint32_t volume) {
    uint32_t vol = volume & 0xFFu;
    for (uint32_t tl = 0; tl < 256u; tl++) {
        uint32_t x = (255u - tl) * vol;
        uint32_t scaled = (x + 1u + (x >> 8)) >> 8; /* floor(x/255) for x<=65025 */
        g_tl_scale_lut[tl] = (uint8_t)(255u - scaled);
    }
    g_tl_lut_volume = vol;
}

/**
 * Initialize Driver State at absolute addresses
 */
void driver_init(void) {
    IPC_STATUS->magic = AICAF_MAGIC;
    IPC_STATUS->arm_status = 0; // Idle
    IPC_STATUS->current_tick = 0;
    IPC_STATUS->flow_pos = 0;
    IPC_STATUS->volume = 255;
    IPC_STATUS->q_head = 0;
    IPC_STATUS->q_tail = 0;
    
    PLAYER_STATE->afx_base = 0;
    PLAYER_STATE->flow_ptr = 0;
    PLAYER_STATE->flow_count = 0;
    PLAYER_STATE->flow_idx = 0;
    PLAYER_STATE->next_event_tick = 0;
    PLAYER_STATE->is_playing = 0;
    PLAYER_STATE->loop_count = 0;
    PLAYER_STATE->q_tail_latch = 0;
    PLAYER_STATE->q_cmd = 0;
    PLAYER_STATE->q_arg0 = 0;
    PLAYER_STATE->q_arg1 = 0;
    PLAYER_STATE->q_arg2 = 0;
    PLAYER_STATE->seek_target = 0;
    PLAYER_STATE->flow_stream_ptr = 0;
    PLAYER_STATE->current_hw = 0;
    PLAYER_STATE->hw_delta = 0;
    PLAYER_STATE->reg_ptr = 0;
    PLAYER_STATE->reg_val = 0;
    PLAYER_STATE->resolved_addr = 0;
    PLAYER_STATE->next_cmd_ptr = 0;

    rebuild_tl_scale_lut(IPC_STATUS->volume);

    *AICA_LAST_HW_CLOCK = *AICA_HW_CLOCK;
    *AICA_VIRTUAL_CLOCK = 0;
}

static inline void process_command(uint32_t cmd, uint32_t arg0) {
    switch (cmd) {
        case AICAF_CMD_PLAY:
        {
            const afx_header_t *hdr = (const afx_header_t *)(uintptr_t)arg0;
            if (hdr->magic != AICAF_MAGIC || hdr->version != AICAF_VERSION) {
                IPC_STATUS->arm_status = 3; // Error
                break;
            }

            const afx_section_entry_t *flow_sect = afx_find_section(hdr, AFX_SECT_FLOW);
            const afx_section_entry_t *sdat_sect = afx_find_section(hdr, AFX_SECT_SDAT);
            const afx_section_entry_t *dspc_sect = afx_find_section(hdr, AFX_SECT_DSPC);
            const afx_section_entry_t *dspm_sect = afx_find_section(hdr, AFX_SECT_DSPM);
            if (!flow_sect || !sdat_sect) {
                IPC_STATUS->arm_status = 3; // Error
                break;
            }

            PLAYER_STATE->afx_base     = arg0;
            PLAYER_STATE->flow_ptr     = afx_resolve_file_offset(PLAYER_STATE->afx_base, flow_sect->offset);
            PLAYER_STATE->flow_count   = flow_sect->count;

            if (dspc_sect && dspc_sect->size > 0) {
                const uint32_t *src = (const uint32_t *)(uintptr_t)afx_resolve_file_offset(PLAYER_STATE->afx_base, dspc_sect->offset);
                uint32_t words = dspc_sect->size >> 2;
                for (uint32_t w = 0; w < words; w++) AICA_DSP_COEF[w] = src[w];
            }
            if (dspm_sect && dspm_sect->size > 0) {
                const uint32_t *src = (const uint32_t *)(uintptr_t)afx_resolve_file_offset(PLAYER_STATE->afx_base, dspm_sect->offset);
                uint32_t words = dspm_sect->size >> 2;
                for (uint32_t w = 0; w < words; w++) AICA_DSP_MPRO[w] = src[w];
            }

            PLAYER_STATE->flow_idx = 0;
            PLAYER_STATE->next_event_tick = 0;
            PLAYER_STATE->is_playing = 1;
            *AICA_VIRTUAL_CLOCK = 0;

            IPC_STATUS->flow_pos = 0;
            IPC_STATUS->current_tick = 0;
            IPC_STATUS->arm_status = 1;
            break;
        }
        case AICAF_CMD_STOP:
            PLAYER_STATE->is_playing = 0;
            IPC_STATUS->arm_status = 0;
            break;
        case AICAF_CMD_PAUSE:
            PLAYER_STATE->is_playing = 0;
            IPC_STATUS->arm_status = 2;
            break;
        case AICAF_CMD_VOLUME:
            IPC_STATUS->volume = arg0 & 0xFF;
            if (g_tl_lut_volume != IPC_STATUS->volume) {
                rebuild_tl_scale_lut(IPC_STATUS->volume);
            }
            break;
        case AICAF_CMD_SEEK:
        {
            PLAYER_STATE->seek_target = arg0;
            PLAYER_STATE->flow_stream_ptr = PLAYER_STATE->flow_ptr;
            PLAYER_STATE->flow_idx = afx_cmd_lower_bound_by_tick(
                (const afx_cmd_t *)(uintptr_t)PLAYER_STATE->flow_stream_ptr,
                PLAYER_STATE->flow_count,
                PLAYER_STATE->seek_target
            );
            *AICA_VIRTUAL_CLOCK = PLAYER_STATE->seek_target;
            IPC_STATUS->current_tick = PLAYER_STATE->seek_target;
            break;
        }
        default:
            break;
    }
}

/**
 * Execute Flow Command
 * Writing directly to G2 bus registers for specific slot/reg.
 * SA address fields are file-relative offsets resolved from afx_base.
 */
static inline void execute_cmd(const afx_cmd_t *cmd) {
    PLAYER_STATE->reg_ptr = (uint32_t)(0x00800000 + (cmd->slot * 0x80) + (cmd->reg * 4));
    PLAYER_STATE->reg_val = cmd->value;

    /* SA_LO and SA_HI contain file-relative offsets from .afx start. */
    if (cmd->reg == AICA_REG_SA_LO) {
        PLAYER_STATE->resolved_addr = afx_resolve_file_offset(PLAYER_STATE->afx_base, cmd->value);
        PLAYER_STATE->reg_val = PLAYER_STATE->resolved_addr & 0xFFFF;
    } else if (cmd->reg == AICA_REG_SA_HI) {
        PLAYER_STATE->resolved_addr = afx_resolve_file_offset(PLAYER_STATE->afx_base, cmd->value);
        PLAYER_STATE->reg_val = (PLAYER_STATE->resolved_addr >> 16) & 0xFF;
    } else if (cmd->reg == AICA_REG_TOT_LVL) {
        PLAYER_STATE->reg_val = g_tl_scale_lut[PLAYER_STATE->reg_val & 0xFFu];
    }

    *((volatile uint32_t *)(uintptr_t)PLAYER_STATE->reg_ptr) = PLAYER_STATE->reg_val;
}

/**
 * ARM7 Main Function
 * Entry point after crt0.s setup.
 */
void arm_main(void) {
    driver_init();

    while (1) {
        // Process incoming queued commands (SH4 producer -> ARM7 consumer)
        while (IPC_STATUS->q_tail != IPC_STATUS->q_head) {
            PLAYER_STATE->q_tail_latch = IPC_STATUS->q_tail;
            PLAYER_STATE->q_cmd = IPC_QUEUE[PLAYER_STATE->q_tail_latch].cmd;
            PLAYER_STATE->q_arg0 = IPC_QUEUE[PLAYER_STATE->q_tail_latch].arg0;
            PLAYER_STATE->q_arg1 = IPC_QUEUE[PLAYER_STATE->q_tail_latch].arg1;
            PLAYER_STATE->q_arg2 = IPC_QUEUE[PLAYER_STATE->q_tail_latch].arg2;
            IPC_STATUS->q_tail = (PLAYER_STATE->q_tail_latch + 1u) & (AFX_IPC_QUEUE_CAPACITY - 1);
            process_command(PLAYER_STATE->q_cmd, PLAYER_STATE->q_arg0);
        }
        
        // Update virtual clock based on true hardware ticks
        PLAYER_STATE->current_hw = *AICA_HW_CLOCK;
        PLAYER_STATE->hw_delta = PLAYER_STATE->current_hw - *AICA_LAST_HW_CLOCK;
        if (PLAYER_STATE->hw_delta > 0) {
            *AICA_LAST_HW_CLOCK = PLAYER_STATE->current_hw;
            if (PLAYER_STATE->is_playing) {
                *AICA_VIRTUAL_CLOCK += PLAYER_STATE->hw_delta;
            }
        }

        // Poll for Playback Trigger from SH4
        if (PLAYER_STATE->is_playing) {
            // Did the SH4 host modify the virtual clock backwards? (Rewind)
            if (*AICA_VIRTUAL_CLOCK < IPC_STATUS->current_tick) {
                PLAYER_STATE->flow_idx = 0; // Reset and catch up
            }

            IPC_STATUS->current_tick = *AICA_VIRTUAL_CLOCK;

            // Simple Streaming Interpreter (While loop allows fast catching up if seeked forward)
            while (PLAYER_STATE->is_playing && PLAYER_STATE->flow_idx < PLAYER_STATE->flow_count) {
                PLAYER_STATE->next_cmd_ptr = (uint32_t)(uintptr_t)&((const afx_cmd_t *)(uintptr_t)(PLAYER_STATE->flow_ptr))[PLAYER_STATE->flow_idx];

                if (*AICA_VIRTUAL_CLOCK >= ((const afx_cmd_t *)(uintptr_t)PLAYER_STATE->next_cmd_ptr)->timestamp) {
                    execute_cmd((const afx_cmd_t *)(uintptr_t)PLAYER_STATE->next_cmd_ptr);
                    PLAYER_STATE->flow_idx++;
                    IPC_STATUS->flow_pos = PLAYER_STATE->flow_idx;
                } else {
                    PLAYER_STATE->next_event_tick = ((const afx_cmd_t *)(uintptr_t)PLAYER_STATE->next_cmd_ptr)->timestamp;
                    break;
                }
            }

            if (PLAYER_STATE->flow_idx >= PLAYER_STATE->flow_count) {
                // End of song logic
                PLAYER_STATE->is_playing = 0;
                IPC_STATUS->arm_status = 0; // Back to Idle
            }
        }
    }
}