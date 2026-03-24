#include <stdint.h>
#include <stddef.h>
#include <afx/driver.h>
#include <afx/host.h>

/*
 * AICA Flow ARM7 Core Driver (Lean Linked-List Flow Management)
 * Supports multiple concurrent .afx streams.
 */

#define AICA_CLOCK_ADDR 0x001FFFE0

#define AICA_HW_CLOCK      ((volatile uint32_t *)AICA_CLOCK_ADDR)
#define AICA_PREV_HW_CLOCK ((volatile uint32_t *)(AICA_CLOCK_ADDR + 4))
#define AICA_VIRTUAL_CLOCK ((volatile uint32_t *)(AICA_CLOCK_ADDR + 8))

#define AICA_SGLT_LO  ((volatile uint32_t *)AICA_SGLT_LO_ADDR)
#define AICA_SGLT_HI  ((volatile uint32_t *)AICA_SGLT_HI_ADDR)

#define ipc_ctrl_ptr  ((volatile afx_ipc_control_t *)AFX_IPC_CONTROL_ADDR)
#define drv_state_ptr ((volatile afx_driver_state_t *)AFX_DRIVER_STATE_ADDR)
#define ipc_queue_ptr ((volatile afx_ipc_cmd_t *)AFX_IPC_CMD_QUEUE_ADDR)

static inline uint32_t ptr_to_u32(const volatile afx_flow_state_t *p) {
    return (uint32_t)(uintptr_t)p;
}

static inline volatile afx_flow_state_t *u32_to_ptr(uint32_t addr) {
    return (volatile afx_flow_state_t *)(uintptr_t)addr;
}

static inline void list_add_tail(volatile afx_flow_state_t *node) {
    if (!node) return;

    node->prev_ptr = drv_state_ptr->active_flows_tail;
    node->next_ptr = 0;

    if (drv_state_ptr->active_flows_tail) {
        volatile afx_flow_state_t *tail = u32_to_ptr(drv_state_ptr->active_flows_tail);
        tail->next_ptr = ptr_to_u32(node);
    } else {
        drv_state_ptr->active_flows_head = ptr_to_u32(node);
    }

    drv_state_ptr->active_flows_tail = ptr_to_u32(node);
}

static inline void list_remove(volatile afx_flow_state_t *node) {
    if (!node) return;

    uint32_t prev = node->prev_ptr;
    uint32_t next = node->next_ptr;

    if (prev) {
        volatile afx_flow_state_t *p = u32_to_ptr(prev);
        p->next_ptr = next;
    } else {
        drv_state_ptr->active_flows_head = next;
    }

    if (next) {
        volatile afx_flow_state_t *n = u32_to_ptr(next);
        n->prev_ptr = prev;
    } else {
        drv_state_ptr->active_flows_tail = prev;
    }

    node->prev_ptr = 0;
    node->next_ptr = 0;
}

static inline uint16_t apply_flow_tl_lut(volatile afx_flow_state_t *flow, uint16_t reg_value) {
    uint32_t lut_ptr = flow->tl_scale_lut_ptr;
    uint8_t tl = (uint8_t)(reg_value & 0xFFu);
    uint8_t scaled_tl = *(volatile uint8_t *)(uintptr_t)(lut_ptr + tl);
    return (uint16_t)((reg_value & 0xFF00u) | scaled_tl);
}

static inline uint32_t resolve_flow_slot(const volatile afx_flow_state_t *flow,
                                         uint32_t slot) {
    if (slot >= 64u) return 0u;
    if (flow->channel_map[slot] == 0xFFu) return 0u;
    return flow->channel_map[slot];
}

static inline uint32_t flow_uses_absolute_sample_addrs(const volatile afx_flow_state_t *flow) {
    return (flow->flags & AFX_FLOW_FLAG_SAMPLE_ADDRS_ABSOLUTE) != 0u;
}

/*
 * In relative mode, FLOW writes encode SA_HI/SA_LO as offsets from afx_base.
 * If both words are present in this command, patch them to absolute AICA RAM.
 */
static inline uint32_t patch_relative_sample_addr_words(const volatile afx_flow_state_t *flow,
                                                        const afx_cmd_t *cmd,
                                                        uint16_t *patched_sa_hi,
                                                        uint16_t *patched_sa_lo) {
    uint32_t sa_hi_idx = 0xFFFFFFFFu;
    uint32_t sa_lo_idx = 0xFFFFFFFFu;
    uint32_t start = cmd->offset;
    uint32_t end = start + cmd->length;

    if (start <= AICA_REG_SA_HI && AICA_REG_SA_HI < end) {
        sa_hi_idx = AICA_REG_SA_HI - start;
    }
    if (start <= AICA_REG_SA_LO && AICA_REG_SA_LO < end) {
        sa_lo_idx = AICA_REG_SA_LO - start;
    }

    if (sa_hi_idx == 0xFFFFFFFFu || sa_lo_idx == 0xFFFFFFFFu) return 0u;

    uint16_t sa_hi_word = cmd->values[sa_hi_idx];
    uint16_t sa_lo_word = cmd->values[sa_lo_idx];

    uint32_t relative_addr = (((uint32_t)sa_hi_word & 0x7Fu) << 16) | (uint32_t)sa_lo_word;
    uint32_t absolute_addr = flow->afx_base + relative_addr;

    *patched_sa_hi = (uint16_t)((sa_hi_word & 0xFF80u) | ((absolute_addr >> 16) & 0x7Fu));
    *patched_sa_lo = (uint16_t)(absolute_addr & 0xFFFFu);
    return 1u;
}

static inline void execute_cmd(volatile afx_flow_state_t *flow, const afx_cmd_t *cmd) {
    uint32_t hw_slot = resolve_flow_slot(flow, (uint32_t)cmd->slot);
    uint32_t base_ptr = AICA_REG_BASE + (hw_slot << 7);
    uint32_t reg_idx = cmd->offset;

    uint16_t patched_sa_hi = 0;
    uint16_t patched_sa_lo = 0;
    uint32_t have_patched_sa = 0u;
    if (!flow_uses_absolute_sample_addrs(flow)) {
        have_patched_sa =
            patch_relative_sample_addr_words(flow, cmd, &patched_sa_hi, &patched_sa_lo);
    }

    for (uint32_t i = 0; i < cmd->length; i++) {
        uint32_t current_reg = reg_idx + i;
        uint32_t reg_addr = base_ptr + (current_reg << 2);
        uint16_t reg_value = cmd->values[i];

        if (have_patched_sa) {
            if (current_reg == AICA_REG_SA_HI) {
                reg_value = patched_sa_hi;
            } else if (current_reg == AICA_REG_SA_LO) {
                reg_value = patched_sa_lo;
            }
        }

        if (current_reg == AICA_REG_TOT_LVL && flow->tl_scale_lut_ptr) {
            reg_value = apply_flow_tl_lut(flow, reg_value);
        }
        *(volatile uint16_t *)reg_addr = reg_value;
    }
}

static inline uint32_t flow_step_until_tick(volatile afx_flow_state_t *flow, uint32_t tick) {
    uint32_t offset = flow->flow_idx;

    while (offset < flow->flow_size) {
        const afx_cmd_t *cmd = (const afx_cmd_t *)(uintptr_t)(flow->flow_ptr + offset);
        if (cmd->timestamp > tick) {
            flow->next_event_tick = cmd->timestamp;
            break;
        }

        execute_cmd(flow, cmd);

        uint32_t cmd_size = 6u + ((uint32_t)cmd->length << 1);
        cmd_size = (cmd_size + 3u) & ~3u;
        offset += cmd_size;
    }

    flow->flow_idx = offset;
    return offset;
}

/* Returns non-zero if every slot in the mask has its SGLT completion bit set. */
static inline uint32_t channels_all_silent(uint64_t mask) {
    if (mask == 0) return 1u;
    uint64_t sglt = ((uint64_t)*AICA_SGLT_HI << 32) | *AICA_SGLT_LO;
    return ((sglt & mask) == mask) ? 1u : 0u;
}

static inline void signal_flow_completed(uint32_t flow_addr) {
    ipc_ctrl_ptr->completed_flow_addr = flow_addr;
    ipc_ctrl_ptr->completed_flow_seq = ipc_ctrl_ptr->completed_flow_seq + 1u;
}

void driver_init(void) {
    ipc_ctrl_ptr->magic = AICAF_MAGIC;
    ipc_ctrl_ptr->arm_status = 0;
    ipc_ctrl_ptr->current_tick = 0;
    ipc_ctrl_ptr->volume = 255;
    ipc_ctrl_ptr->q_head = 0;
    ipc_ctrl_ptr->q_tail = 0;
    ipc_ctrl_ptr->completed_flow_addr = 0;
    ipc_ctrl_ptr->completed_flow_seq = 0;

    drv_state_ptr->active_flows_head = 0;
    drv_state_ptr->active_flows_tail = 0;

    drv_state_ptr->stack_canary = 0xDEADBEEFu;
    *AICA_PREV_HW_CLOCK = *AICA_HW_CLOCK;
    *AICA_VIRTUAL_CLOCK = 0;
}

static inline void process_ipc_command(uint32_t cmd, uint32_t arg0, uint32_t arg1) {
    switch (cmd) {
        case AICAF_CMD_PLAY_FLOW: {
            volatile afx_flow_state_t *flow = u32_to_ptr(arg0);
            if (!flow) break;

            if (flow->required_channels == 0 || flow->required_channels > 64u) break;
            if (flow->assigned_channels == 0) break;
            if (flow->flow_ptr == 0 || flow->flow_size == 0 || flow->flow_count == 0) break;

            flow->flow_idx = 0;
            flow->next_event_tick = 0;

            flow->is_playing = AFX_FLOW_PLAYING;
            list_add_tail(flow);
            break;
        }

        case AICAF_CMD_STOP_FLOW:
        case AICAF_CMD_RETIRE_FLOW: {
            volatile afx_flow_state_t *flow = u32_to_ptr(arg0);
            if (!flow) break;
            flow->is_playing = AFX_FLOW_STOPPED;
            list_remove(flow);
            break;
        }

        case AICAF_CMD_PAUSE_FLOW: {
            volatile afx_flow_state_t *flow = u32_to_ptr(arg0);
            if (flow) flow->is_playing = AFX_FLOW_PAUSED;
            break;
        }

        case AICAF_CMD_RESUME_FLOW: {
            volatile afx_flow_state_t *flow = u32_to_ptr(arg0);
            if (flow) flow->is_playing = AFX_FLOW_PLAYING;
            break;
        }

        case AICAF_CMD_SEEK_FLOW: {
            volatile afx_flow_state_t *flow = u32_to_ptr(arg0);
            if (!flow || flow->flow_ptr == 0) break;
            flow->flow_idx = arg1;
            flow->next_event_tick = 0;
            break;
        }

        case AICAF_CMD_VOLUME:
            ipc_ctrl_ptr->volume = arg0 & 0xFFu;
            break;

        default:
            break;
    }
}

void arm_main(void) {
    driver_init();

    while (1) {
        uint32_t current_hw = *AICA_HW_CLOCK;
        uint32_t hw_delta = current_hw - *AICA_PREV_HW_CLOCK;
        if (hw_delta > 0) {
            *AICA_PREV_HW_CLOCK = current_hw;
            *AICA_VIRTUAL_CLOCK += hw_delta;
        }

        ipc_ctrl_ptr->current_tick = *AICA_VIRTUAL_CLOCK;

        if (drv_state_ptr->stack_canary != 0xDEADBEEFu) {
            ipc_ctrl_ptr->arm_status = 3;
            break;
        }

        while (ipc_ctrl_ptr->q_tail != ipc_ctrl_ptr->q_head) {
            volatile afx_ipc_cmd_t *c = &ipc_queue_ptr[ipc_ctrl_ptr->q_tail];
            process_ipc_command(c->cmd, c->arg0, c->arg1);
            ipc_ctrl_ptr->q_tail = (ipc_ctrl_ptr->q_tail + 1u) & (AFX_IPC_QUEUE_CAPACITY - 1u);
        }

        uint32_t active = 0;
        volatile afx_flow_state_t *flow = u32_to_ptr(drv_state_ptr->active_flows_head);
        while (flow) {
            uint32_t next_addr = flow->next_ptr;

            if (flow->is_playing == AFX_FLOW_PLAYING) {
                active = 1;
                flow_step_until_tick(flow, ipc_ctrl_ptr->current_tick);

                if (flow->flow_idx >= flow->flow_size) {
                    /* Commands exhausted — wait for HW channels to go silent. */
                    flow->is_playing = AFX_FLOW_DRAINING;
                }
            } else if (flow->is_playing == AFX_FLOW_DRAINING) {
                active = 1;
                if (channels_all_silent(flow->assigned_channels)) {
                    uint32_t completed_addr = ptr_to_u32(flow);
                    flow->is_playing = AFX_FLOW_STOPPED;
                    list_remove(flow);
                    signal_flow_completed(completed_addr);
                }
            }

            flow = u32_to_ptr(next_addr);
        }

        ipc_ctrl_ptr->arm_status = active ? 1u : 0u;
    }
}
