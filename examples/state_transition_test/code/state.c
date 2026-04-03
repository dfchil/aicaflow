#include <afx/aica_channel.h>
#include <afx/host.h>
#include <afx/memory.h>
// #include <afx/bin/guarded_blob.h>

#include <enDjinn/enj_enDjinn.h>
#include <enDjinn/ext/dca_file.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <terminal.h>

#define AICA_CLOCK_ADDR                                                        \
  0x001FFFE0 /* Reserve uppermost 4 words for clock registers */

#define AICA_HW_CLOCK                                                          \
  ((volatile uint32_t *)(SPU_RAM_BASE_SH4 + AICA_CLOCK_ADDR))
#define AICA_PREV_HW_CLOCK                                                     \
  ((volatile uint32_t *)(SPU_RAM_BASE_SH4 + AICA_CLOCK_ADDR + 4))
#define AICA_VIRTUAL_CLOCK                                                     \
  ((volatile uint32_t *)(SPU_RAM_BASE_SH4 + AICA_CLOCK_ADDR + 8))

#define drv_state_ptr                                                          \
  ((volatile afx_driver_state_t *)(SPU_RAM_BASE_SH4 + AFX_DRIVER_STATE_ADDR))

typedef void (*step_fn_t)(void *data);

typedef struct {
  terminal_buffer_t *terminal;
  uint32_t cur_step;
  uint32_t num_steps;
  step_fn_t *test_phases;
} state_state_t;

state_state_t *global_state;

static int init_afx_driver(void) {
  if (!afx_init()) {
    printf("afx_init failed\n");
    return -1;
  }
  printf("afx_init ok\n");
  return 0;
}

void main_mode_updater(void *data) {
  state_state_t *state = (state_state_t *)data;
  do {
    enj_ctrlr_state_t **ctrl_states = enj_ctrl_get_states();
    for (int curctrlr = 0; curctrlr < MAPLE_PORT_COUNT; curctrlr++) {
      if (ctrl_states[curctrlr] != NULL) {
        if (ctrl_states[curctrlr]->button.X == ENJ_BUTTON_DOWN_THIS_FRAME) {
          afx_driver_state_info(drv_state_ptr, "btn X pressed");
        }
        if (ctrl_states[curctrlr]->button.Y == ENJ_BUTTON_DOWN_THIS_FRAME) {
          afx_driver_state_info(drv_state_ptr, "btn Y pressed");
        }
        if (ctrl_states[curctrlr]->button.DOWN == ENJ_BUTTON_DOWN) {
          terminal_scroll(state->terminal, 6);
        }
        if (ctrl_states[curctrlr]->button.UP == ENJ_BUTTON_DOWN) {
          terminal_scroll(state->terminal, -6);
        }

        if (ctrl_states[curctrlr]->rtrigger > 10) {
          terminal_scroll(state->terminal,
                          ctrl_states[curctrlr]->rtrigger >> 4);
        }
        if (ctrl_states[curctrlr]->ltrigger > 10) {
          terminal_scroll(state->terminal,
                          -(ctrl_states[curctrlr]->ltrigger >> 4));
        }
      }
    }
  } while (0);
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
  state->cur_step = state->num_steps; // prevent further phases from running
}

void step_init_afx(void *data) {
  state_state_t *state = (state_state_t *)data;
  terminal_writeline(state->terminal, "Initializing AFX driver...");
  if (init_afx_driver() != 0) {
    terminal_writeline(state->terminal, "AFX driver init failed ... [FAIL]");
    step_final(data);
    return;
  }
  terminal_writeline(state->terminal, "");
}

void step_verify_afx_init(void *data) {
  char buffer[128];
  state_state_t *state = (state_state_t *)data;
  terminal_writeline(state->terminal, "Verifying AFX driver initialization...");
  if (drv_state_ptr->arm_status == 0) {
    terminal_writeline(state->terminal, "AFX driver arm_status == 0 ... [PASS]");
  } else {
    terminal_writeline(state->terminal,
                       "AFX driver arm_status should be 0 (IDLE) after init ... [FAIL]");
    step_final(data);
    return;
  }
  terminal_writeline(state->terminal, "");

  uint32_t *sp_val = (uint32_t *)(SPU_RAM_BASE_SH4 + 0x00100000u);
  if (*sp_val == (uint32_t)(&drv_state_ptr->mini_stack) - SPU_RAM_BASE_SH4 +
                     sizeof(drv_state_ptr->mini_stack)) {
    terminal_writeline(state->terminal,
                       "ARM7 stack pointer is set to end of mini_stack ... [PASS]");
  } else {
    terminal_writeline(
        state->terminal,
        "ARM7 stack pointer should be located at the end of mini_stack ... [FAIL]");
    printf("SP value: 0x%08X, expected: 0x%08X\n", *sp_val,
           (uint32_t)(&drv_state_ptr->mini_stack) - SPU_RAM_BASE_SH4 +
               sizeof(drv_state_ptr->mini_stack));
    step_final(data);
    return;
  }
  terminal_writeline(state->terminal, "");

  if (drv_state_ptr->stack_canary == 0xDEADB12D) {
    terminal_writeline(state->terminal,
                       "AFX driver stack canary == 0xDEADB12D ... [PASS]");
  } else {
    terminal_writeline(state->terminal, "AFX driver stack canary check failed ... [FAIL]");
    step_final(data);
    return;
  }
  terminal_writeline(state->terminal, "");

  terminal_writeline(state->terminal, "Verifying Aicaflow virtual clock...");
  uint32_t prev_virtual_clock = *AICA_VIRTUAL_CLOCK;
  for (volatile int i = 0; i < 1000000; i++) {
    // wait a bit to let the clock advance
  }
  uint32_t vr_clock = *AICA_HW_CLOCK;
  if (vr_clock > prev_virtual_clock) {
    snprintf(
        buffer, sizeof(buffer),
        "Aicaflow virtual clock advanced from %lu to %lu ... [PASS]", prev_virtual_clock, vr_clock);
    terminal_writeline(state->terminal, buffer);
  } else {
    terminal_writeline(state->terminal,
                       "Aicaflow virtual clock is not advancing ... [FAIL]");
    step_final(data);
    return;
  }
  terminal_writeline(state->terminal, "");
  terminal_writeline(state->terminal, "AFX driver init verified");
  terminal_writeline(state->terminal, "===============================");
}

void step_verify_flow_states_initialized(void *data) {
  state_state_t *state = (state_state_t *)data;
  terminal_writeline(state->terminal, "Verifying AFX flow states...");
  for (uint32_t i = 0; i < AFX_FLOW_POOL_CAPACITY; i++) {
    if (drv_state_ptr->flow_states[i].status != AFX_FLOW_AVAILABLE) {
      terminal_writeline(state->terminal,
                         "Expected all flow slots to be AVAILABLE after init ... [FAIL]");
      step_final(data);
      return;
    }
  }
  terminal_writeline(
      state->terminal,
      "AFX flow states all initialized to AFX_FLOW_AVAILABLE ... [PASS]");
  terminal_writeline(state->terminal, "===============================");
}

void step_verify_memory_layout(void *data) {
  state_state_t *state = (state_state_t *)data;
  char buffer[128];

  terminal_writeline(state->terminal,
                     "Verifying AFX memory layout and heap allocation...");

  size_t expected_available_memory =
      AFX_DRIVER_STATE_ADDR - align_up_u32(afx_get_driver_blob_size(), 32);
  uint32_t available_memory = afx_mem_available();

  if (available_memory == expected_available_memory) {
    snprintf(
        buffer, sizeof(buffer),
        "AFX initial %u bytes mem_available, matches expected value ... [PASS]",
        (unsigned)available_memory);
    terminal_writeline(state->terminal, buffer);
  } else {
    terminal_writeline(state->terminal,
                       "AFX initial mem_available does not match expected "
                       "value based on driver blob size ... [FAIL]");
    printf("Expected available memory: %u, got: %u\n",
           (unsigned)expected_available_memory, (unsigned)available_memory);
    step_final(data);
    return;
  }
  terminal_writeline(state->terminal, "");

  uint32_t expected_heap_start = align_up_u32(afx_get_driver_blob_size(), 32);
  uint32_t spu_heap_start = afx_mem_alloc(4, 32);
  if (spu_heap_start == 0) {
    terminal_writeline(state->terminal, "Failed to allocate from SPU heap ... [FAIL]");
    step_final(data);
    return;
  } else {
    snprintf(buffer, sizeof(buffer),
             "Allocated 4 bytes from SPU heap at 0x%08X", spu_heap_start);
    terminal_writeline(state->terminal, buffer);
  }
  terminal_writeline(state->terminal, "");

  if (spu_heap_start >= AFX_DRIVER_STATE_ADDR) {
    terminal_writeline(
        state->terminal,
        "SPU heap allocation should be located before driver state ... [FAIL]");
    step_final(data);
    return;
  } else if (spu_heap_start != expected_heap_start) {
    terminal_writeline(state->terminal,
                       "SPU heap start address does not match expected value ... [FAIL]");
    printf("Expected SPU heap start: 0x%08X, got: 0x%08X\n",
           expected_heap_start, spu_heap_start);
    step_final(data);
    return;
  } else {
    terminal_writeline(
        state->terminal,
        "SPU heap allocation is located just after driver blob ... [PASS]");
  }
  terminal_writeline(state->terminal, "");

  uint32_t prev_mem_available = available_memory;
  available_memory = afx_mem_available();
  if (available_memory == prev_mem_available - 32) {
    terminal_writeline(state->terminal,
                       "AFX mem_available decreased by 32 bytes after 4 byte "
                       "allocation... [PASS]");
  } else {
    terminal_writeline(state->terminal, "AFX mem_available did not decrease "
                                        "by expected amount after allocation");
    step_final(data);
    printf("Previous available memory: %u, got: %u\n",
           (unsigned)prev_mem_available, (unsigned)available_memory);
    return;
  }
  terminal_writeline(state->terminal, "");

  // now make a 2nd 4byte 32byte aligned allocation to verify alignment and that
  // multiple allocations work
  uint32_t second_alloc = afx_mem_alloc(4, 32);
  if (second_alloc == 0) {
    terminal_writeline(state->terminal,
                       "Failed to allocate second block from SPU heap ... [FAIL]");
    step_final(data);
    return;
  } else if (second_alloc != spu_heap_start + 32) {
    terminal_writeline(state->terminal,
                       "Second SPU heap allocation should be located 32 bytes "
                       "after first allocation ... [FAIL]");
    printf("Expected second allocation at: 0x%08X, got: 0x%08X\n",
           spu_heap_start + 32, second_alloc);
    step_final(data);
    return;
  } else {
    snprintf(buffer, sizeof(buffer),
             "Allocated second 4 byte block from SPU heap at 0x%08X, correctly "
             "aligned after first block ... [PASS]",
             second_alloc);
    terminal_writeline(state->terminal, buffer);
  }
  terminal_writeline(state->terminal, "");

  if (afx_mem_available() != prev_mem_available - 64) {
    terminal_writeline(state->terminal,
                       "AFX mem_available did not decrease by expected amount "
                       "after second allocation ... [FAIL]");
    printf("Previous available memory: %u, got: %u\n",
           (unsigned)prev_mem_available, (unsigned)afx_mem_available());
    step_final(data);
    return;
  } else {
    terminal_writeline(state->terminal,
                       "AFX mem_available decreased by 64 bytes after two 4 "
                       "byte allocations ... [PASS]");

    terminal_writeline(
        state->terminal,
        "\n(NB!: Minimum allocation block size is 32 bytes, to keep fragmentation, should probably be increased to 128)\n");
    terminal_writeline(
        state->terminal,
        "SPU heap allocation and AFX mem_available behavior verified");
    terminal_writeline(state->terminal, "===============================");
  }
  terminal_writeline(state->terminal, "");

  uint32_t test_value = 0x0D15EA5E; // zero-disease, from GC and Wii

  if (!afx_mem_write(spu_heap_start, &test_value, sizeof(test_value))) {
    terminal_writeline(state->terminal, "Failed to write to SPU heap ... [FAIL]");
    step_final(data);
    return;
  } else {
    snprintf(buffer, sizeof(buffer), "Wrote test value 0x%08X to SPU heap ... [PASS]",
             test_value);
    terminal_writeline(state->terminal, buffer);
  }
  terminal_writeline(state->terminal, "");

  if (*(uint32_t *)(SPU_RAM_BASE_SH4 + spu_heap_start) != test_value) {
    terminal_writeline(
        state->terminal,
        "Data read back from SPU heap does not match data written ... [FAIL]");
    printf("Expected: 0x%08X, got: 0x%08X\n", test_value,
           *(uint32_t *)(SPU_RAM_BASE_SH4 + spu_heap_start));
    step_final(data);
    return;
  } else {
    terminal_writeline(
        state->terminal,
        "Data read back from SPU heap matches data written ... [PASS]");
  }
  terminal_writeline(state->terminal, "");

  if (afx_mem_free(spu_heap_start)) {
    terminal_writeline(state->terminal, "Freed SPU heap allocation ... [PASS]");
  } else {
    terminal_writeline(state->terminal, "Failed to free SPU heap allocation ... [FAIL]");
    step_final(data);
    return;
  }
  terminal_writeline(state->terminal, "");

  if (afx_mem_available() != prev_mem_available - 32) {
    terminal_writeline(
        state->terminal,
        "AFX mem_available did not return to previous value after "
        "freeing first allocation ... [FAIL]");
    printf("Previous available memory: %u, got: %u\n",
           (unsigned)prev_mem_available, (unsigned)afx_mem_available());
    step_final(data);
    return;
  } else {
    terminal_writeline(state->terminal,
                       "AFX mem_available returned to previous value after "
                       "freeing first allocation ... [PASS]\n");
  }
  if (afx_mem_free(second_alloc)) {
    terminal_writeline(state->terminal,
                       "Freed second SPU heap allocation ... [PASS]");
  } else {
    terminal_writeline(state->terminal,
                       "Failed to free second SPU heap allocation ... [FAIL]");
    step_final(data);
    return;
  }
  if(afx_mem_available() != prev_mem_available) {
    terminal_writeline(
        state->terminal,
        "AFX mem_available did not return to initial value after freeing both "
        "allocations ... [FAIL]");
    printf("Expected available memory: %u, got: %u\n",
           (unsigned)prev_mem_available, (unsigned)afx_mem_available());
    step_final(data);
    return;
  } else {
    terminal_writeline(state->terminal,
                       "AFX mem_available returned to initial value after freeing "
                       "both allocations ... [PASS]");
  }

  terminal_writeline(state->terminal,
                     "AFX memory layout and heap allocation verified");
  terminal_writeline(state->terminal, "===============================");
}

void step_success(void *data) {
  state_state_t *state = (state_state_t *)data;
  terminal_writeline(state->terminal, "");
  terminal_writeline(state->terminal, "All tests completed successfully!");
  terminal_writeline(state->terminal, "===============================");
}

int main(__unused int argc, __unused char **argv) {

  enj_state_init_defaults();
  enj_state_soft_reset_set(ENJ_BUTTON_DOWN << (8 << 1));
  if (enj_state_startup() != 0) {
    ENJ_DEBUG_PRINT("enDjinn startup failed, exiting\n");
    return -1;
  }

  static step_fn_t state_phases[] = {
      step_init_afx,
      step_verify_afx_init,
      step_verify_flow_states_initialized,
      step_verify_memory_layout,

      step_success,
      step_final,
  };

  state_state_t state = (state_state_t){
      .cur_step = 0,
      .num_steps = sizeof(state_phases) / sizeof(state_phases[0]),
      .test_phases = state_phases,
  };
  state.terminal = malloc(sizeof(terminal_buffer_t));
  terminal_clear(state.terminal);

  enj_mode_t main_mode = {
      .name = "Main Mode",
      .mode_updater = main_mode_updater,
      .data = &state,
  };
  enj_mode_push(&main_mode);
  enj_state_run();
  free(state.terminal);
  return 0;
}
