#include <afx/channels.h>
#include <afx/host.h>
#include <afx/memory.h>
#include <kos.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef KOS_HEADERS
#include <arch/timer.h>
#include <dc/sound/sound.h>
#include <dc/spu.h>
#endif

#define AICA_CLOCK_ADDR                                                        \
  0x001FFFE0 /* Reserve uppermost 4 words for clock registers */

#define AICA_HW_CLOCK                                                          \
  ((volatile uint32_t *)(SPU_RAM_BASE_SH4 + AICA_CLOCK_ADDR))
#define AICA_PREV_HW_CLOCK                                                     \
  ((volatile uint32_t *)(SPU_RAM_BASE_SH4 + AICA_CLOCK_ADDR + 4))
#define AICA_VIRTUAL_CLOCK                                                     \
  ((volatile uint32_t *)(SPU_RAM_BASE_SH4 + AICA_CLOCK_ADDR + 8))
#define AICA_CMD_COUNT                                                         \
  ((volatile uint32_t *)(SPU_RAM_BASE_SH4 + AICA_CLOCK_ADDR + 12))

#define AICA_DSP_COEF_ADDR 0x00801000u
#define AICA_DSP_MPRO_ADDR 0x00803000u

#define G2_WAIT_REG (*(volatile uint32_t *)0xa05f68a0)
#define AICA_ARM_EN_REG (*(volatile uint32_t *)0xa0702c00)
#define AFX_STATE

#define drv_state_ptr                                                          \
  ((volatile afx_driver_state_t *)(SPU_RAM_BASE_SH4 + AFX_DRIVER_STATE_ADDR))

static const afx_section_entry_t *find_afx_section(const afx_header_t *hdr,
                                                   uint32_t section_id) {
  if (!hdr)
    return NULL;
  const afx_section_entry_t *sections = (const afx_section_entry_t *)(hdr + 1);
  for (uint32_t i = 0; i < hdr->section_count; i++) {
    if (sections[i].id == section_id)
      return &sections[i];
  }
  return NULL;
}

/*
static void apply_song_dsp_sections_host(uint32_t flow_spu_addr) {
  const afx_header_t *hdr =
      (const afx_header_t *)(uintptr_t)(SPU_RAM_BASE_SH4 + flow_spu_addr);
  if (!hdr || hdr->magic != AICAF_MAGIC || hdr->version != AICAF_VERSION)
    return;

  const afx_section_entry_t *dspc_sect = find_afx_section(hdr, AFX_SECT_DSPC);
  const afx_section_entry_t *dspm_sect = find_afx_section(hdr, AFX_SECT_DSPM);

  if (dspc_sect && dspc_sect->size > 0) {
    const uint32_t *src =
        (const uint32_t *)(uintptr_t)(SPU_RAM_BASE_SH4 + flow_spu_addr +
                                      dspc_sect->offset);
    volatile uint32_t *dst =
        (volatile uint32_t *)(uintptr_t)(SPU_RAM_BASE_SH4 + AICA_DSP_COEF_ADDR);
    uint32_t words = dspc_sect->size >> 2;
    for (uint32_t w = 0; w < words; w++)
      dst[w] = src[w];
  }

  if (dspm_sect && dspm_sect->size > 0) {
    const uint32_t *src =
        (const uint32_t *)(uintptr_t)(SPU_RAM_BASE_SH4 + flow_spu_addr +
                                      dspm_sect->offset);
    volatile uint32_t *dst =
        (volatile uint32_t *)(uintptr_t)(SPU_RAM_BASE_SH4 + AICA_DSP_MPRO_ADDR);
    uint32_t words = dspm_sect->size >> 2;
    for (uint32_t w = 0; w < words; w++)
      dst[w] = src[w];
  }
}
*/

uint32_t afx_upload_tl_scale_lut(const uint8_t lut[256]) {
  if (!lut)
    return 0;

  uint32_t spu_addr = afx_mem_alloc(256u, 32u);
  if (spu_addr == 0)
    return 0;
  if (!afx_mem_write(spu_addr, lut, 256u))
    return 0;
  return spu_addr;
}

uint32_t afx_create_tl_scale_lut(uint8_t volume) {
  uint8_t lut[256];
  uint32_t vol = (uint32_t)volume;

  for (uint32_t tl = 0; tl < 256u; tl++) {
    uint32_t x = (255u - tl) * vol;
    uint32_t scaled = (x + 1u + (x >> 8)) >> 8;
    lut[tl] = (uint8_t)(255u - scaled);
  }

  return afx_upload_tl_scale_lut(lut);
}

bool afx_init(void) {
  G2_WAIT_REG = 0x1f;

  spu_disable();

  const uint32_t fw_spu_addr = 0;
  const uint32_t marker_addr = fw_spu_addr + afx_get_driver_blob_size();
  if (marker_addr < fw_spu_addr)
    return false;

  if (!range_fits_dynamic(fw_spu_addr,
                          afx_get_driver_blob_size() + sizeof(uint32_t)))
    return false;

  spu_memload(fw_spu_addr, (void *)afx_get_driver_blob(),
              afx_get_driver_blob_size());

  uint32_t dynamic_base = align_up_u32(marker_addr, 32);
  if (dynamic_base >= AFX_DRIVER_STATE_ADDR)
    return false;
  memset((void *)(uintptr_t)(SPU_RAM_BASE_SH4 + AFX_DRIVER_STATE_ADDR), 0,
         sizeof(afx_driver_state_t));

  afx_mem_reset(dynamic_base);

  spu_enable();

  /* Poll for ARM7 ready signal; spin up to ~200ms worth of iterations */
  uint32_t timeout = 0x800000u;
  while (drv_state_ptr->stack_canary != 0xDEADB12D && --timeout)
    ;
  if (drv_state_ptr->stack_canary != 0xDEADB12D)
    return false;

  return true;
}

static inline void afx_state_release_flow_slot(uint8_t slot) {
  if (slot < AFX_FLOW_POOL_CAPACITY) {
    /* release channels */
    uint64_t mask = 0;
    volatile afx_flow_state_t *flow_state =
        &drv_state_ptr->flow_states[slot];
    const afx_header_t *hdr =
        (const afx_header_t *)(uintptr_t)(SPU_RAM_BASE_SH4 +
                                          flow_state->afx_base);
    for (uint32_t ch = 0; ch < hdr->required_channels; ch++) {
      uint8_t hw_ch = ((uint8_t *)(uintptr_t)(SPU_RAM_BASE_SH4 + flow_state->channel_map))[ch];
      mask |= (1ULL << hw_ch);
    }
    afx_channels_release(mask);
    afx_channel_release_mapping(hdr->required_channels,
                                flow_state->channel_map);
    flow_state->status = AFX_FLOW_AVAILABLE;
  }
}

static uint8_t afx_state_allocate_flow_slot(uint8_t num_channels) {
  /* Find first available slot (Sparse Array Approach) */
  for (uint32_t i = 0; i < AFX_FLOW_POOL_CAPACITY; i++) {
    if (drv_state_ptr->flow_states[i].status == AFX_FLOW_AVAILABLE) {
      /* Mark as reserved but STOPPED; activation command will finish init */
      drv_state_ptr->flow_states[i].status = AFX_FLOW_STOPPED;
      return (uint8_t)i;
    }
  }

  /* Scan for retired slots to reclaim them */
  for (uint32_t i = 0; i < AFX_FLOW_POOL_CAPACITY; i++) {
    if (drv_state_ptr->flow_states[i].status == AFX_FLOW_RETIRED) {
      afx_state_release_flow_slot(i);
      drv_state_ptr->flow_states[i].status = AFX_FLOW_STOPPED;
      return (uint8_t)i;
    }
  }
  
  return 0xFFu;
}

void afx_driver_state_info(const volatile afx_driver_state_t *driver_state,
                           const char *label) {
  if (!driver_state) {
    printf("[AFX] afx_driver_state_info: driver_state is NULL\n");
    return;
  }

  const char *flow_status_names[] = {"AFX_FLOW_STOPPED", "AFX_FLOW_PLAYING",
                                     "AFX_FLOW_PAUSED", "AFX_FLOW_RETIRED",
                                     "AFX_FLOW_AVAILABLE"};

  uint32_t hw_clock = *AICA_HW_CLOCK;
  uint32_t prev_hw_clock = *AICA_PREV_HW_CLOCK;
  uint32_t virtual_clock = *AICA_VIRTUAL_CLOCK;
  uint32_t cmd_count = *AICA_CMD_COUNT;

  printf("[AFX] driver state @%s %p\n", label ? label : "",
         (const void *)driver_state);
  printf("[AFX]  stack_canary = 0x%08X\n",
         (unsigned)driver_state->stack_canary);
  printf("[AFX]  AICA_CMD_COUNT = %u\n", (unsigned)cmd_count);
  printf("[AFX]  arm_status = %u\n", (unsigned)driver_state->arm_status);
  printf("[AFX]  HW clock = %lu, prev = %lu, virtual = %lu\n", hw_clock,
         prev_hw_clock, virtual_clock);

  for (uint32_t i = 0; i < 5; i++) {
    const volatile afx_flow_state_t *f = &driver_state->flow_states[i];
    const char *status = "UNKNOWN";
    if (f->status < 5) {
      status = flow_status_names[f->status];
    }

    uint32_t total_ticks = 0;
    uint32_t flow_section_size = 0;
    uint32_t tick_progress = 0;
    uint32_t flow_progress = 0;

    uint32_t required_channels = 0;
    if (f->afx_base != 0) {
      const afx_header_t *hdr =
          (const afx_header_t *)(uintptr_t)(SPU_RAM_BASE_SH4 + f->afx_base);
      if (hdr) {
        required_channels = hdr->required_channels;
        total_ticks = hdr->total_ticks;
        flow_section_size = hdr->flow_size;
        if (total_ticks > 0) {
          tick_progress =
              (uint32_t)((uint64_t)f->next_event_tick * 100 / total_ticks);
        }
        if (flow_section_size > 0) {
          flow_progress =
              (uint32_t)((uint64_t)f->flow_offset * 100 / flow_section_size);
        }
      }
    }

    char channel_map_str[256];
    channel_map_str[0] = '\0';
    size_t cm_len = 0;
    cm_len += snprintf(channel_map_str + cm_len,
                       sizeof(channel_map_str) - cm_len, "[");
    for (uint32_t ch = 0; ch < required_channels; ch++) {
      uint32_t hw = ((uint8_t *)(uintptr_t)(SPU_RAM_BASE_SH4 + f->channel_map))[ch];
      if (hw != AFX_FLOW_CHANNEL_MAP_INVALID) {
        cm_len += snprintf(channel_map_str + cm_len,
                           sizeof(channel_map_str) - cm_len, "%s%u->%u",
                           cm_len > 1 ? "," : "", (unsigned)ch, (unsigned)hw);
        if (cm_len >= sizeof(channel_map_str) - 1)
          break;
      }
    }
    if (cm_len < sizeof(channel_map_str) - 1)
      snprintf(channel_map_str + cm_len, sizeof(channel_map_str) - cm_len, "]");
    else
      channel_map_str[sizeof(channel_map_str) - 1] = '\0';

    printf("[AFX]  flow_slot %02u: status=%s (raw=%u), afx_base=0x%08X, "
           "flow_ptr=0x%08X, flow_offset=%u/%u (%u%%), tick_adjust=%u, "
           "next_event_tick=%u/%u (%u%%), tl_lut=0x%08X, sample_addr_mode=%u, "
           "map=%s\n",
           (unsigned)i, status, (unsigned)f->status, (unsigned)f->afx_base,
           (unsigned)f->flow_ptr, (unsigned)f->flow_offset,
           (unsigned)flow_section_size, (unsigned)flow_progress,
           (unsigned)f->tick_adjust, (unsigned)f->next_event_tick,
           (unsigned)total_ticks, (unsigned)tick_progress,
           (unsigned)f->tl_scale_lut_ptr, (unsigned)f->sample_addr_mode,
           channel_map_str);
  }
}

/**
 * Activate a flow for the given aicaflow at the SPU address
 *
 * @param flow_spu_addr SPU address of the .afx file to activate; must have been
 * previously uploaded with afx_upload_afx() and not yet freed with
 * afx_free_afx()
 *
 * @return flow slot (0..63) on success, 0xFF on failure.
 *
 * @note This process cleans up slots marked by the SPU as retired, freeing
 * their bound channels.
 *
 * @note Loaded flows can be activated multiple times concurrently.
 *
 * @note Activating a flow does not automatically start playback; call
 * afx_flow_play() with the returned slot to start playback.
 * */
static inline bool afx_ipc_enqueue(uint32_t cmd, uint32_t arg0, uint32_t arg1,
                                   uint32_t arg2) {
  volatile afx_driver_state_t *driver = drv_state_ptr;
  uint32_t head = driver->ipc_head;
  uint32_t next_head = (head + 1) & (AFX_IPC_QUEUE_CAPACITY - 1);
  if (next_head == driver->ipc_tail) {
    return false; // Queue full
  }
  
  volatile afx_ipc_cmd_t *q_cmd = &driver->ipc_queue[head];
  q_cmd->cmd = cmd;
  q_cmd->arg0 = arg0; // flow_idx
  q_cmd->arg1 = arg1;
  q_cmd->arg2 = arg2;

  __asm__ __volatile__("":::"memory"); /* Compiler barrier */
  driver->ipc_head = next_head;
  return true;
}

uint8_t afx_flow_activate(uint32_t flow_spu_addr) {
  if (flow_spu_addr == 0 || flow_spu_addr > AFX_DRIVER_STATE_ADDR)
    return 0xFFu;

  const afx_header_t *hdr =
      (const afx_header_t *)(uintptr_t)(SPU_RAM_BASE_SH4 + flow_spu_addr);

  if (!hdr || hdr->magic != AICAF_MAGIC || hdr->version != AICAF_VERSION)
    return 0xFFu;

  const bool uses_external_samples =
      (hdr->flags & AFX_FILE_FLAG_EXTERNAL_SAMPLE_ADDRS) != 0u;
  const afx_section_entry_t *sdat_sect = find_afx_section(hdr, AFX_SECT_SDAT);
  if (!uses_external_samples && !sdat_sect)
    return 0xFFu;

  uint8_t flow_slot = afx_state_allocate_flow_slot(hdr->required_channels);
  if (flow_slot == 0xFFu)
    return 0xFFu;

  uint32_t required_channels = hdr->required_channels;
  if (required_channels == 0 || required_channels > 64u)
    return 0xFFu;

  uint64_t channel_mask = afx_channels_allocate(required_channels);
  if (channel_mask == 0)
    return 0xFFu;

  uint32_t channel_map_addr = afx_channel_setup_mapping(required_channels, channel_mask);

  /* Send ACTIVATE command instead of writing flow_state directly */
  /* arg1 = flow_spu_addr, arg2 = channel_map_addr */
  if (!afx_ipc_enqueue(AFX_CMD_ACTIVATE_FLOW, flow_slot, flow_spu_addr, channel_map_addr)) {
    return 0xFFu; // Queue full
  }

  return flow_slot;
}

void afx_flow_play(uint8_t flow_slot) {
  if (flow_slot >= AFX_FLOW_POOL_CAPACITY)
    return;

  afx_ipc_enqueue(AFX_CMD_PLAY_FLOW, flow_slot, 0, 0);
}

void afx_flow_pause(uint8_t flow_slot) {
  if (flow_slot >= AFX_FLOW_POOL_CAPACITY)
    return;
  afx_ipc_enqueue(AFX_CMD_PAUSE_FLOW, flow_slot, 0, 0);
}

void afx_flow_resume(uint8_t flow_slot) {
  if (flow_slot >= AFX_FLOW_POOL_CAPACITY)
    return;

  afx_ipc_enqueue(AFX_CMD_RESUME_FLOW, flow_slot, 0, 0);
}

void afx_flow_stop(uint8_t flow_slot) {
  if (flow_slot >= AFX_FLOW_POOL_CAPACITY)
    return;
  afx_ipc_enqueue(AFX_CMD_STOP_FLOW, flow_slot, 0, 0);
}

void afx_flow_seek(uint8_t flow_slot, uint32_t tick_ms) {
  if (flow_slot >= AFX_FLOW_POOL_CAPACITY)
    return;

  volatile afx_flow_state_t *flow = drv_state_ptr->flow_states + flow_slot;
  const afx_header_t *hdr =
      (const afx_header_t *)(uintptr_t)(SPU_RAM_BASE_SH4 + flow->afx_base);
  if (!hdr)
    return;

  uint32_t flow_offset = afx_cmd_lower_bound_by_offset(
      (const uint8_t *)flow->flow_ptr, hdr->flow_size, 0, tick_ms);

  // Send seek details to ARM7
  // arg1 = flow_offset, arg2 = tick_ms
  afx_ipc_enqueue(AFX_CMD_SEEK_FLOW, flow_slot, flow_offset, tick_ms);
}
