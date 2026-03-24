#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <limits.h>
#include <string.h>
#include <afx/host.h>
#include <afx/bin/driver_blob.h>
#include <kos.h> 


#ifdef KOS_HEADERS
#include <arch/timer.h>
#include <dc/sound/sound.h>
#include <dc/spu.h>
#endif

#define SPU_RAM_BASE_SH4 0xA0800000

#define AFX_DYNAMIC_DEFAULT_BASE 0x00002000u
#define AFX_FIRMWARE_DEFAULT_ADDR 0x00000000u
#define AICA_DSP_COEF_ADDR 0x00801000u
#define AICA_DSP_MPRO_ADDR 0x00803000u

#define G2_WAIT_REG (*(volatile uint32_t *)0xa05f68a0)
#define AICA_ARM_EN_REG (*(volatile uint32_t *)0xa0702c00)

static aica_state_t g_aica_state = {
    .dynamic_base = AFX_DYNAMIC_DEFAULT_BASE,
    .dynamic_cursor = AFX_DYNAMIC_DEFAULT_BASE,
};

static uint64_t g_host_available_channels = 0xFFFFFFFFFFFFFFFFULL;

#define AFX_FREE_BLOCK_CAPACITY 128u
#define AFX_SAMPLE_HANDLE_CAPACITY 256u
#define AFX_AFX_ALLOC_CAPACITY 32u

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
static afx_free_block_t g_afx_allocs[AFX_AFX_ALLOC_CAPACITY];
static uint32_t g_afx_alloc_count = 0;

static volatile afx_ipc_cmd_t *get_ipc_queue(void) {
    return (volatile afx_ipc_cmd_t *)(SPU_RAM_BASE_SH4 + AFX_IPC_CMD_QUEUE_ADDR);
}

static volatile afx_ipc_control_t *get_ipc_control(void) {
    return (volatile afx_ipc_control_t *)(SPU_RAM_BASE_SH4 + AFX_IPC_CONTROL_ADDR);
}

static uint64_t allocate_channels_host(uint32_t num_channels) {
    if (num_channels == 0 || num_channels > 64) return 0;

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

static void release_channels_host(uint64_t mask) {
    g_host_available_channels |= mask;
}

static const afx_section_entry_t *find_section_host(const afx_header_t *hdr,
                                                    uint32_t section_id) {
    if (!hdr) return NULL;
    const afx_section_entry_t *sections = (const afx_section_entry_t *)(hdr + 1);
    for (uint32_t i = 0; i < hdr->section_count; i++) {
        if (sections[i].id == section_id) return &sections[i];
    }
    return NULL;
}

static void build_flow_channel_map_host(afx_flow_state_t *flow, uint32_t required_channels) {
    for (uint32_t i = 0; i < 64u; i++) flow->channel_map[i] = 0xFFu;

    uint64_t mask = flow->assigned_channels;
    uint32_t logical = 0;
    for (uint32_t hw = 0; hw < 64u && logical < required_channels; hw++) {
        if (mask & (1ULL << hw)) {
            flow->channel_map[logical++] = (uint8_t)hw;
        }
    }
}

static void apply_song_dsp_sections_host(uint32_t song_spu_addr) {
    const afx_header_t *hdr = (const afx_header_t *)(uintptr_t)(SPU_RAM_BASE_SH4 + song_spu_addr);
    if (!hdr || hdr->magic != AICAF_MAGIC || hdr->version != AICAF_VERSION) return;

    const afx_section_entry_t *dspc_sect = find_section_host(hdr, AFX_SECT_DSPC);
    const afx_section_entry_t *dspm_sect = find_section_host(hdr, AFX_SECT_DSPM);

    if (dspc_sect && dspc_sect->size > 0) {
        const uint32_t *src =
            (const uint32_t *)(uintptr_t)(SPU_RAM_BASE_SH4 + song_spu_addr + dspc_sect->offset);
        volatile uint32_t *dst = (volatile uint32_t *)(uintptr_t)(SPU_RAM_BASE_SH4 + AICA_DSP_COEF_ADDR);
        uint32_t words = dspc_sect->size >> 2;
        for (uint32_t w = 0; w < words; w++) dst[w] = src[w];
    }

    if (dspm_sect && dspm_sect->size > 0) {
        const uint32_t *src =
            (const uint32_t *)(uintptr_t)(SPU_RAM_BASE_SH4 + song_spu_addr + dspm_sect->offset);
        volatile uint32_t *dst = (volatile uint32_t *)(uintptr_t)(SPU_RAM_BASE_SH4 + AICA_DSP_MPRO_ADDR);
        uint32_t words = dspm_sect->size >> 2;
        for (uint32_t w = 0; w < words; w++) dst[w] = src[w];
    }
}

static bool afx_ipc_push(uint32_t cmd, uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    volatile afx_ipc_cmd_t *queue = get_ipc_queue();
    volatile afx_ipc_control_t *ctrl = get_ipc_control();

    for (uint32_t tries = 0; tries < 100000u; tries++) {
        uint32_t head = ctrl->q_head;
        uint32_t tail = ctrl->q_tail;
        uint32_t next = (head + 1u) & (AFX_IPC_QUEUE_CAPACITY - 1u);
        if (next == tail) continue;

        queue[head].cmd = cmd;
        queue[head].arg0 = arg0;
        queue[head].arg1 = arg1;
        queue[head].arg2 = arg2;
        ctrl->q_head = next;
        return true;
    }

    return false;
}

static inline bool range_fits_dynamic(uint32_t addr, uint32_t size) {
    if (size == 0) return false;
    if (addr >= AFX_DRIVER_STATE_ADDR) return false;
    if (addr + size < addr) return false;
    return (addr + size) <= AFX_DRIVER_STATE_ADDR;
}

static inline uint32_t align_up_u32(uint32_t value, uint32_t align) {
    if (align == 0) return value;
    uint32_t mask = align - 1u;
    return (value + mask) & ~mask;
}

static bool is_dynamic_range_valid(uint32_t addr, uint32_t size) {
    if (size == 0) return false;
    if (addr < g_aica_state.dynamic_base) return false;
    if (addr >= AFX_DRIVER_STATE_ADDR) return false;
    if (addr + size < addr) return false;
    return (addr + size) <= AFX_DRIVER_STATE_ADDR;
}

static void reset_sample_slots(void) {
    memset(g_sample_slots, 0, sizeof(g_sample_slots));
    memset(g_afx_allocs, 0, sizeof(g_afx_allocs));
    g_afx_alloc_count = 0;
}

static void reset_free_list(uint32_t dynamic_base) {
    g_free_block_count = 0;
    if (dynamic_base >= AFX_DRIVER_STATE_ADDR) return;

    g_free_blocks[0].addr = dynamic_base;
    g_free_blocks[0].size = AFX_DRIVER_STATE_ADDR - dynamic_base;
    g_free_block_count = 1;
}

static bool insert_free_block_sorted(uint32_t addr, uint32_t size) {
    if (!is_dynamic_range_valid(addr, size)) return false;

    uint32_t idx = 0;
    while (idx < g_free_block_count && g_free_blocks[idx].addr < addr) {
        idx++;
    }

    if (idx > 0) {
        uint32_t prev_end = g_free_blocks[idx - 1].addr + g_free_blocks[idx - 1].size;
        if (prev_end > addr) return false;
    }
    if (idx < g_free_block_count) {
        uint32_t end = addr + size;
        if (end > g_free_blocks[idx].addr) return false;
    }

    if (g_free_block_count >= AFX_FREE_BLOCK_CAPACITY) return false;

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

static uint32_t make_sample_handle(uint32_t slot_idx) {
    return ((uint32_t)g_sample_slots[slot_idx].generation << 16) | (slot_idx + 1u);
}

static bool resolve_sample_handle(uint32_t handle, uint32_t *slot_idx_out) {
    if (handle == 0 || !slot_idx_out) return false;

    uint32_t raw_idx = handle & 0xFFFFu;
    uint32_t generation = handle >> 16;
    if (raw_idx == 0) return false;

    uint32_t idx = raw_idx - 1u;
    if (idx >= AFX_SAMPLE_HANDLE_CAPACITY) return false;

    afx_sample_slot_t *slot = &g_sample_slots[idx];
    if (!slot->in_use) return false;
    if ((uint32_t)slot->generation != generation) return false;

    *slot_idx_out = idx;
    return true;
}

void afx_mem_reset(uint32_t dynamic_base) {
    if (dynamic_base == 0) dynamic_base = AFX_DYNAMIC_DEFAULT_BASE;
    dynamic_base = align_up_u32(dynamic_base, 32);
    if (dynamic_base >= AFX_DRIVER_STATE_ADDR) dynamic_base = AFX_DYNAMIC_DEFAULT_BASE;
    g_aica_state.dynamic_base = dynamic_base;
    g_aica_state.dynamic_cursor = dynamic_base;
    reset_free_list(dynamic_base);
    reset_sample_slots();
    g_host_available_channels = 0xFFFFFFFFFFFFFFFFULL;
}

uint32_t afx_mem_alloc(uint32_t size, uint32_t align) {
    if (size == 0) return 0;

    uint32_t use_align = (align == 0) ? 32u : align;
    for (uint32_t i = 0; i < g_free_block_count; i++) {
        afx_free_block_t block = g_free_blocks[i];
        uint32_t start = align_up_u32(block.addr, use_align);
        if (start < block.addr) continue;

        uint32_t pad = start - block.addr;
        if (pad > block.size) continue;
        if (size > (block.size - pad)) continue;

        uint32_t used = pad + size;
        uint32_t remain = block.size - used;
        uint32_t end = start + size;
        if (end < start) return 0;

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
            if (g_free_block_count >= AFX_FREE_BLOCK_CAPACITY) return 0;
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
    if (!src || size == 0) return false;
    if (!range_fits_dynamic(spu_addr, size)) return false;
    memcpy((void *)(uintptr_t)(SPU_RAM_BASE_SH4 + spu_addr), src, size);
    return true;
}

uint32_t afx_upload_afx(const void *afx_data, uint32_t afx_size) {
    if (!afx_data || afx_size == 0) return 0;
    if (g_afx_alloc_count >= AFX_AFX_ALLOC_CAPACITY) return 0;
    uint32_t spu_addr = afx_mem_alloc(afx_size, 32);
    if (spu_addr == 0) return 0;
    if (!afx_mem_write(spu_addr, afx_data, afx_size)) {
        (void)afx_mem_free(spu_addr, afx_size);
        return 0;
    }
    g_afx_allocs[g_afx_alloc_count].addr = spu_addr;
    g_afx_allocs[g_afx_alloc_count].size = afx_size;
    g_afx_alloc_count++;
    return spu_addr;
}

bool afx_free_afx(uint32_t spu_addr) {
    for (uint32_t i = 0; i < g_afx_alloc_count; i++) {
        if (g_afx_allocs[i].addr == spu_addr) {
            uint32_t size = g_afx_allocs[i].size;
            for (uint32_t j = i; j + 1u < g_afx_alloc_count; j++) {
                g_afx_allocs[j] = g_afx_allocs[j + 1u];
            }
            g_afx_alloc_count--;
            return afx_mem_free(spu_addr, size);
        }
    }
    return false;
}

uint32_t afx_upload_tl_scale_lut(const uint8_t lut[256]) {
    if (!lut) return 0;

    uint32_t spu_addr = afx_mem_alloc(256u, 32u);
    if (spu_addr == 0) return 0;
    if (!afx_mem_write(spu_addr, lut, 256u)) return 0;
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

uint32_t afx_sample_upload(const char *buf, size_t len,
                           uint32_t rate, uint8_t bitsize, uint8_t channels) {
    if (!buf || len == 0 || len > UINT32_MAX) return 0;
    if (bitsize == 0 || channels == 0) return 0;

    uint32_t slot_idx = AFX_SAMPLE_HANDLE_CAPACITY;
    for (uint32_t i = 0; i < AFX_SAMPLE_HANDLE_CAPACITY; i++) {
        if (!g_sample_slots[i].in_use) {
            slot_idx = i;
            break;
        }
    }
    if (slot_idx == AFX_SAMPLE_HANDLE_CAPACITY) return 0;

    uint32_t sample_size = (uint32_t)len;
    uint32_t spu_addr = afx_mem_alloc(sample_size, 32u);
    if (spu_addr == 0) return 0;
    if (!afx_mem_write(spu_addr, buf, sample_size)) {
        (void)afx_mem_free(spu_addr, sample_size);
        return 0;
    }

    afx_sample_slot_t *slot = &g_sample_slots[slot_idx];
    if (slot->generation == 0) slot->generation = 1;
    slot->in_use = true;
    slot->info.spu_addr = spu_addr;
    slot->info.length = sample_size;
    slot->info.rate = rate;
    slot->info.bitsize = bitsize;
    slot->info.channels = channels;

    return make_sample_handle(slot_idx);
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

void aica_shutdown(void) {
    AICA_ARM_EN_REG = AICA_ARM_EN_REG | 1u;

    volatile uint32_t *aica_reg = (volatile uint32_t *)SPU_RAM_BASE_SH4;
    for (int i = 0; i < 64; i++) {
        volatile uint32_t *slot = (volatile uint32_t *)((uint8_t *)aica_reg + (i * 128));
        slot[0] = 0x8000;
        for (int j = 1; j < 32; j++) slot[j] = 0;
    }
}

const aica_state_t *afx_get_state(void) {
    return &g_aica_state;
}


bool afx_init(void) {
    G2_WAIT_REG = 0x1f;

    aica_shutdown();

    const void *fw_data = afx_driver_data;
    uint32_t fw_size = (uint32_t)afx_driver_size;

    uint32_t fw_spu_addr = AFX_FIRMWARE_DEFAULT_ADDR;
    uint32_t marker_addr = fw_spu_addr + fw_size;
    if (marker_addr < fw_spu_addr) return false;

    if (!range_fits_dynamic(fw_spu_addr, fw_size + sizeof(uint32_t))) return false;

    memcpy((void *)(uintptr_t)(SPU_RAM_BASE_SH4 + fw_spu_addr), fw_data, fw_size);

    uint32_t dynamic_base = align_up_u32(marker_addr + sizeof(uint32_t), 32);
    if (dynamic_base >= AFX_DRIVER_STATE_ADDR) return false;

    *(volatile uint32_t *)(uintptr_t)(SPU_RAM_BASE_SH4 + marker_addr) = dynamic_base;

    memset((void *)(uintptr_t)(SPU_RAM_BASE_SH4 + AFX_IPC_CMD_QUEUE_ADDR), 0, AFX_IPC_QUEUE_SZ);
    memset((void *)(uintptr_t)(SPU_RAM_BASE_SH4 + AFX_DRIVER_STATE_ADDR), 0, sizeof(afx_driver_state_t));
    memset((void *)(uintptr_t)(SPU_RAM_BASE_SH4 + AFX_IPC_CONTROL_ADDR), 0, sizeof(afx_ipc_control_t));

    afx_mem_reset(dynamic_base);
    g_host_available_channels = 0xFFFFFFFFFFFFFFFFULL;

    /* Halt then re-enable ARM7 so it boots from the freshly uploaded firmware */
    // 5. Restart ARM7 CPU
    AICA_ARM_EN_REG = AICA_ARM_EN_REG & ~1;

    /* Poll for ARM7 ready signal; spin up to ~200ms worth of iterations */
    volatile afx_ipc_control_t *ctrl = get_ipc_control();
    uint32_t timeout = 0x800000u;
    while (ctrl->magic != AICAF_MAGIC && --timeout) {}

    if (ctrl->magic != AICAF_MAGIC) return false;

    return true;
}

uint64_t afx_allocate_channels(uint32_t num_channels) {
    return allocate_channels_host(num_channels);
}

bool afx_poll_completed_flow(uint32_t *flow_spu_addr, uint32_t *last_seq) {
    if (!flow_spu_addr || !last_seq) return false;

    volatile afx_ipc_control_t *ctrl = get_ipc_control();
    uint32_t seq = ctrl->completed_flow_seq;
    if (seq == 0 || seq == *last_seq) return false;

    *flow_spu_addr = ctrl->completed_flow_addr;
    *last_seq = seq;
    return true;
}

void afx_release_channels(uint32_t flow_spu_addr) {
    if (flow_spu_addr == 0) return;

    volatile afx_flow_state_t *flow =
        (volatile afx_flow_state_t *)(uintptr_t)(SPU_RAM_BASE_SH4 + flow_spu_addr);
    uint64_t mask = flow->assigned_channels;
    if (mask == 0) return;

    release_channels_host(mask);
    uint64_t zero = 0;
    (void)afx_mem_write(flow_spu_addr + (uint32_t)offsetof(afx_flow_state_t, assigned_channels),
                        &zero, sizeof(zero));
}

uint32_t afx_create_flow(uint32_t song_spu_addr) {
    if (song_spu_addr == 0) return 0;

    const afx_header_t *hdr = (const afx_header_t *)(uintptr_t)(SPU_RAM_BASE_SH4 + song_spu_addr);
    if (!hdr || hdr->magic != AICAF_MAGIC || hdr->version != AICAF_VERSION) return 0;

    const bool uses_external_samples =
        (hdr->flags & AFX_FILE_FLAG_EXTERNAL_SAMPLE_ADDRS) != 0u;
    const afx_section_entry_t *flow_sect = find_section_host(hdr, AFX_SECT_FLOW);
    const afx_section_entry_t *sdat_sect = find_section_host(hdr, AFX_SECT_SDAT);
    if (!flow_sect) return 0;
    if (!uses_external_samples && !sdat_sect) return 0;

    uint32_t required_channels = 0;
    const afx_section_entry_t *meta_sect = find_section_host(hdr, AFX_SECT_META);
    if (meta_sect && meta_sect->size >= sizeof(afx_meta_t)) {
        const afx_meta_t *meta =
            (const afx_meta_t *)(uintptr_t)(SPU_RAM_BASE_SH4 + song_spu_addr + meta_sect->offset);
        if (meta->version == AFX_META_VERSION) {
            required_channels = meta->required_channels;
        }
    }

    if (required_channels == 0 || required_channels > 64u) return 0;

    uint64_t channel_mask = allocate_channels_host(required_channels);
    if (channel_mask == 0) return 0;

    uint32_t flow_addr = afx_mem_alloc(sizeof(afx_flow_state_t), 32);
    if (flow_addr == 0) {
        release_channels_host(channel_mask);
        return 0;
    }

    afx_flow_state_t flow_template;
    memset(&flow_template, 0, sizeof(flow_template));
    flow_template.afx_base = song_spu_addr;
    flow_template.flow_ptr = song_spu_addr + flow_sect->offset;
    flow_template.flow_size = flow_sect->size;
    flow_template.flow_count = flow_sect->count;
    flow_template.flow_idx = 0;
    flow_template.next_event_tick = 0;
    flow_template.assigned_channels = channel_mask;
    flow_template.required_channels = required_channels;
    flow_template.flags =
        uses_external_samples ? AFX_FLOW_FLAG_SAMPLE_ADDRS_ABSOLUTE : 0u;
    build_flow_channel_map_host(&flow_template, required_channels);

    if (!afx_mem_write(flow_addr, &flow_template, sizeof(flow_template))) {
        release_channels_host(channel_mask);
        return 0;
    }

    return flow_addr;
}

bool afx_set_flow_tl_scale_lut(uint32_t flow_spu_addr, uint32_t lut_spu_addr) {
    if (flow_spu_addr == 0) return false;

    uint32_t field_addr = flow_spu_addr + (uint32_t)offsetof(afx_flow_state_t, tl_scale_lut_ptr);
    return afx_mem_write(field_addr, &lut_spu_addr, sizeof(lut_spu_addr));
}

void afx_play_flow(uint32_t flow_spu_addr) {
    if (flow_spu_addr != 0) {
        volatile afx_flow_state_t *flow =
            (volatile afx_flow_state_t *)(uintptr_t)(SPU_RAM_BASE_SH4 + flow_spu_addr);
        apply_song_dsp_sections_host(flow->afx_base);
    }
    (void)afx_ipc_push(AICAF_CMD_PLAY_FLOW, flow_spu_addr, 0, 0);
}

void afx_pause_flow(uint32_t flow_spu_addr) {
    (void)afx_ipc_push(AICAF_CMD_PAUSE_FLOW, flow_spu_addr, 0, 0);
}

void afx_resume_flow(uint32_t flow_spu_addr) {
    (void)afx_ipc_push(AICAF_CMD_RESUME_FLOW, flow_spu_addr, 0, 0);
}

void afx_stop_flow(uint32_t flow_spu_addr) {
    (void)afx_ipc_push(AICAF_CMD_STOP_FLOW, flow_spu_addr, 0, 0);
}

void afx_seek_flow(uint32_t flow_spu_addr, uint32_t tick_ms) {
    if (flow_spu_addr == 0) return;

    volatile afx_flow_state_t *flow =
        (volatile afx_flow_state_t *)(uintptr_t)(SPU_RAM_BASE_SH4 + flow_spu_addr);
    if (flow->flow_ptr == 0 || flow->flow_size == 0 || flow->flow_count == 0) return;

    uint32_t flow_offset = afx_cmd_lower_bound_by_offset(
        (const uint8_t *)(uintptr_t)(SPU_RAM_BASE_SH4 + flow->flow_ptr),
        flow->flow_size,
        flow->flow_count,
        tick_ms
    );

    (void)afx_ipc_push(AICAF_CMD_SEEK_FLOW, flow_spu_addr, flow_offset, 0);
}

uint32_t afx_get_tick(void) {
    return get_ipc_control()->current_tick;
}

void afx_set_volume(uint8_t vol) {
    (void)afx_ipc_push(AICAF_CMD_VOLUME, (uint32_t)vol, 0, 0);
}
