#include <afx/bin/driver_blob.h>
#include <afx/host.h>
#include <afx/memory.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

aica_state_t g_aica_state = {
    .dynamic_base = 0,
    .dynamic_cursor = 0,
};

uint64_t g_host_available_channels = 0xFFFFFFFFFFFFFFFFULL;

#define AFX_FREE_BLOCK_CAPACITY 128u
#define AFX_SAMPLE_HANDLE_CAPACITY 64u
#define AFX_AFX_ALLOC_CAPACITY 128u

typedef struct {
  uint32_t addr;
  uint32_t size;
} afx_free_block_t;

typedef struct {
  bool in_use;
  uint16_t generation;
  afx_sample_info_t info;
} afx_sample_slot_t;

static afx_free_block_t g_free_blocks[AFX_FREE_BLOCK_CAPACITY];
static uint32_t g_free_block_count = 0;
static afx_sample_slot_t g_sample_slots[AFX_SAMPLE_HANDLE_CAPACITY];
static uint64_t g_flow_pool_alloc_mask =
    0ull; /* One bit per flow slot, 64 max. */
static afx_free_block_t g_afx_allocs[AFX_AFX_ALLOC_CAPACITY];
static uint32_t g_afx_alloc_count = 0;

static inline bool range_fits_dynamic(uint32_t addr, uint32_t size) {
  if (size == 0)
    return false;
  if (addr >= AFX_DRIVER_STATE_ADDR)
    return false;
  if (addr + size < addr)
    return false;
  return (addr + size) <= AFX_DRIVER_STATE_ADDR;
}

static inline uint32_t align_up_u32(uint32_t value, uint32_t align) {
  if (align == 0)
    return value;
  uint32_t mask = align - 1u;
  return (value + mask) & ~mask;
}

static inline bool is_dynamic_range_valid(uint32_t addr, uint32_t size) {
  if (size == 0)
    return false;
  if (addr < g_aica_state.dynamic_base)
    return false;
  if (addr >= AFX_DRIVER_STATE_ADDR)
    return false;
  if (addr + size < addr)
    return false;
  return (addr + size) <= AFX_DRIVER_STATE_ADDR;
}

static void reset_sample_slots(void) {
  memset(g_sample_slots, 0, sizeof(g_sample_slots));
  memset(g_afx_allocs, 0, sizeof(g_afx_allocs));
  g_afx_alloc_count = 0;
}

static void reset_free_list(uint32_t dynamic_base) {
  g_free_block_count = 0;
  if (dynamic_base >= AFX_DRIVER_STATE_ADDR)
    return;

  g_free_blocks[0].addr = dynamic_base;
  g_free_blocks[0].size = AFX_DRIVER_STATE_ADDR - dynamic_base;
  g_free_block_count = 1;
}

static bool insert_free_block_sorted(uint32_t addr, uint32_t size) {
  if (!is_dynamic_range_valid(addr, size))
    return false;

  uint32_t idx = 0;
  while (idx < g_free_block_count && g_free_blocks[idx].addr < addr) {
    idx++;
  }

  if (idx > 0) {
    uint32_t prev_end =
        g_free_blocks[idx - 1].addr + g_free_blocks[idx - 1].size;
    if (prev_end > addr)
      return false;
  }

  if (idx < g_free_block_count) {
    uint32_t end = addr + size;
    if (end > g_free_blocks[idx].addr)
      return false;
  }

  if (g_free_block_count >= AFX_FREE_BLOCK_CAPACITY)
    return false;

  for (uint32_t j = g_free_block_count; j > idx; j--) {
    g_free_blocks[j] = g_free_blocks[j - 1u];
  }

  g_free_blocks[idx].addr = addr;
  g_free_blocks[idx].size = size;
  g_free_block_count++;

  for (uint32_t i = 0; i + 1u < g_free_block_count;) {
    uint32_t end = g_free_blocks[i].addr + g_free_blocks[i].size;
    if (end == g_free_blocks[i + 1u].addr) {
      g_free_blocks[i].size += g_free_blocks[i + 1u].size;
      for (uint32_t j = i + 1u; j + 1u < g_free_block_count; j++) {
        g_free_blocks[j] = g_free_blocks[j + 1u];
      }
      g_free_block_count--;
      continue;
    }
    i++;
  }

  return true;
}

static inline uint32_t make_sample_handle(uint32_t slot_idx) {
  return ((uint32_t)g_sample_slots[slot_idx].generation << 16) |
         (slot_idx + 1u);
}

static bool resolve_sample_handle(uint32_t handle, uint32_t *slot_idx_out) {
  if (handle == 0 || !slot_idx_out)
    return false;

  uint32_t raw_idx = handle & 0xFFFFu;
  uint32_t generation = handle >> 16;
  if (raw_idx == 0)
    return false;

  uint32_t idx = raw_idx - 1u;
  if (idx >= AFX_SAMPLE_HANDLE_CAPACITY)
    return false;

  afx_sample_slot_t *slot = &g_sample_slots[idx];
  if (!slot->in_use)
    return false;
  if ((uint32_t)slot->generation != generation)
    return false;

  *slot_idx_out = idx;
  return true;
}

void afx_mem_reset(uint32_t dynamic_base) {
  g_aica_state.dynamic_base = dynamic_base;
  g_aica_state.dynamic_cursor = dynamic_base;
  reset_free_list(dynamic_base);
  reset_sample_slots();
  g_flow_pool_alloc_mask = 0ull;
  g_host_available_channels = 0xFFFFFFFFFFFFFFFFULL;
}

uint32_t afx_mem_alloc(uint32_t size, uint32_t align) {
  if (size == 0)
    return 0;

  uint32_t use_align = (align == 0) ? 32u : align;
  for (uint32_t i = 0; i < g_free_block_count; i++) {
    afx_free_block_t block = g_free_blocks[i];
    uint32_t start = align_up_u32(block.addr, use_align);
    if (start < block.addr)
      continue;

    uint32_t pad = start - block.addr;
    if (pad > block.size)
      continue;
    if (size > (block.size - pad))
      continue;

    uint32_t used = pad + size;
    uint32_t remain = block.size - used;
    uint32_t end = start + size;
    if (end < start)
      return 0;

    if (pad == 0 && remain == 0) {
      for (uint32_t j = i; j + 1u < g_free_block_count; j++) {
        g_free_blocks[j] = g_free_blocks[j + 1u];
      }
      g_free_block_count--;
    } else if (pad == 0) {
      g_free_blocks[i].addr = end;
      g_free_blocks[i].size = remain;
    } else if (remain == 0) {
      g_free_blocks[i].size = pad;
    } else {
      if (g_free_block_count >= AFX_FREE_BLOCK_CAPACITY)
        return 0;
      for (uint32_t j = g_free_block_count; j > (i + 1u); j--) {
        g_free_blocks[j] = g_free_blocks[j - 1u];
      }
      g_free_blocks[i].size = pad;
      g_free_blocks[i + 1u].addr = end;
      g_free_blocks[i + 1u].size = remain;
      g_free_block_count++;
    }

    if (end > g_aica_state.dynamic_cursor) {
      g_aica_state.dynamic_cursor = end;
    }
    return start;
  }

  return 0;
}

bool afx_mem_free(uint32_t spu_addr, uint32_t size) {
  return insert_free_block_sorted(spu_addr, size);
}

bool afx_mem_write(uint32_t spu_addr, const void *src, uint32_t size) {
  if (!src || size == 0)
    return false;
  if (!range_fits_dynamic(spu_addr, size))
    return false;
  memcpy((void *)(uintptr_t)(SPU_RAM_BASE_SH4 + spu_addr), src, size);
  return true;
}

uint64_t afx_channels_allocate(uint32_t num_channels, uint8_t flow_slot) {
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

void afx_channels_release(uint8_t flow_slot) {

  uint32_t flow_addr = flow_slot_to_addr(flow_slot);
  if (flow_addr == 0)
    return;

  volatile afx_flow_state_t *flow =
      (volatile afx_flow_state_t *)(uintptr_t)(SPU_RAM_BASE_SH4 + flow_addr);
  uint64_t mask = flow->assigned_channels;
  if (mask == 0)
    return;

  g_host_available_channels |= mask;
  uint64_t zero = 0;
  (void)afx_mem_write(flow_addr + offsetof(afx_flow_state_t, assigned_channels),
                      &zero, sizeof(zero));
}

bool afx_sample_free(uint32_t sample_handle) {
    uint32_t slot_idx = 0;
    if (!resolve_sample_handle(sample_handle, &slot_idx)) return false;

    afx_sample_slot_t *slot = &g_sample_slots[slot_idx];
    if (!afx_mem_free(slot->info.spu_addr, slot->info.length)) return false;

    slot->in_use = false;
    memset(&slot->info, 0, sizeof(slot->info));
    slot->generation++;
    if (slot->generation == 0) slot->generation = 1;
    return true;
}

uint32_t afx_sample_get_spu_addr(uint32_t sample_handle) {
    uint32_t slot_idx = 0;
    if (!resolve_sample_handle(sample_handle, &slot_idx)) return 0;
    return g_sample_slots[slot_idx].info.spu_addr;
}

bool afx_sample_get_info(uint32_t sample_handle, afx_sample_info_t *out_info) {
    uint32_t slot_idx = 0;
    if (!out_info) return false;
    if (!resolve_sample_handle(sample_handle, &slot_idx)) return false;

    *out_info = g_sample_slots[slot_idx].info;
    return true;
}
