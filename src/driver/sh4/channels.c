#include <afx/channels.h>
#include <afx/driver.h>
#include <afx/host.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

uint64_t g_host_available_channels = 0xFFFFFFFFFFFFFFFFULL;
static uint64_t g_host_channel_maps[5] = {0};

#define drv_state_ptr                                                          \
  ((volatile afx_driver_state_t *)(SPU_RAM_BASE_SH4 + AFX_DRIVER_STATE_ADDR))


uint64_t afx_channels_allocate(uint8_t num_channels) {
  if (num_channels == 0 || num_channels > 64)
    return 0;

  uint64_t allocated = 0;
  for (uint32_t bit = 0; bit < 64u && num_channels > 0; bit++) {
    uint64_t b = (1ULL << bit);
    if (g_host_available_channels & b) {
      g_host_available_channels &= ~b;
      allocated |= b;
      num_channels--;
    }
  }
  if (num_channels != 0) {
    g_host_available_channels |= allocated;
    return 0;
  }
  return allocated;
}

uint32_t afx_channel_setup_mapping(uint8_t num_channels, uint64_t channel_mask) {
  if (num_channels == 0 || num_channels > 64)
    return 0;
  
  int arena = -1;
  if (num_channels <= 4) {
    arena = 0;
  } else if (num_channels <= 8) {
    arena = 1;
  } else if (num_channels <= 16) {
    arena = 2;
  } else if (num_channels <= 32) {
    arena = 3;
  } else {
    arena = 4;
  }

  uint32_t offset = 0;
  // find free continuous offset in the channel map for the given number of channels
  uint64_t used_map = g_host_channel_maps[arena];
  uint64_t mask = (~0ULL >> (64 - num_channels));
  for (; offset + num_channels <= 64u; offset++) {
    if ((used_map & (mask << offset)) == 0) {
      g_host_channel_maps[arena] |= (mask << offset);
      break;
    }
  }
  /* map virtual channels */
  uint32_t flow_chn = 0;
  for (uint32_t hw = 0; hw < 64u; hw++) {
    if (channel_mask & (1ULL << hw)) {
      drv_state_ptr->chan_arenas[arena][offset + flow_chn] = (uint8_t)hw;
      flow_chn++;
      if (flow_chn >= num_channels)
        break;
    }
  }
  return AFX_DRIVER_STATE_ADDR + offsetof(afx_driver_state_t, chan_arenas[arena][offset]);
}


void afx_channels_release(uint64_t channel_mask) {
  g_host_available_channels |= channel_mask;
}

void afx_channel_release_mapping(uint8_t num_channels, uint32_t channel_map_addr) {
  if (num_channels == 0 || num_channels > 64)
    return;

  uint32_t base_arenas = AFX_DRIVER_STATE_ADDR + offsetof(afx_driver_state_t, chan_arenas[0][0]);
  if (channel_map_addr < base_arenas)
    return;

  uint32_t map_offset = channel_map_addr - base_arenas;
  int arena = map_offset / 64;
  uint32_t offset_in_arena = map_offset % 64;

  if (arena > 4)
    return;

  uint64_t mask = (~0ULL >> (64 - num_channels));
  g_host_channel_maps[arena] &= ~(mask << offset_in_arena);
}

void afx_channels_reset() {
  g_host_available_channels = 0xFFFFFFFFFFFFFFFFULL;
  for (int i = 0; i < 5; i++) {
    g_host_channel_maps[i] = 0;
    for (int j = 0; j < 64; j++) {
      drv_state_ptr->chan_arenas[i][j] = 0xFFu;
    }
  }
}





