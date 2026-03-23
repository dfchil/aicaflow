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
    plr_state_ptr->stack_canary = 0xDEADBEEFu;
    plr_state_ptr->tl_lut_volume = 0xFFFFFFFFu;

    rebuild_tl_scale_lut(ipc_state_ptr->volume);

    *AICA_PREV_HW_CLOCK = *AICA_HW_CLOCK;
    *AICA_VIRTUAL_CLOCK = 0;

    /* Initialize Stack Pointer to the top of our reserved mini_stack.
       Stack grows downwards, so we point to &mini_stack[64]. */
    __asm__ volatile (
        "add r0, %0, #256\n\t"
        "mov sp, r0\n\t"
        : 
        : "r" (plr_state_ptr->mini_stack)
        : "r0", "sp"
    );
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
            plr_state_ptr->flow_size    = flow_sect->size;

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
            uint32_t seek_target = arg0;
            plr_state_ptr->flow_idx = afx_cmd_lower_bound_by_offset(
                (const uint8_t *)(uintptr_t)plr_state_ptr->flow_ptr,
                plr_state_ptr->flow_size,
                plr_state_ptr->flow_count,
                seek_target
            );
            /* Skip forward pointer to found offset */
            plr_state_ptr->flow_ptr += plr_state_ptr->flow_idx;
            *AICA_VIRTUAL_CLOCK = seek_target;
            ipc_state_ptr->current_tick = seek_target;
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
    uint32_t base_ptr = AICA_REG_BASE + ((uint32_t)cmd->slot << 7);
    uint32_t reg_idx = cmd->offset;
    
    for (uint32_t i = 0; i < cmd->length; i++) {
        uint32_t current_reg = reg_idx + i;
        uint32_t reg_addr = base_ptr + (current_reg << 2);
        uint32_t val = (uint32_t)cmd->values[i];

        /* Special handling for registers that need absolute address mapping or scaling */
        switch (current_reg) {
            case AICA_REG_SA_LO:
            {
                uint32_t resolved = afx_resolve_file_offset(plr_state_ptr->afx_base, val);
                val = resolved & 0xFFFFu;
                break;
            }
            case AICA_REG_SA_HI:
            {
                /* We assume the 'val' passed in here has the flags (KeyOn, etc) in high bits.
                   We only replace the low bits [6:0] with the upper bits of absolute address. */
                uint32_t resolved = afx_resolve_file_offset(plr_state_ptr->afx_base, val);
                uint32_t addr_hi = (resolved >> 16) & 0x7Fu;
                val = (val & ~0x7Fu) | addr_hi;
                break;
            }
            case AICA_REG_TOT_LVL:
                /* TL scale mapping (velocity curve) */
                val = plr_state_ptr->tl_scale_lut[val & 0xFFu];
                break;
        }

        *((volatile uint32_t *)(uintptr_t)reg_addr) = val;
    }
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
            uint32_t q_idx = ipc_state_ptr->q_tail;
            uint32_t q_cmd = ipc_queue_ptr[q_idx].cmd;
            uint32_t q_arg0 = ipc_queue_ptr[q_idx].arg0;
            
            ipc_state_ptr->q_tail = (q_idx + 1u) & (AFX_IPC_QUEUE_CAPACITY - 1);
            process_ipc_command(q_cmd, q_arg0);
        }
        
        // Update virtual clock based on true hardware ticks
        uint32_t current_hw = *AICA_HW_CLOCK;
        uint32_t hw_delta = current_hw - *AICA_PREV_HW_CLOCK;
        if (hw_delta > 0) {
            *AICA_PREV_HW_CLOCK = current_hw;
            if (plr_state_ptr->is_playing) {
                *AICA_VIRTUAL_CLOCK += hw_delta;
            }
        }

        // Poll for Playback Trigger from SH4
        if (plr_state_ptr->is_playing) {
            // Did the SH4 host modify the virtual clock backwards? (Rewind)
            if (*AICA_VIRTUAL_CLOCK < ipc_state_ptr->current_tick) {
                plr_state_ptr->flow_idx = 0; // Reset and catch up
            }

            ipc_state_ptr->current_tick = *AICA_VIRTUAL_CLOCK;
        // Check for stack overflow using our canary
        if (plr_state_ptr->stack_canary != 0xDEADBEEFu) {
            ipc_state_ptr->arm_status = 3; // Error
            plr_state_ptr->is_playing = 0;
        }
            // Simple Streaming Interpreter (While loop allows fast catching up if seeked forward)
            while (plr_state_ptr->is_playing && plr_state_ptr->flow_idx < plr_state_ptr->flow_count) {
                const afx_cmd_t *cmd = (const afx_cmd_t *)(uintptr_t)plr_state_ptr->flow_ptr;
                
                if (*AICA_VIRTUAL_CLOCK >= cmd->timestamp) {
                    execute_cmd(cmd);
                    
                    /* Advance pointer by variable command size (4 ts + 2 context + length * 2) */
                    uint32_t cmd_size = 6 + (cmd->length * 2);
                    /* Ensure 4-byte padding if an odd number of values were written */
                    if (cmd->length & 1) cmd_size += 2;
                    
                    plr_state_ptr->flow_ptr += cmd_size;
                    plr_state_ptr->flow_idx++;
                    ipc_state_ptr->flow_pos = plr_state_ptr->flow_idx;
                } else {
                    plr_state_ptr->next_event_tick = cmd->timestamp;
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