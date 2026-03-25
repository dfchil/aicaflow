#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <limits.h>
#include <string.h>
#include <afx/host.h>
#include <afx/memory.h>
#include <afx/bin/driver_blob.h>
#include <kos.h> 


#ifdef KOS_HEADERS
#include <arch/timer.h>
#include <dc/sound/sound.h>
#include <dc/spu.h>
#endif

#define AICA_DSP_COEF_ADDR 0x00801000u
#define AICA_DSP_MPRO_ADDR 0x00803000u

#define G2_WAIT_REG (*(volatile uint32_t *)0xa05f68a0)
#define AICA_ARM_EN_REG (*(volatile uint32_t *)0xa0702c00)
#define AFX_STATE 


#define drv_state_ptr ((volatile afx_driver_state_t *)(SPU_RAM_BASE_SH4 + AFX_DRIVER_STATE_ADDR))

static void build_flow_channel_map_host(afx_flow_state_t *flow, uint64_t mask) {
    uint32_t flow_chn = 0;
    for (uint32_t hw = 0; hw < 64u; hw++) {
        if (mask & (1ULL << hw)) {
            afx_channel_map_set(flow, flow_chn++, hw);
        }
    }
}

static void apply_song_dsp_sections_host(uint32_t song_spu_addr) {
    const afx_header_t *hdr = (const afx_header_t *)(uintptr_t)(SPU_RAM_BASE_SH4 + song_spu_addr);
    if (!hdr || hdr->magic != AICAF_MAGIC || hdr->version != AICAF_VERSION) return;

    const afx_section_entry_t *dspc_sect = find_afx_section(hdr, AFX_SECT_DSPC);
    const afx_section_entry_t *dspm_sect = find_afx_section(hdr, AFX_SECT_DSPM);

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
    for (uint32_t tries = 0; tries < 100000u; tries++) {
        uint32_t head = ipc_ctrl_ptr->q_head;
        uint32_t tail = ipc_ctrl_ptr->q_tail;
        uint32_t next = (head + 1u) & (AFX_IPC_QUEUE_CAPACITY - 1u);
        if (next == tail) continue;

        ipc_queue_ptr[head].cmd = cmd;
        ipc_queue_ptr[head].arg0 = arg0;
        ipc_queue_ptr[head].arg1 = arg1;
        ipc_queue_ptr[head].arg2 = arg2;
        ipc_ctrl_ptr->q_head = next;
        return true;
    }

    return false;
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
    
    // volatile afx_driver_state_t *driver =
    //     (volatile afx_driver_state_t *)(uintptr_t)(SPU_RAM_BASE_SH4 + AFX_DRIVER_STATE_ADDR);

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


bool afx_init(void) {
    G2_WAIT_REG = 0x1f;

    aica_shutdown();

    const void *fw_data = afx_driver_data;
    uint32_t fw_size = (uint32_t)afx_driver_size;

    uint32_t fw_spu_addr = fw_size;
    uint32_t marker_addr = fw_spu_addr + fw_size;
    if (marker_addr < fw_spu_addr) return false;

    if (!range_fits_dynamic(fw_spu_addr, fw_size + sizeof(uint32_t))) return false;

    memcpy((void *)(uintptr_t)(SPU_RAM_BASE_SH4 + fw_spu_addr), fw_data, fw_size);

    uint32_t dynamic_base = align_up_u32(marker_addr + sizeof(uint32_t), 32);
    if (dynamic_base >= AFX_DRIVER_STATE_ADDR) return false;

    *(volatile uint32_t *)(uintptr_t)(SPU_RAM_BASE_SH4 + marker_addr) = dynamic_base;

    memset((void *)(uintptr_t)(SPU_RAM_BASE_SH4 + AFX_DRIVER_STATE_ADDR), 0, sizeof(afx_driver_state_t));

    afx_mem_reset(dynamic_base);
    g_host_available_channels = 0xFFFFFFFFFFFFFFFFULL;

    /* Halt then re-enable ARM7 so it boots from the freshly uploaded firmware */
    // 5. Restart ARM7 CPU
    AICA_ARM_EN_REG = AICA_ARM_EN_REG & ~1;

    /* Poll for ARM7 ready signal; spin up to ~200ms worth of iterations */
    uint32_t timeout = 0x800000u;
    while (drv_state_ptr->stack_canary != 0xDEADB12D && --timeout);
    if (drv_state_ptr->stack_canary != 0xDEADB12D)
        return false;

    return true;
}

bool afx_flow_poll_completed(uint8_t *flow_slot) {
    if (!flow_slot) return false;

    volatile afx_driver_state_t *driver =
        (volatile afx_driver_state_t *)(uintptr_t)(SPU_RAM_BASE_SH4 + AFX_DRIVER_STATE_ADDR);

    if (driver->flow_count_retired == 0) return false;

    uint8_t active = driver->flow_count_active;
    uint32_t retired_addr = driver->flow_entries[active];
    uint8_t retired_slot = flow_addr_to_slot(retired_addr);
    if (retired_slot == 0xFFu) return false;

    /* Shift remaining retired flow addresses down by one. */
    for (uint8_t i = 1; i < driver->flow_count_retired; i++) {
        driver->flow_entries[active + i - 1] = driver->flow_entries[active + i];
    }
    driver->flow_count_retired--;

    *flow_slot = retired_slot;
    return true;
}

uint8_t afx_flow_init(uint32_t song_spu_addr) {

    if (drv_state_ptr->flow_count_active + drv_state_ptr->flow_count_retired >= AFX_FLOW_POOL_CAPACITY) {
        return 0xFFu;
    }

    if (song_spu_addr == 0) return 0xFFu;

    const afx_header_t *hdr = (const afx_header_t *)(uintptr_t)(SPU_RAM_BASE_SH4 + song_spu_addr);
    if (!hdr || hdr->magic != AICAF_MAGIC || hdr->version != AICAF_VERSION) return 0xFFu;

    const bool uses_external_samples =
        (hdr->flags & AFX_FILE_FLAG_EXTERNAL_SAMPLE_ADDRS) != 0u;
    const afx_section_entry_t *flow_sect = find_afx_section(hdr, AFX_SECT_FLOW);
    const afx_section_entry_t *sdat_sect = find_afx_section(hdr, AFX_SECT_SDAT);
    if (!flow_sect) return 0xFFu;
    if (!uses_external_samples && !sdat_sect) return 0xFFu;

    uint32_t required_channels = hdr->required_channels;

    if (required_channels == 0 || required_channels > 64u) return 0xFFu;

    uint64_t channel_mask = afx_channels_allocate(required_channels);
    if (channel_mask == 0) return 0xFFu;

    uint32_t flow_addr = afx_mem_alloc(sizeof(afx_flow_state_t), 32);
    if (flow_addr == 0) {
        afx_channels_release(channel_mask);
        return 0xFFu;
    }

    afx_flow_state_t flow_template;
    memset(&flow_template, 0, sizeof(flow_template));
    flow_template.afx_base = song_spu_addr;
    flow_template.flow_ptr = song_spu_addr + flow_sect->offset;
    flow_template.flow_offset = 0;
    flow_template.next_event_tick = 0;
    flow_template.flags.sample_addr_mode = uses_external_samples;
    build_flow_channel_map_host(&flow_template, required_channels);

    if (!afx_mem_write(flow_addr, &flow_template, sizeof(flow_template))) {
        afx_channels_release(channel_mask);
        afx_mem_free(flow_addr, sizeof(afx_flow_state_t));
        return 0xFFu;
    }

    uint8_t slot = flow_addr_to_slot(flow_addr);
    return slot;
}

bool afx_flow_set_tl_scale_lut(uint8_t flow_slot, uint32_t lut_spu_addr) {
    uint32_t flow_addr = flow_slot_to_addr(flow_slot);
    if (flow_addr == 0) return false;

    uint32_t field_addr = flow_addr + (uint32_t)offsetof(afx_flow_state_t, tl_scale_lut_ptr);
    return afx_mem_write(field_addr, &lut_spu_addr, sizeof(lut_spu_addr));
}

void afx_flow_play(uint8_t flow_slot) {
    uint32_t flow_addr = flow_slot_to_addr(flow_slot);
    if (flow_addr != 0) {
        volatile afx_flow_state_t *flow = (volatile afx_flow_state_t *)(uintptr_t)(SPU_RAM_BASE_SH4 + flow_addr);
        apply_song_dsp_sections_host(flow->afx_base);
    }
    (void)afx_ipc_push(AICAF_CMD_PLAY_FLOW, (uint32_t)flow_slot, 0, 0);
}

void afx_flow_pause(uint8_t flow_slot) {
    (void)afx_ipc_push(AICAF_CMD_PAUSE_FLOW, (uint32_t)flow_slot, 0, 0);
}

void afx_flow_resume(uint8_t flow_slot) {
    (void)afx_ipc_push(AICAF_CMD_RESUME_FLOW, (uint32_t)flow_slot, 0, 0);
}

void afx_flow_stop(uint8_t flow_slot) {
    (void)afx_ipc_push(AICAF_CMD_STOP_FLOW, (uint32_t)flow_slot, 0, 0);
}

void afx_flow_seek(uint8_t flow_slot, uint32_t tick_ms) {

    volatile afx_flow_state_t *flow = drv_state_ptr->flow_entries + flow_slot;
    volatile afx_header_t *hdr = (volatile afx_header_t *)(uintptr_t)(SPU_RAM_BASE_SH4 + flow->afx_base);
    volatile afx_section_entry_t *flow_sect = find_afx_section(hdr, AFX_SECT_FLOW);

    uint32_t flow_offset = afx_cmd_lower_bound_by_offset(
        flow->flow_ptr,
        flow_sect->size,
        flow_sect->count,
        tick_ms
    );
    (void)afx_ipc_push(AICAF_CMD_SEEK_FLOW, (uint32_t)flow_slot, flow_offset, 0);
}

