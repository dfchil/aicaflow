#include <stdint.h>
#include <stddef.h>
#include <afx/driver.h>
#include <afx/host.h>

/**
 * AICA Flow ARM7 Core Driver (Standard C99/C11)
 * Optimized for Zero-Stack / Absolute Memory Mapping
 * Compatible with ARM GCC v8.x
 */

#define AICA_CLOCK_ADDR 0x001FFFE0 

#define AICA_HW_CLOCK      ((volatile uint32_t *)AICA_CLOCK_ADDR)
#define AICA_PREV_HW_CLOCK ((volatile uint32_t *)(AICA_CLOCK_ADDR + 4))
#define AICA_VIRTUAL_CLOCK ((volatile uint32_t *)(AICA_CLOCK_ADDR + 8))

/* AICA DSP register regoins (ARM7 address space) */
#define AICA_DSP_COEF ((volatile uint32_t *)0x00801000)
#define AICA_DSP_MPRO ((volatile uint32_t *)0x00803000)

#define ipc_state_ptr ((volatile afx_ipc_status_t *)AFX_IPC_STATUS_ADDR)
#define plr_state_ptr ((volatile afx_player_state_t *)AFX_PLAYER_STATE_ADDR)
#define ipc_queue_ptr ((volatile afx_ipc_cmd_t *)AFX_IPC_CMD_QUEUE_ADDR)

static inline void rebuild_tl_scale_lut(uint32_t volume) {
    uint32_t vol = volume & 0xFFu;
    for (uint32_t tl = 0; tl < 256u; tl++) {
        uint32_t x = (255u - tl) * vol;
        uint32_t scaled = (x + 1u + (x >> 8)) >> 8; /* floor(x/255) for x<=65025 */
        plr_state_ptr->tl_scale_lut[tl] = (uint8_t)(255u - scaled);
    }
    plr_state_ptr->tl_lut_volume = vol;
}

/**
 * Initialize Driver State at absolute addresses
 */
void driver_init(void) {
    ipc_state_ptr->magic = AICAF_MAGIC;
    ipc_state_ptr->arm_status = 0; // Idle
    ipc_state_ptr->current_tick = 0;
    ipc_state_ptr->flow_pos = 0;
    ipc_state_ptr->volume = 255;
    ipc_state_ptr->q_head = 0;
    ipc_state_ptr->q_tail = 0;
    
    plr_state_ptr->afx_base = 0;
    plr_state_ptr->flow_ptr = 0;
    plr_state_ptr->flow_count = 0;
    plr_state_ptr->flow_idx = 0;
    plr_state_ptr->next_event_tick = 0;
    plr_state_ptr->is_playing = 0;
    plr_state_ptr->loop_count = 0;
    plr_state_ptr->q_tail_latch = 0;
    plr_state_ptr->q_cmd = 0;
    plr_state_ptr->q_arg0 = 0;
    plr_state_ptr->q_arg1 = 0;
    plr_state_ptr->q_arg2 = 0;
    plr_state_ptr->seek_target = 0;
    plr_state_ptr->flow_stream_ptr = 0;
    plr_state_ptr->current_hw = 0;
    plr_state_ptr->hw_delta = 0;
    plr_state_ptr->reg_ptr = 0;
    plr_state_ptr->reg_val = 0;
    plr_state_ptr->resolved_addr = 0;
    plr_state_ptr->next_cmd_ptr = 0;
    plr_state_ptr->tl_lut_volume = 0xFFFFFFFFu;

    rebuild_tl_scale_lut(ipc_state_ptr->volume);

    *AICA_PREV_HW_CLOCK = *AICA_HW_CLOCK;
    *AICA_VIRTUAL_CLOCK = 0;
}

/**
 * Finds a specific section defined by its 4CC ID in an .afx file header.
 * The section table immediately follows the afx_header_t.
 */
static inline const afx_section_entry_t *afx_find_section(const afx_header_t *hdr,
                                                          uint32_t section_id) {
    const afx_section_entry_t *tab = (const afx_section_entry_t *)(hdr + 1);
    for (uint32_t i = 0; i < hdr->section_count; i++) {
        if (tab[i].id == section_id) return &tab[i];
    }
    return NULL;
}

static inline void process_ipc_command(uint32_t cmd, uint32_t arg0) {
    switch (cmd) {
        case AICAF_CMD_PLAY:
        {
            const afx_header_t *hdr = (const afx_header_t *)(uintptr_t)arg0;
            if (hdr->magic != AICAF_MAGIC || hdr->version != AICAF_VERSION) {
                ipc_state_ptr->arm_status = 3; // Error
                break;
            }

            const afx_section_entry_t *flow_sect = afx_find_section(hdr, AFX_SECT_FLOW);
            const afx_section_entry_t *sdat_sect = afx_find_section(hdr, AFX_SECT_SDAT);
            const afx_section_entry_t *dspc_sect = afx_find_section(hdr, AFX_SECT_DSPC);
            const afx_section_entry_t *dspm_sect = afx_find_section(hdr, AFX_SECT_DSPM);
            if (!flow_sect || !sdat_sect) {
                ipc_state_ptr->arm_status = 3; // Error
                break;
            }

            plr_state_ptr->afx_base     = arg0;
            plr_state_ptr->flow_ptr     = afx_resolve_file_offset(plr_state_ptr->afx_base, flow_sect->offset);
            plr_state_ptr->flow_count   = flow_sect->count;

            if (dspc_sect && dspc_sect->size > 0) {
                const uint32_t *src = (const uint32_t *)(uintptr_t)afx_resolve_file_offset(plr_state_ptr->afx_base, dspc_sect->offset);
                uint32_t words = dspc_sect->size >> 2;
                for (uint32_t w = 0; w < words; w++) AICA_DSP_COEF[w] = src[w];
            }
            if (dspm_sect && dspm_sect->size > 0) {
                const uint32_t *src = (const uint32_t *)(uintptr_t)afx_resolve_file_offset(plr_state_ptr->afx_base, dspm_sect->offset);
                uint32_t words = dspm_sect->size >> 2;
                for (uint32_t w = 0; w < words; w++) AICA_DSP_MPRO[w] = src[w];
            }

            plr_state_ptr->flow_idx = 0;
            plr_state_ptr->next_event_tick = 0;
            plr_state_ptr->is_playing = 1;
            *AICA_VIRTUAL_CLOCK = 0;

            ipc_state_ptr->flow_pos = 0;
            ipc_state_ptr->current_tick = 0;
            ipc_state_ptr->arm_status = 1;
            break;
        }
        case AICAF_CMD_STOP:
            plr_state_ptr->is_playing = 0;
            ipc_state_ptr->arm_status = 0;
            break;
        case AICAF_CMD_PAUSE:
            plr_state_ptr->is_playing = 0;
            ipc_state_ptr->arm_status = 2;
            break;
        case AICAF_CMD_VOLUME:
            ipc_state_ptr->volume = arg0 & 0xFF;
            if (plr_state_ptr->tl_lut_volume != ipc_state_ptr->volume) {
                rebuild_tl_scale_lut(ipc_state_ptr->volume);
            }
            break;
        case AICAF_CMD_SEEK:
        {
            plr_state_ptr->seek_target = arg0;
            plr_state_ptr->flow_stream_ptr = plr_state_ptr->flow_ptr;
            plr_state_ptr->flow_idx = afx_cmd_lower_bound_by_tick(
                (const afx_cmd_t *)(uintptr_t)plr_state_ptr->flow_stream_ptr,
                plr_state_ptr->flow_count,
                plr_state_ptr->seek_target
            );
            *AICA_VIRTUAL_CLOCK = plr_state_ptr->seek_target;
            ipc_state_ptr->current_tick = plr_state_ptr->seek_target;
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
    plr_state_ptr->reg_ptr = (uint32_t)(AICA_REG_BASE + ((uint32_t)cmd->slot << 7) + ((uint32_t)cmd->reg << 2));
    plr_state_ptr->reg_val = (uint32_t)cmd->value;

    /* SA_LO/HI contain absolute SPU addresses resolved from file-relative offsets. */
    switch (cmd->reg) {
        case AICA_REG_SA_LO:
            plr_state_ptr->resolved_addr = afx_resolve_file_offset(plr_state_ptr->afx_base, (uint32_t)cmd->value);
            plr_state_ptr->reg_val = plr_state_ptr->resolved_addr & 0xFFFF;
            break;
        case AICA_REG_SA_HI:
            plr_state_ptr->resolved_addr = afx_resolve_file_offset(plr_state_ptr->afx_base, (uint32_t)cmd->value);
            /* Bits [22:16] of absolute address go into play_ctrl Bits [6:0] */
            uint32_t val = (plr_state_ptr->resolved_addr >> 16) & 0x7F;
            /* Preserve existing flags (KeyOn/Loop/Format) if needed, 
               but usually the sequencer writes the whole control word. */
            plr_state_ptr->reg_val = (cmd->value & ~0x7Fu) | val;
            break;
        case AICA_REG_TOT_LVL:
            plr_state_ptr->reg_val = plr_state_ptr->tl_scale_lut[plr_state_ptr->reg_val & 0xFFu];
            break;
    }

    *((volatile uint32_t *)(uintptr_t)plr_state_ptr->reg_ptr) = plr_state_ptr->reg_val;
}

/**
 * ARM7 Main Function
 * Entry point after crt0.s setup.
 */
void arm_main(void) {
    driver_init();

    while (1) {
        // Process incoming queued commands (SH4 producer -> ARM7 consumer)
        while (ipc_state_ptr->q_tail != ipc_state_ptr->q_head) {
            plr_state_ptr->q_tail_latch = ipc_state_ptr->q_tail;
            plr_state_ptr->q_cmd = ipc_queue_ptr[plr_state_ptr->q_tail_latch].cmd;
            plr_state_ptr->q_arg0 = ipc_queue_ptr[plr_state_ptr->q_tail_latch].arg0;
            plr_state_ptr->q_arg1 = ipc_queue_ptr[plr_state_ptr->q_tail_latch].arg1;
            plr_state_ptr->q_arg2 = ipc_queue_ptr[plr_state_ptr->q_tail_latch].arg2;
            ipc_state_ptr->q_tail = (plr_state_ptr->q_tail_latch + 1u) & (AFX_IPC_QUEUE_CAPACITY - 1);
            process_ipc_command(plr_state_ptr->q_cmd, plr_state_ptr->q_arg0);
        }
        
        // Update virtual clock based on true hardware ticks
        plr_state_ptr->current_hw = *AICA_HW_CLOCK;
        plr_state_ptr->hw_delta = plr_state_ptr->current_hw - *AICA_PREV_HW_CLOCK;
        if (plr_state_ptr->hw_delta > 0) {
            *AICA_PREV_HW_CLOCK = plr_state_ptr->current_hw;
            if (plr_state_ptr->is_playing) {
                *AICA_VIRTUAL_CLOCK += plr_state_ptr->hw_delta;
            }
        }

        // Poll for Playback Trigger from SH4
        if (plr_state_ptr->is_playing) {
            // Did the SH4 host modify the virtual clock backwards? (Rewind)
            if (*AICA_VIRTUAL_CLOCK < ipc_state_ptr->current_tick) {
                plr_state_ptr->flow_idx = 0; // Reset and catch up
            }

            ipc_state_ptr->current_tick = *AICA_VIRTUAL_CLOCK;

            // Simple Streaming Interpreter (While loop allows fast catching up if seeked forward)
            while (plr_state_ptr->is_playing && plr_state_ptr->flow_idx < plr_state_ptr->flow_count) {
                plr_state_ptr->next_cmd_ptr = (uint32_t)(uintptr_t)&((const afx_cmd_t *)(uintptr_t)(plr_state_ptr->flow_ptr))[plr_state_ptr->flow_idx];

                if (*AICA_VIRTUAL_CLOCK >= ((const afx_cmd_t *)(uintptr_t)plr_state_ptr->next_cmd_ptr)->timestamp) {
                    execute_cmd((const afx_cmd_t *)(uintptr_t)plr_state_ptr->next_cmd_ptr);
                    plr_state_ptr->flow_idx++;
                    ipc_state_ptr->flow_pos = plr_state_ptr->flow_idx;
                } else {
                    plr_state_ptr->next_event_tick = ((const afx_cmd_t *)(uintptr_t)plr_state_ptr->next_cmd_ptr)->timestamp;
                    break;
                }
            }

            if (plr_state_ptr->flow_idx >= plr_state_ptr->flow_count) {
                // End of song logic
                plr_state_ptr->is_playing = 0;
                ipc_state_ptr->arm_status = 0; // Back to Idle
            }
        }
    }
}