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
    
    PLAYER_STATE->song_base = 0;
    PLAYER_STATE->sample_base = 0;
    PLAYER_STATE->flow_ptr = 0;
    PLAYER_STATE->flow_count = 0;
    PLAYER_STATE->flow_idx = 0;
    PLAYER_STATE->next_event_tick = 0;
    PLAYER_STATE->is_playing = 0;
    PLAYER_STATE->loop_count = 0;

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

            PLAYER_STATE->song_base    = arg0;
            PLAYER_STATE->sample_base  = arg0 + hdr->sample_data_off;
            PLAYER_STATE->flow_ptr     = arg0 + hdr->flow_data_off;
            PLAYER_STATE->flow_count   = hdr->flow_data_size;

            if (hdr->dsp_coef_size > 0) {
                const uint32_t *src = (const uint32_t *)(uintptr_t)(arg0 + hdr->dsp_coef_off);
                uint32_t words = hdr->dsp_coef_size >> 2;
                for (uint32_t w = 0; w < words; w++) AICA_DSP_COEF[w] = src[w];
            }
            if (hdr->dsp_mpro_size > 0) {
                const uint32_t *src = (const uint32_t *)(uintptr_t)(arg0 + hdr->dsp_mpro_off);
                uint32_t words = hdr->dsp_mpro_size >> 2;
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
            break;
        case AICAF_CMD_SEEK:
        {
            uint32_t target = arg0;
            const afx_flow_cmd_t *flow = (const afx_flow_cmd_t *)(uintptr_t)PLAYER_STATE->flow_ptr;
            PLAYER_STATE->flow_idx = afx_flow_cmd_lower_bound_by_tick(
                flow,
                PLAYER_STATE->flow_count,
                target
            );
            *AICA_VIRTUAL_CLOCK = target;
            IPC_STATUS->current_tick = target;
            break;
        }
        default:
            break;
    }
}

/**
 * Execute Flow Command
 * Writing directly to G2 bus registers for specific slot/reg.
 */
static inline void execute_flow_cmd(const afx_flow_cmd_t *cmd) {
    volatile uint32_t *reg_ptr = (volatile uint32_t *)(0x00800000 + (cmd->slot * 0x80) + (cmd->reg * 4));
    uint32_t val = cmd->value;

    /* SA flow commands store a blob-local byte offset; add the cached absolute
     * sample base (set at PLAY time) and extract the correct bits. */
    if (cmd->reg == AICA_REG_SA_LO) {
        val = (cmd->value + PLAYER_STATE->sample_base) & 0xFFFF;
    } else if (cmd->reg == AICA_REG_SA_HI) {
        val = ((cmd->value + PLAYER_STATE->sample_base) >> 16) & 0xFF;
    } else if (cmd->reg == AICA_REG_TOT_LVL) {
        val = afx_scale_total_level(val, IPC_STATUS->volume);
    }

    *reg_ptr = val;
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
            uint32_t tail = IPC_STATUS->q_tail;
            afx_ipc_cmd_t c = IPC_QUEUE[tail];
            IPC_STATUS->q_tail = (tail + 1u) % AFX_IPC_QUEUE_CAPACITY;
            process_command(c.cmd, c.arg0);
        }
        
        // Update virtual clock based on true hardware ticks
        uint32_t current_hw = *AICA_HW_CLOCK;
        uint32_t delta = current_hw - *AICA_LAST_HW_CLOCK;
        if (delta > 0) {
            *AICA_LAST_HW_CLOCK = current_hw;
            if (PLAYER_STATE->is_playing) {
                *AICA_VIRTUAL_CLOCK += delta;
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
                const afx_flow_cmd_t *next_cmd = &((const afx_flow_cmd_t *)(uintptr_t)(PLAYER_STATE->flow_ptr))[PLAYER_STATE->flow_idx];

                if (*AICA_VIRTUAL_CLOCK >= next_cmd->timestamp) {
                    execute_flow_cmd(next_cmd);
                    PLAYER_STATE->flow_idx++;
                    IPC_STATUS->flow_pos = PLAYER_STATE->flow_idx;
                } else {
                    PLAYER_STATE->next_event_tick = next_cmd->timestamp;
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