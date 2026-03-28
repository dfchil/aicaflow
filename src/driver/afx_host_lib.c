#include <afx/bin/driver_blob.h>
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

#define AICA_CLOCK_ADDR 0x001FFFE0 /* Reserve uppermost 4 words for clock registers */

#define AICA_HW_CLOCK      ((volatile uint32_t *)(SPU_RAM_BASE_SH4 + AICA_CLOCK_ADDR))
#define AICA_PREV_HW_CLOCK ((volatile uint32_t *)(SPU_RAM_BASE_SH4 + AICA_CLOCK_ADDR + 4))
#define AICA_VIRTUAL_CLOCK ((volatile uint32_t *)(SPU_RAM_BASE_SH4 + AICA_CLOCK_ADDR + 8))
#define AICA_CMD_COUNT ((volatile uint32_t *)(SPU_RAM_BASE_SH4 + AICA_CLOCK_ADDR + 12))


#define AICA_DSP_COEF_ADDR 0x00801000u
#define AICA_DSP_MPRO_ADDR 0x00803000u

#define G2_WAIT_REG (*(volatile uint32_t *)0xa05f68a0)
#define AICA_ARM_EN_REG (*(volatile uint32_t *)0xa0702c00)
#define AFX_STATE

#define drv_state_ptr                                                          \
  ((volatile afx_driver_state_t *)(SPU_RAM_BASE_SH4 + AFX_DRIVER_STATE_ADDR))

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

void aica_shutdown(void) {
  // 1. Halt AICA ARM7 CPU
  AICA_ARM_EN_REG = AICA_ARM_EN_REG | 1u;

  // 2. Reset the AICA Hardware state from the SH-4 side
  // We clear the channel registers to stop all sound immediately
  volatile uint32_t *aica_reg = (volatile uint32_t *)SPU_RAM_BASE_SH4;
  for (int i = 0; i < 64; i++) {
    volatile uint32_t *slot =
        (volatile uint32_t *)((uint8_t *)aica_reg + (i * 128));
    slot[0] = 0x8000;
    for (int j = 1; j < 32; j++)
      slot[j] = 0;
  }
}

bool afx_init(void) {
  G2_WAIT_REG = 0x1f;

  aica_shutdown();

  const void *fw_data = afx_driver_data;
  uint32_t fw_size = (uint32_t)afx_driver_size;

  uint32_t fw_spu_addr = 0;
  uint32_t marker_addr = fw_spu_addr + fw_size;
  if (marker_addr < fw_spu_addr)
    return false;

  if (!range_fits_dynamic(fw_spu_addr, fw_size + sizeof(uint32_t)))
    return false;

  spu_memload(fw_spu_addr, (void*)fw_data, fw_size);

  uint32_t dynamic_base = align_up_u32(marker_addr + sizeof(uint32_t), 32);
  if (dynamic_base >= AFX_DRIVER_STATE_ADDR)
    return false;

  memset((void *)(uintptr_t)(SPU_RAM_BASE_SH4 + AFX_DRIVER_STATE_ADDR), 0,
         sizeof(afx_driver_state_t));

  afx_mem_reset(dynamic_base);
  g_host_available_channels = 0xFFFFFFFFFFFFFFFFULL;

  /* Halt then re-enable ARM7 so it boots from the freshly uploaded firmware */
  // 5. Restart ARM7 CPU
  AICA_ARM_EN_REG = AICA_ARM_EN_REG & ~1;

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
    const volatile afx_flow_state_t *flow_state =
        &drv_state_ptr->flow_states[slot];
    const afx_header_t *hdr =
        (const afx_header_t *)(uintptr_t)(SPU_RAM_BASE_SH4 +
                                          flow_state->afx_base);
    uint32_t channel_map_arena = flow_state->channel_map >> 6u;
    uint32_t channel_arena_offset = (flow_state->channel_map & 0x3Fu);
    for (uint32_t ch = 0; ch < hdr->required_channels; ch++) {
      uint8_t hw_ch = ((uint8_t*)flow_state->channel_map)[ch];
      mask |= (1ULL << hw_ch);
    }
    afx_channels_release(mask);

    if (slot == drv_state_ptr->flow_count_active - 1) {
      /* Simple case: just decrement active count to retire the last slot */
      drv_state_ptr->flow_states[slot].status = AFX_FLOW_AVAILABLE;
      drv_state_ptr->flow_count_active--;
      return;
    }
    /* find last active slot and copy that to retired slot location */
    for (int8_t i = drv_state_ptr->flow_count_active - 1; i > slot; i--) {
      if (drv_state_ptr->flow_states[i].status != AFX_FLOW_RETIRED) {
        /* Move this active slot down to the retired slot */
        drv_state_ptr->flow_states[slot] = drv_state_ptr->flow_states[i];
        /* Mark the moved slot as retired */
        drv_state_ptr->flow_states[i].status = AFX_FLOW_AVAILABLE;
        drv_state_ptr->flow_count_active--;
        return;
      }
    }
  }
}

static uint8_t afx_state_allocate_flow_slot(uint8_t num_channels) {

  /* Free up any retired slots before allocating a new one. This also releases
  channels bound to retired slots so that they can be reused by new flows. We do
  this in the host code since the SPU firmware is focused on real-time flow
  processing and doesn't need to manage slot retirement or channel reuse logic.

  Going backwards makes it computationally simpler to handle retired slots */
  for (int8_t i = drv_state_ptr->flow_count_active - 1; i >= 0; i--) {
    if (drv_state_ptr->flow_states[i].status == AFX_FLOW_RETIRED) {
      afx_state_release_flow_slot(i);
    }
  }
  if (drv_state_ptr->flow_count_active >= AFX_FLOW_POOL_CAPACITY) {
    return 0xFFu;
  }

  uint8_t slot = drv_state_ptr->flow_count_active;
  drv_state_ptr->flow_count_active++;
  return slot;
}

void afx_driver_state_info(const volatile afx_driver_state_t *driver_state, const char* label) {
  if (!driver_state) {
    printf("[AFX] afx_driver_state_info: driver_state is NULL\n");
    return;
  }

  const char *flow_status_names[] = {
      "AFX_FLOW_STOPPED", "AFX_FLOW_PLAYING", 
      "AFX_FLOW_PAUSED", "AFX_FLOW_RETIRED", "AFX_FLOW_AVAILABLE"};

  uint32_t hw_clock = *AICA_HW_CLOCK;
  uint32_t prev_hw_clock = *AICA_PREV_HW_CLOCK;
  uint32_t virtual_clock = *AICA_VIRTUAL_CLOCK;
  uint32_t cmd_count = *AICA_CMD_COUNT;

  printf("[AFX] driver state @%s %p\n", label ? label : "", (const void *)driver_state);
  printf("[AFX]  stack_canary = 0x%08X\n", (unsigned)driver_state->stack_canary);
  printf("[AFX]  flow_count_active = %u\n", (unsigned)driver_state->flow_count_active);
  printf("[AFX]  AICA_CMD_COUNT = %u\n", (unsigned)cmd_count);
  printf("[AFX]  arm_status = %u\n", (unsigned)driver_state->arm_status);
  printf("[AFX]  HW clock = %u, prev = %u, virtual = %u\n", hw_clock, prev_hw_clock, virtual_clock);

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
      const afx_section_entry_t *flow_sect = find_afx_section(hdr, AFX_SECT_FLOW);
      if (hdr && flow_sect) {
        required_channels = hdr->required_channels;
        total_ticks = hdr->total_ticks;
        flow_section_size = flow_sect->size;
        if (total_ticks > 0) {
          tick_progress = (uint32_t)((uint64_t)f->next_event_tick * 100 / total_ticks);
        }
        if (flow_section_size > 0) {
          flow_progress = (uint32_t)((uint64_t)f->flow_offset * 100 / flow_section_size);
        }
      }
    }

    char channel_map_str[256];
    channel_map_str[0] = '\0';
    size_t cm_len = 0;
    cm_len += snprintf(channel_map_str + cm_len,
                       sizeof(channel_map_str) - cm_len,
                       "[");
    for (uint32_t ch = 0; ch < required_channels; ch++) {
      uint32_t hw = ((uint8_t*)f->channel_map)[ch];
      if (hw != AFX_FLOW_CHANNEL_MAP_INVALID) {
        cm_len += snprintf(channel_map_str + cm_len,
                           sizeof(channel_map_str) - cm_len,
                           "%s%u->%u", cm_len > 1 ? "," : "", (unsigned)ch,
                           (unsigned)hw);
        if (cm_len >= sizeof(channel_map_str) - 1) break;
      }
    }
    if (cm_len < sizeof(channel_map_str) - 1)
      snprintf(channel_map_str + cm_len,
               sizeof(channel_map_str) - cm_len,
               "]");
    else
      channel_map_str[sizeof(channel_map_str) - 1] = '\0';

    printf("[AFX]  flow_slot %02u: status=%s (raw=%u), afx_base=0x%08X, flow_ptr=0x%08X, flow_offset=%u/%u (%u%%), tick_adjust=%u, next_event_tick=%u/%u (%u%%), tl_lut=0x%08X, sample_addr_mode=%u, map=%s\n",
           (unsigned)i,
           status,
           (unsigned)f->status,
           (unsigned)f->afx_base,
           (unsigned)f->flow_ptr,
           (unsigned)f->flow_offset,
           (unsigned)flow_section_size,
           (unsigned)flow_progress,
           (unsigned)f->tick_adjust,
           (unsigned)f->next_event_tick,
           (unsigned)total_ticks,
           (unsigned)tick_progress,
           (unsigned)f->tl_scale_lut_ptr,
           (unsigned)f->sample_addr_mode,
            channel_map_str
          );
  }
}

bool afx_flow_deactivate(uint8_t flow_slot) {
  if (flow_slot >= AFX_FLOW_POOL_CAPACITY)
    return false;
  afx_state_release_flow_slot(flow_slot);
  return true;
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
 * @note Loaded flows can be activated multiple times concurrently, but each
 * activation must be paired with a call to afx_flow_deactivate() to properly
 * release resources.
 *
 * @note Activating a flow does not automatically start playback; call
 * afx_flow_play() with the returned slot to start playback.
 * */
uint8_t afx_flow_activate(uint32_t flow_spu_addr) {

  if (flow_spu_addr == 0)
    return 0xFFu;
  if (drv_state_ptr->flow_count_active >= AFX_FLOW_POOL_CAPACITY) {
    return 0xFFu;
  }
  const afx_header_t *hdr =
      (const afx_header_t *)(uintptr_t)(SPU_RAM_BASE_SH4 + flow_spu_addr);

  if (!hdr || hdr->magic != AICAF_MAGIC || hdr->version != AICAF_VERSION)
    return 0xFFu;

  const bool uses_external_samples =
      (hdr->flags & AFX_FILE_FLAG_EXTERNAL_SAMPLE_ADDRS) != 0u;
  const afx_section_entry_t *flow_sect = find_afx_section(hdr, AFX_SECT_FLOW);
  if (!flow_sect)
    return 0xFFu;
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

  afx_flow_state_t flow_template;
  memset(&flow_template, 0, sizeof(flow_template));
  flow_template.afx_base = flow_spu_addr;
  flow_template.flow_ptr = flow_spu_addr + flow_sect->offset;
  flow_template.flow_offset = 0;
  flow_template.next_event_tick = 0;
  flow_template.sample_addr_mode = uses_external_samples;
  flow_template.status = AFX_FLOW_STOPPED;


  uint8_t channel_map[64];

  /* map virtual channels */
  uint32_t flow_chn = 0;
  for (uint32_t hw = 0; hw < 64u; hw++) {
    if (channel_mask & (1ULL << hw)) {
      channel_map[flow_chn] = (uint8_t)hw;
      flow_chn++;
      if (flow_chn >= required_channels)
        break;
    }
  }

  drv_state_ptr->flow_states[flow_slot] = flow_template;
  return flow_slot;
}

bool afx_flow_set_tl_scale_lut(uint8_t flow_slot, uint32_t lut_spu_addr) {
//   uint32_t flow_addr = drv_state_ptr->flow_states[flow_slot].afx_base +
//                        offsetof(afx_flow_state_t, tl_scale_lut_ptr);
//   if (flow_addr == 0)
//     return false;

//   uint32_t field_addr =
//       flow_addr + (uint32_t)offsetof(afx_flow_state_t, tl_scale_lut_ptr);
//   return afx_mem_write(field_addr, &lut_spu_addr, sizeof(lut_spu_addr));
    return true;
}

void afx_flow_play(uint8_t flow_slot) {
  if (flow_slot >= AFX_FLOW_POOL_CAPACITY)
    return;

  volatile afx_flow_state_t *flow_state = &drv_state_ptr->flow_states[flow_slot];
  if (flow_state->afx_base != 0) {
    apply_song_dsp_sections_host(flow_state->afx_base);
  }

  // If this is a fresh play (or after stop), reset playback pointers.
  if (flow_state->status != AFX_FLOW_PAUSED) {
    flow_state->flow_offset = 0;
    flow_state->next_event_tick = 0;
    flow_state->tick_adjust = *AICA_VIRTUAL_CLOCK;
  }

  flow_state->status = AFX_FLOW_PLAYING;
}

void afx_flow_pause(uint8_t flow_slot) {
  drv_state_ptr->flow_states[flow_slot].status = AFX_FLOW_PAUSED;
}

void afx_flow_resume(uint8_t flow_slot) {
  if (flow_slot >= AFX_FLOW_POOL_CAPACITY)
    return;

  volatile afx_flow_state_t *flow_state = &drv_state_ptr->flow_states[flow_slot];
  if (flow_state->status != AFX_FLOW_PAUSED)
    return;

  // Rebase tick_adjust so playback resumes from paused next_event_tick at current wall-clock time.
  uint32_t paused_next_tick = flow_state->next_event_tick;
  flow_state->tick_adjust = *AICA_VIRTUAL_CLOCK - paused_next_tick;
  flow_state->status = AFX_FLOW_PLAYING;
}

void afx_flow_stop(uint8_t flow_slot) {
  drv_state_ptr->flow_states[flow_slot].flow_offset = 0;
  drv_state_ptr->flow_states[flow_slot].tick_adjust = 0;
  drv_state_ptr->flow_states[flow_slot].next_event_tick = 0;
  drv_state_ptr->flow_states[flow_slot].status = AFX_FLOW_STOPPED;
}

void afx_flow_seek(uint8_t flow_slot, uint32_t tick_ms) {
  if (flow_slot >= AFX_FLOW_POOL_CAPACITY)
    return;

  volatile afx_flow_state_t *flow = drv_state_ptr->flow_states + flow_slot;
  const afx_header_t *hdr =
      (const afx_header_t *)(uintptr_t)(SPU_RAM_BASE_SH4 + flow->afx_base);
  const volatile afx_section_entry_t *flow_sect =
      find_afx_section(hdr, AFX_SECT_FLOW);
  if (!flow_sect)
    return;

  uint32_t flow_offset =
      afx_cmd_lower_bound_by_offset((const uint8_t *)flow->flow_ptr,
                                    flow_sect->size, flow_sect->count, tick_ms);

  flow->flow_offset = flow_offset;
  flow->next_event_tick = tick_ms;
  flow->tick_adjust = *AICA_VIRTUAL_CLOCK - tick_ms;
  flow->status = AFX_FLOW_STOPPED;
}
