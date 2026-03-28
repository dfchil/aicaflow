#include <afx/driver.h>
#include <afx/host.h>
#include <stddef.h>
#include <stdint.h>

/*
 * AICA Flow ARM7 Core Driver (Lean Flow Management)
 * Supports multiple concurrent .afx streams.
 */

#define AICA_CLOCK_ADDR                                                        \
  0x001FFFE0 /* Reserve uppermost 4 words for clock registers */

#define AICA_HW_CLOCK ((volatile uint32_t *)AICA_CLOCK_ADDR)
#define AICA_PREV_HW_CLOCK ((volatile uint32_t *)(AICA_CLOCK_ADDR + 4))
#define AICA_VIRTUAL_CLOCK ((volatile uint32_t *)(AICA_CLOCK_ADDR + 8))
#define AICA_CMD_COUNT ((volatile uint32_t *)(AICA_CLOCK_ADDR + 12))

#define AICA_SGLT_LO ((volatile uint32_t *)AICA_SGLT_LO_ADDR)
#define AICA_SGLT_HI ((volatile uint32_t *)AICA_SGLT_HI_ADDR)

#define drv_state_ptr ((volatile afx_driver_state_t *)AFX_DRIVER_STATE_ADDR)

static inline uint16_t apply_flow_tl_lut(volatile afx_flow_state_t *flow,
                                         uint16_t reg_value) {
  uint32_t lut_ptr = flow->tl_scale_lut_ptr;
  uint8_t tl = (uint8_t)(reg_value & 0xFFu);
  uint8_t scaled_tl = *(volatile uint8_t *)(uintptr_t)(lut_ptr + tl);
  return (uint16_t)((reg_value & 0xFF00u) | scaled_tl);
}

/*
 * In relative mode, FLOW writes encode SA_HI/SA_LO as offsets from afx_base.
 * If both words are present in this command, patch them to absolute AICA RAM.
 */
static inline uint32_t
patch_relative_sample_addr_words(const volatile afx_flow_state_t *flow,
                                 const afx_cmd_t *cmd, uint16_t *patched_sa_hi,
                                 uint16_t *patched_sa_lo) {
  uint32_t sa_hi_idx = 0xFFFFFFFFu;
  uint32_t sa_lo_idx = 0xFFFFFFFFu;
  uint32_t start = AFX_CMD_GET_OFFSET(cmd);
  uint32_t end = start + AFX_CMD_GET_LENGTH(cmd);

  if (start <= AICA_REG_SA_HI && AICA_REG_SA_HI < end) {
    sa_hi_idx = AICA_REG_SA_HI - start;
  }
  if (start <= AICA_REG_SA_LO && AICA_REG_SA_LO < end) {
    sa_lo_idx = AICA_REG_SA_LO - start;
  }

  if (sa_hi_idx == 0xFFFFFFFFu || sa_lo_idx == 0xFFFFFFFFu)
    return 0u;

  uint16_t sa_hi_word = cmd->values[sa_hi_idx];
  uint16_t sa_lo_word = cmd->values[sa_lo_idx];

  uint32_t relative_addr =
      (((uint32_t)sa_hi_word & 0x7Fu) << 16) | (uint32_t)sa_lo_word;
  uint32_t absolute_addr = flow->afx_base + relative_addr;

  *patched_sa_hi =
      (uint16_t)((sa_hi_word & 0xFF80u) | ((absolute_addr >> 16) & 0x7Fu));
  *patched_sa_lo = (uint16_t)(absolute_addr & 0xFFFFu);
  return 1u;
}

static inline void cmd2chnl(volatile afx_flow_state_t *flow,
                            const afx_cmd_t *cmd) {
  uint32_t hw_slot = ((uint8_t*)flow->channel_map)[AFX_CMD_GET_SLOT(cmd)];
  uint32_t base_ptr = AICA_REG_BASE + (hw_slot << 7);
  uint32_t reg_idx = AFX_CMD_GET_OFFSET(cmd);

  uint16_t patched_sa_hi = 0;
  uint16_t patched_sa_lo = 0;
  uint32_t have_patched_sa = 0u;
  if (!flow->sample_addr_mode) {
    have_patched_sa = patch_relative_sample_addr_words(
        flow, cmd, &patched_sa_hi, &patched_sa_lo);
  }

  uint32_t has_play_ctrl = AFX_CMD_GET_OFFSET(cmd) == AICA_REG_SA_HI
                               ? 1
                               : 0; // If SA_HI is being written, we'll write
                                    // play_ctrl last after patching

  for (uint32_t i = has_play_ctrl; i < AFX_CMD_GET_LENGTH(cmd); i++) {
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
  if (has_play_ctrl) {
    if (have_patched_sa) {
      *(volatile uint16_t *)(base_ptr) = patched_sa_hi;
    } else {
      *(volatile uint16_t *)(base_ptr) = cmd->values[0];
    }
  }

  (*AICA_CMD_COUNT)++;
}

static inline uint32_t flow_step_until_tick(volatile afx_flow_state_t *flow,
                                            uint32_t tick) {
  uint32_t offset = flow->flow_offset;
  const afx_header_t *hdr = (const afx_header_t *)(flow->afx_base);
  const afx_section_entry_t *flow_sect = find_afx_section(hdr, AFX_SECT_FLOW);

  uint32_t next_event_rel = 0;
  while (offset < flow_sect->size) {
    const afx_cmd_t *cmd =
        (const afx_cmd_t *)(uintptr_t)(flow->flow_ptr + offset);
    uint32_t cmd_global_tick = cmd->timestamp + flow->tick_adjust;

    if (cmd_global_tick > tick) {
      next_event_rel = cmd->timestamp;
      break;
    }

    cmd2chnl(flow, cmd);

    uint32_t cmd_size = 6u + ((uint32_t)AFX_CMD_GET_LENGTH(cmd) << 1);
    cmd_size = (cmd_size + 3u) & ~3u;
    offset += cmd_size;
  }

  flow->flow_offset = offset;

  if (offset >= flow_sect->size) {
    // Reached end of command stream; mark next_event_tick to emit as end.
    flow->next_event_tick = hdr->total_ticks;
  } else {
    flow->next_event_tick = next_event_rel;
  }

  return offset;
}

static inline void set_mini_stack(void) {
  /* Initialize Stack Pointer (SP) to the top of our reserved mini_stack.
     - mini_stack is 64 words (256 bytes); stack grows downward on ARM.
     - So initial SP should be base + size (one-past-end of buffer).
  */
  __asm__ volatile("add r0, %0, #256\n\t"
                   "mov sp, r0\n\t"
                   :
                   : "r"(drv_state_ptr->mini_stack)
                   : "r0", "sp");
}

void arm_main(void) {
  set_mini_stack();

  drv_state_ptr->arm_status = 0;
  drv_state_ptr->flow_count_active = 0;
  for (uint32_t i = 0; i < AFX_FLOW_POOL_CAPACITY; i++) {
    drv_state_ptr->flow_states[i].status = AFX_FLOW_AVAILABLE;
  }

  drv_state_ptr->stack_canary = 0xDEADB12D;
  *AICA_PREV_HW_CLOCK = *AICA_HW_CLOCK;
  *AICA_VIRTUAL_CLOCK = 0;
  *AICA_CMD_COUNT = 0;

  while (1) {
    uint32_t hw_delta = *AICA_HW_CLOCK - *AICA_PREV_HW_CLOCK;
    if (hw_delta > 0) {
      *AICA_PREV_HW_CLOCK = *AICA_HW_CLOCK;
      *AICA_VIRTUAL_CLOCK += hw_delta;
    }

    if (drv_state_ptr->stack_canary != 0xDEADB12D) {
      drv_state_ptr->arm_status = 3;
      break;
    }
    uint32_t active = 0;
    uint8_t i = 0;

    while (i < drv_state_ptr->flow_count_active) {
      volatile afx_flow_state_t *flow = drv_state_ptr->flow_states + i;
      if (!flow) {
        i++;
        continue;
      }
      if (flow->status == AFX_FLOW_PLAYING) {
        active = 1;
        // if (flow->next_event_tick + flow->tick_adjust <= *AICA_VIRTUAL_CLOCK) {
          flow_step_until_tick(flow, *AICA_VIRTUAL_CLOCK);
        // }

        if (((afx_header_t *)flow->afx_base)->total_ticks > 0 &&
            (*AICA_VIRTUAL_CLOCK) - flow->tick_adjust >=
                ((afx_header_t *)flow->afx_base)->total_ticks) {
          flow->status = AFX_FLOW_RETIRED;
        }
        i++;
      } else {
        i++;
      }
    }
    drv_state_ptr->arm_status = active ? 1u : 0u;
  }
}
