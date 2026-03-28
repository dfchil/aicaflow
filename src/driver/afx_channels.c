#include <afx/channels.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

static uint64_t g_host_available_channels = 0xFFFFFFFFFFFFFFFFULL;
static uint64_t g_host_channel_maps[5] = {0};

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

uint32_t afx_reserve_channel_mapping(uint8_t num_channels) {
  if (num_channels == 0 || num_channels > 64)
    return 0;
  
  int arena = 0;
  if (num_channels > 32) {
    arena = 4;
  } else if (num_channels > 16) {
    arena = 3;
  } else if (num_channels > 8) {
    arena = 2;
  } else if (num_channels > 4) {
    arena = 1;
  } else {
    arena = 0;
  }
  uint32_t offset = 0;
  // find free continuous offset in the channel map for the given number of channels
  uint64_t used_map = g_host_channel_maps[arena];
  uint64_t mask = (1ULL << num_channels) - 1;
  for (; offset + num_channels <= 64u; offset++) {
    if ((used_map & (mask << offset)) == 0) {
      g_host_channel_maps[arena] |= (mask << offset);
      break;
    }
  }
  return (arena << 6) | offset;
}


void afx_channels_release(uint64_t channel_mask) {
  g_host_available_channels |= channel_mask;
}








