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

#define SONG_HEADER  ((volatile afx_header_t *)AFX_PLAYER_STATE_ADDR)
#define IPC_STATUS   ((volatile afx_ipc_status_t *)AFX_IPC_STATUS_ADDR)
#define PLAYER_STATE ((volatile afx_player_state_t *)(AFX_PLAYER_STATE_ADDR + sizeof(afx_header_t)))

/* Internal Driver State - Absolute High-RAM Mapping */
typedef struct {
    uint32_t stream_idx;
    uint32_t next_event_tick;
    uint32_t is_playing;
    uint32_t loop_count;
} afx_player_state_t;

/**
 * Initialize Driver State at absolute addresses
 */
void driver_init(void) {
    IPC_STATUS->magic = AICAF_MAGIC;
    IPC_STATUS->arm_status = 0; // Idle
    IPC_STATUS->current_tick = 0;
    IPC_STATUS->stream_pos = 0;
    IPC_STATUS->cmd = 0;
    IPC_STATUS->volume = 255;
    
    PLAYER_STATE->stream_idx = 0;
    PLAYER_STATE->next_event_tick = 0;
    PLAYER_STATE->is_playing = 0;
    PLAYER_STATE->loop_count = 0;

    *AICA_LAST_HW_CLOCK = *AICA_HW_CLOCK;
    *AICA_VIRTUAL_CLOCK = 0;
}

/**
 * Execute Opcode
 * Writing directly to G2 bus registers for specific slot/reg.
 */
static inline void execute_opcode(const afx_opcode_t *op) {
    volatile uint32_t *reg_ptr = (volatile uint32_t *)(0x00800000 + (op->slot * 0x80) + (op->reg * 4));
    uint32_t val = op->value;

    /* SA opcodes store a blob-local byte offset; add the cached absolute
     * sample base (set at PLAY time) and extract the correct bits. */
    if (op->reg == AICA_REG_SA_LO) {
        val = (op->value + SONG_HEADER->sample_data_off) & 0xFFFF;
    } else if (op->reg == AICA_REG_SA_HI) {
        val = ((op->value + SONG_HEADER->sample_data_off) >> 16) & 0xFF;
    } else if (op->reg == AICA_REG_TOT_LVL) {
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
        // Process Incoming Commands (IPC)
        if (IPC_STATUS->cmd != 0) {
            switch (IPC_STATUS->cmd) {
                case AICAF_CMD_PLAY:
                {
                    // cmd_arg is the SPU RAM address of the .afx file
                    // Cache the fields we need as absolute SPU addresses
                    const afx_header_t *hdr = (const afx_header_t *)(uintptr_t)IPC_STATUS->cmd_arg;
                    SONG_HEADER->magic            = hdr->magic;
                    SONG_HEADER->sample_data_off  = IPC_STATUS->cmd_arg + hdr->sample_data_off;
                    SONG_HEADER->stream_data_off  = IPC_STATUS->cmd_arg + hdr->stream_data_off;
                    SONG_HEADER->stream_data_size = hdr->stream_data_size;

                    /* Upload DSP coefficients and microprogram if present */
                    if (hdr->dsp_coef_size > 0) {
                        const uint32_t *src = (const uint32_t *)(uintptr_t)(IPC_STATUS->cmd_arg + hdr->dsp_coef_off);
                        uint32_t words = hdr->dsp_coef_size >> 2;
                        for (uint32_t w = 0; w < words; w++) AICA_DSP_COEF[w] = src[w];
                    }
                    if (hdr->dsp_mpro_size > 0) {
                        const uint32_t *src = (const uint32_t *)(uintptr_t)(IPC_STATUS->cmd_arg + hdr->dsp_mpro_off);
                        uint32_t words = hdr->dsp_mpro_size >> 2;
                        for (uint32_t w = 0; w < words; w++) AICA_DSP_MPRO[w] = src[w];
                    }
                    
                    PLAYER_STATE->stream_idx = 0;
                    PLAYER_STATE->next_event_tick = 0;
                    PLAYER_STATE->is_playing = 1;
                    *AICA_VIRTUAL_CLOCK = 0;
                    
                    IPC_STATUS->stream_pos = 0;
                    IPC_STATUS->current_tick = 0;
                    IPC_STATUS->arm_status = 1; // Playing
                    break;
                }
                case AICAF_CMD_STOP:
                    PLAYER_STATE->is_playing = 0;
                    IPC_STATUS->arm_status = 0;
                    break;
                case AICAF_CMD_PAUSE:
                    PLAYER_STATE->is_playing = 0;
                    IPC_STATUS->arm_status = 2; // Paused
                    break;
                case AICAF_CMD_VOLUME:
                    /* IPC_STATUS->volume is written directly by the SH4 host before sending
                     * this command; execute_opcode() reads it on every TOT_LVL write. */
                    break;
                case AICAF_CMD_SEEK:
                {
                    /* Binary search for first opcode at or after target tick.
                     * Skips intermediate opcodes; voice state resumes from that point. */
                    uint32_t target = IPC_STATUS->cmd_arg;
                    const afx_opcode_t *stream = (const afx_opcode_t *)(uintptr_t)SONG_HEADER->stream_data_off;
                    PLAYER_STATE->stream_idx = afx_opcode_lower_bound_by_tick(
                        stream,
                        SONG_HEADER->stream_data_size,
                        target
                    );
                    *AICA_VIRTUAL_CLOCK = target;
                    IPC_STATUS->current_tick = target;
                    break;
                }
            }
            IPC_STATUS->cmd = 0; // Clear command to ack SH4
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
                PLAYER_STATE->stream_idx = 0; // Reset and catch up
            }

            IPC_STATUS->current_tick = *AICA_VIRTUAL_CLOCK;

            // Simple Streaming Interpreter (While loop allows fast catching up if seeked forward)
            while (PLAYER_STATE->is_playing && PLAYER_STATE->stream_idx < SONG_HEADER->stream_data_size) {
                const afx_opcode_t *next_op = &((const afx_opcode_t *)(uintptr_t)(SONG_HEADER->stream_data_off))[PLAYER_STATE->stream_idx];

                if (*AICA_VIRTUAL_CLOCK >= next_op->timestamp) {
                    execute_opcode(next_op);
                    PLAYER_STATE->stream_idx++;
                    IPC_STATUS->stream_pos = PLAYER_STATE->stream_idx;
                } else {
                    PLAYER_STATE->next_event_tick = next_op->timestamp;
                    break;
                }
            }

            if (PLAYER_STATE->stream_idx >= SONG_HEADER->stream_data_size) {
                // End of song logic
                PLAYER_STATE->is_playing = 0;
                IPC_STATUS->arm_status = 0; // Back to Idle
            }
        }
    }
}