#include <afx/aica_channel.h>
#include <afx/host.h>
#include <afx/memory.h>
#include <afx/driver.h>

#include <enDjinn/enj_enDjinn.h>
#include <enDjinn/ext/dca_file.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <terminal/terminal.h>

#define drv_state_ptr                                                          \
  ((volatile afx_driver_state_t *)(SPU_RAM_BASE_SH4 + AFX_DRIVER_STATE_ADDR))

typedef void (*step_fn_t)(void *data);

typedef struct {
  terminal_buffer_t *terminal;
  uint32_t cur_step;
  uint32_t num_steps;
  step_fn_t *test_phases;
  uint32_t flow_addr;
  uint8_t slot;
} state_state_t;

static int init_afx_driver(void) {
  if (!afx_init()) {
    return -1;
  }
  return 0;
}

void main_mode_updater(void *data) {
  state_state_t *state = (state_state_t *)data;
  
  enj_ctrlr_state_t **ctrl_states = enj_ctrl_get_states();
  for (int curctrlr = 0; curctrlr < MAPLE_PORT_COUNT; curctrlr++) {
    if (ctrl_states[curctrlr] != NULL) {
      if (ctrl_states[curctrlr]->button.X == ENJ_BUTTON_DOWN_THIS_FRAME) {
        afx_driver_state_info(drv_state_ptr, "debug info");
      }
      if (ctrl_states[curctrlr]->button.DOWN == ENJ_BUTTON_DOWN) {
        terminal_scroll(state->terminal, 6);
      }
      if (ctrl_states[curctrlr]->button.UP == ENJ_BUTTON_DOWN) {
        terminal_scroll(state->terminal, -6);
      }
    }
  }

  if (state->cur_step < state->num_steps) {
    state->test_phases[state->cur_step](data);
    state->cur_step++;
  }
  enj_render_list_add(PVR_LIST_PT_POLY, terminal_render, state->terminal);
}

void step_final(void *data) {
  state_state_t *state = (state_state_t *)data;
  terminal_writeline(state->terminal, "");
  terminal_writeline(state->terminal, "Press START to exit");
  state->cur_step = state->num_steps;
}

void step_init_afx(void *data) {
  state_state_t *state = (state_state_t *)data;
  terminal_writeline(state->terminal, "Initializing AFX driver...");
  if (init_afx_driver() != 0) {
    terminal_writeline(state->terminal, "AFX driver init failed ... [FAIL]");
    step_final(data);
    return;
  }
  terminal_writeline(state->terminal, "AFX driver init ... [PASS]");
}

// Dummy AFX file content (Header only)
static const uint32_t dummy_afx[] = {
    0xA1CAF100, // Magic 'AICA' (AICAF_MAGIC)
    0x00000001, // Version 1 (AICAF_VERSION)
    0x00000030, // flow_offset (48 bytes: header 24 + 1 sect 24)
    0x00000028, // flow_size (40 bytes)
    1000,       // total_ticks (1000)
    0x01,       // section_count = 1
    0x01,       // required_channels = 1
    0x0000,     // flags = 0
    // Section entry (afx_section_entry_t)
    0x54414453, // ID 'SDAT' (AFX_SECT_SDAT)
    48,         // offset 48
    40,         // size 40
    0, 0, 0     // count, align, flags
};

void step_test_ipc_activation(void *data) {
  state_state_t *state = (state_state_t *)data;
  terminal_writeline(state->terminal, "Testing Activation via IPC...");

  uint32_t addr = afx_mem_alloc(sizeof(dummy_afx), 32);
  afx_mem_write(addr, dummy_afx, sizeof(dummy_afx));
  state->flow_addr = addr;

  uint8_t slot = afx_flow_activate(addr);
  if (slot == 0xFFu) {
    terminal_writeline(state->terminal, "afx_flow_activate failed ... [FAIL]");
    step_final(data);
    return;
  }
  state->slot = slot;

  char buf[64];
  sprintf(buf, "Flow activated into slot %u", slot);
  terminal_writeline(state->terminal, buf);

  // Poll for ARM7 to consume the command
  uint32_t timeout = 1000000;
  while (drv_state_ptr->ipc_tail != drv_state_ptr->ipc_head && --timeout);

  if (drv_state_ptr->flow_states[slot].status == AFX_FLOW_STOPPED && 
      drv_state_ptr->flow_states[slot].afx_base == addr) {
    terminal_writeline(state->terminal, "ARM7 processed ACTIVATE ... [PASS]");
  } else {
    terminal_writeline(state->terminal, "ARM7 failed to process ACTIVATE ... [FAIL]");
    step_final(data);
  }
}

void step_test_ipc_play(void *data) {
  state_state_t *state = (state_state_t *)data;
  terminal_writeline(state->terminal, "Testing PLAY via IPC...");

  afx_flow_play(state->slot);

  uint32_t timeout = 1000000;
  while (drv_state_ptr->ipc_tail != drv_state_ptr->ipc_head && --timeout);

  if (drv_state_ptr->flow_states[state->slot].status == AFX_FLOW_PLAYING) {
    terminal_writeline(state->terminal, "ARM7 entered PLAYING state ... [PASS]");
  } else {
    terminal_writeline(state->terminal, "ARM7 failed to enter PLAYING ... [FAIL]");
  }
}

void step_success(void *data) {
  state_state_t *state = (state_state_t *)data;
  terminal_writeline(state->terminal, "");
  terminal_writeline(state->terminal, "IPC and Sparse Array tests passed!");
}

int main(__unused int argc, __unused char **argv) {
  enj_state_init_defaults();
  if (enj_state_startup() != 0) return -1;

  static step_fn_t state_phases[] = {
      step_init_afx,
      step_test_ipc_activation,
      step_test_ipc_play,
      step_success,
      step_final,
  };

  state_state_t state = {
      .cur_step = 0,
      .num_steps = sizeof(state_phases) / sizeof(state_phases[0]),
      .test_phases = state_phases,
  };
  state.terminal = malloc(sizeof(terminal_buffer_t));
  terminal_clear(state.terminal);

  enj_mode_t main_mode = {
      .name = "IPC Test",
      .mode_updater = main_mode_updater,
      .data = &state,
  };
  enj_mode_push(&main_mode);
  enj_state_run();
  return 0;
}
