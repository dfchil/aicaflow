#ifndef AFX_HOST_H
#define AFX_HOST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <afx/common.h>
#include <afx/driver.h>

/* SPU RAM is mapped at this fixed address on SH4 */
#define SPU_RAM_BASE_SH4 0xA0800000u


typedef struct {
  uint32_t dynamic_base;
  uint32_t dynamic_cursor;
} aica_state_t;

typedef struct {
  uint32_t spu_addr;
  uint32_t length;
  uint32_t rate;
  uint8_t bitsize;
  uint8_t channels;
} afx_sample_info_t;

void aica_shutdown(void);

uint32_t afx_mem_alloc(uint32_t size, uint32_t align);
bool afx_mem_free(uint32_t spu_addr, uint32_t size);
bool afx_mem_write(uint32_t spu_addr, const void *src, uint32_t size);

uint32_t afx_upload_afx(const void *afx_data, uint32_t afx_size);
bool afx_free_afx(uint32_t spu_addr);

uint32_t afx_upload_tl_scale_lut(const uint8_t lut[256]);
uint32_t afx_create_tl_scale_lut(uint8_t volume);

/* Sample upload API (returns opaque handle, 0 on failure). */
uint32_t afx_sample_upload(const char *buf, size_t len,
                           uint32_t rate, uint8_t bitsize, uint8_t channels);
bool afx_sample_free(uint32_t sample_handle);
uint32_t afx_sample_get_spu_addr(uint32_t sample_handle);
bool afx_sample_get_info(uint32_t sample_handle, afx_sample_info_t *out_info);

bool afx_init(void);

/* Flow-based playback API */
uint8_t afx_flow_init(uint32_t song_spu_addr); /* 0xFF on failure */
bool afx_flow_set_tl_scale_lut(uint8_t flow_slot, uint32_t lut_spu_addr);
void afx_flow_play(uint8_t flow_slot);
void afx_flow_pause(uint8_t flow_slot);
void afx_flow_resume(uint8_t flow_slot);
void afx_flow_stop(uint8_t flow_slot);
void afx_flow_seek(uint8_t flow_slot, uint32_t tick_ms);
bool afx_flow_poll_completed(uint8_t *flow_slot);


// /* Flow slot helpers */
// uint32_t flow_slot_to_addr(uint8_t slot);
// uint8_t flow_addr_to_slot(uint32_t flow_addr);


static inline uint32_t afx_cmd_lower_bound_by_offset(const uint8_t *stream,
                                                     uint32_t size,
                                                     uint32_t count,
                                                     uint32_t target_tick) {
  /* Variable-length streams cannot be binary searched by offset directly.
     We skip ahead based on the command size. */
  uint32_t curr_ptr = 0;
  uint32_t curr_idx = 0;
  while (curr_idx < count && curr_ptr < size) {
    const afx_cmd_t *cmd = (const afx_cmd_t *)(stream + curr_ptr);
    if (cmd->timestamp >= target_tick)
      return curr_ptr;

    uint32_t cmd_num_vals = cmd->length;
    uint32_t cmd_size = 6 + (cmd_num_vals << 1);
    // Commands are 4-byte aligned in the stream
    cmd_size = (cmd_size + 3) & ~3;

    curr_ptr += cmd_size;
    curr_idx++;
  }
  return curr_ptr;
}

/**
 * Set the channel map entry for the given slot index. Each entry is 5 bits,
 * packed into a byte array. Setting a value will be masked to the valid bit
 * range. Does nothing if the slot index is out of bounds.
 */
static inline void afx_channel_map_set(volatile afx_flow_state_t *flow,
                                       uint32_t slot, uint32_t value) {
  if (!flow || slot >= AFX_FLOW_CHANNEL_MAP_ENTRIES)
    return;

  uint32_t encoded = value & ((1u << AFX_FLOW_CHANNEL_MAP_BITS_PER_ENTRY) - 1u);
  uint32_t bit_index = (slot << 2) + slot;
  uint32_t byte_index = bit_index >> 3;
  uint32_t bit_offset = bit_index & 7u;

  uint64_t raw = 0;
  raw |= (uint64_t)flow->channel_map[byte_index];
  raw |= (uint64_t)flow->channel_map[byte_index + 1] << 8;

  uint64_t clear_mask =
      ~(((uint64_t)((1u << AFX_FLOW_CHANNEL_MAP_BITS_PER_ENTRY) - 1u))
        << bit_offset);
  raw = (raw & clear_mask) | ((uint64_t)encoded << bit_offset);
  flow->channel_map[byte_index] = (uint8_t)(raw & 0xFFu);
  flow->channel_map[byte_index + 1] = (uint8_t)((raw >> 8) & 0xFFu);
}


#endif /* AFX_HOST_H */
