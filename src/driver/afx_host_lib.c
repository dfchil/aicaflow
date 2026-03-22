#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <afx/host.h>
#include <afx/afx_driver_blob.h>


/* KOS specific headers (simulated/assumed available in user's KOS setup) */
#ifdef KOS_HEADERS
#include <arch/timer.h>
#include <dc/sound/sound.h>
#include <dc/spu.h>
#endif

/* 
 * AICA Flow SH4 Host API
 * This runs on the SH4 (main CPU) to control the ARM7 driver
 */

#define SPU_RAM_BASE 0xA0800000 /* SH4 cached view of SPU RAM (mirrored) */

#define AFX_DYNAMIC_DEFAULT_BASE 0x00002000u
#define AFX_FIRMWARE_DEFAULT_ADDR 0x00000000u


// Bare-Metal hardware registers
#define G2_WAIT_REG (*(volatile uint32_t*)0xa05f68a0)
#define AICA_ARM_EN_REG (*(volatile uint32_t*)0xa0702c00)

static aica_state_t g_aica_state = {
    .dynamic_base = AFX_DYNAMIC_DEFAULT_BASE,
    .dynamic_cursor = AFX_DYNAMIC_DEFAULT_BASE,
};

static volatile afx_ipc_status_t *get_ipc_status(void);

static volatile afx_ipc_cmd_t *get_ipc_queue(void) {
    return (volatile afx_ipc_cmd_t *)(SPU_RAM_BASE + AFX_IPC_CMD_QUEUE_ADDR);
}

static bool afx_queue_push(uint32_t cmd, uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    volatile afx_ipc_cmd_t *queue = get_ipc_queue();

    for (uint32_t tries = 0; tries < 100000u; tries++) {
        volatile afx_ipc_status_t *status = get_ipc_status();
        uint32_t head = status->q_head;
        uint32_t tail = status->q_tail;
        uint32_t next = (head + 1u) & (AFX_IPC_QUEUE_CAPACITY - 1u);
        if (next == tail) continue; // queue full; wait for ARM7 to consume

        queue[head].cmd = cmd;
        queue[head].arg0 = arg0;
        queue[head].arg1 = arg1;
        queue[head].arg2 = arg2;
        status->q_head = next;
        return true;
    }
    return false;
}

static inline bool range_fits_dynamic(uint32_t addr, uint32_t size) {
    if (size == 0) return false;
    if (addr >= AFX_PLAYER_STATE_ADDR) return false;
    if (addr + size < addr) return false;
    return (addr + size) <= AFX_PLAYER_STATE_ADDR;
}

static inline uint32_t align_up_u32(uint32_t value, uint32_t align) {
    if (align == 0) return value;
    uint32_t mask = align - 1;
    return (value + mask) & ~mask;
}

void afx_mem_reset(uint32_t dynamic_base) {
    if (dynamic_base == 0) dynamic_base = AFX_DYNAMIC_DEFAULT_BASE;
    dynamic_base = align_up_u32(dynamic_base, 32);
    if (dynamic_base >= AFX_PLAYER_STATE_ADDR) dynamic_base = AFX_DYNAMIC_DEFAULT_BASE;
    g_aica_state.dynamic_base = dynamic_base;
    g_aica_state.dynamic_cursor = dynamic_base;
}

uint32_t afx_mem_alloc(uint32_t size, uint32_t align) {
    uint32_t use_align = (align == 0) ? 32u : align;
    uint32_t start = align_up_u32(g_aica_state.dynamic_cursor, use_align);
    uint32_t end = start + size;
    if (size == 0 || end > AFX_PLAYER_STATE_ADDR || end < start) {
        return 0;
    }
    g_aica_state.dynamic_cursor = end;
    return start;
}

bool afx_mem_write(uint32_t spu_addr, const void *src, uint32_t size) {
    if (!src || size == 0) return false;
    if (!range_fits_dynamic(spu_addr, size)) return false;
    memcpy((void *)(uintptr_t)(SPU_RAM_BASE + spu_addr), src, size);
    return true;
}

uint32_t afx_upload_afx(const void *afx_data, uint32_t afx_size) {
    if (!afx_data || afx_size == 0) return 0;
    uint32_t spu_addr = afx_mem_alloc(afx_size, 32);
    if (spu_addr == 0) return 0;
    if (!afx_mem_write(spu_addr, afx_data, afx_size)) return 0;
    return spu_addr;
}


void aicaplayer_shutdown(void) {
    // 1. Halt AICA ARM7 CPU
    AICA_ARM_EN_REG = AICA_ARM_EN_REG | 1;

    // 2. Reset the AICA Hardware state from the SH-4 side
    // We clear the channel registers to stop all sound immediately
    volatile uint32_t* aica_reg = (volatile uint32_t*)SPU_RAM_BASE;
    for(int i = 0; i < 64; i++) {
        // Offset to channel i's registers (128 bytes per slot)
        volatile uint32_t* slot = (volatile uint32_t*)((uint8_t*)aica_reg + (i * 128));
        slot[0] = 0x8000; // Key OFF, 16-bit
        for(int j = 1; j < 32; j++) slot[j] = 0;
    }
}


bool afx_upload_and_init_firmware(const void *fw_data,
                                  uint32_t fw_size) {

    // 1. Initialize G2 Wait States for safe AICA memory writing
    G2_WAIT_REG = 0x1f;

    if (!fw_data || fw_size == 0) return false;

    uint32_t fw_spu_addr = AFX_FIRMWARE_DEFAULT_ADDR;

    /* We store a u32 marker immediately after the firmware image. */
    uint32_t marker_addr = fw_spu_addr + fw_size;
    if (marker_addr < fw_spu_addr) return false;

    if (!range_fits_dynamic(fw_spu_addr, fw_size + sizeof(uint32_t))) return false;

    memcpy((void *)(uintptr_t)(SPU_RAM_BASE + fw_spu_addr), fw_data, fw_size);

    uint32_t dynamic_base = align_up_u32(marker_addr + sizeof(uint32_t), 32);
    if (dynamic_base >= AFX_PLAYER_STATE_ADDR) return false;

    *(volatile uint32_t *)(uintptr_t)(SPU_RAM_BASE + marker_addr) = dynamic_base;

    /* Clear command queue and control/status block before boot handoff. */
    memset((void *)(uintptr_t)(SPU_RAM_BASE + AFX_IPC_CMD_QUEUE_ADDR), 0, AFX_IPC_QUEUE_SZ);
    memset((void *)(uintptr_t)(SPU_RAM_BASE + AFX_PLAYER_STATE_ADDR), 0, sizeof(afx_player_state_t));
    memset((void *)(uintptr_t)(SPU_RAM_BASE + AFX_IPC_STATUS_ADDR), 0, sizeof(afx_ipc_status_t));

    afx_mem_reset(dynamic_base);
    return true;
}

const aica_state_t *afx_get_state(void) {
    return &g_aica_state;
}

static volatile afx_ipc_status_t *get_ipc_status(void) {
    return (volatile afx_ipc_status_t *)(SPU_RAM_BASE + AFX_IPC_STATUS_ADDR);
}

bool afx_init(void) {
    volatile afx_ipc_status_t *status = get_ipc_status();
    
    // Safety check: is the ARM driver even responding?
    if (status->magic != AICAF_MAGIC) {
        return false;
    }

    afx_mem_reset(AFX_DYNAMIC_DEFAULT_BASE);

    return true;
}

void afx_play(uint32_t song_spu_addr) {
    (void)afx_queue_push(AICAF_CMD_PLAY, song_spu_addr, 0, 0);
}

void afx_stop(void) {
    (void)afx_queue_push(AICAF_CMD_STOP, 0, 0, 0);
}

void afx_pause(void) {
    (void)afx_queue_push(AICAF_CMD_PAUSE, 0, 0, 0);
}

uint32_t afx_get_tick(void) {
    return get_ipc_status()->current_tick;
}

void afx_set_volume(uint8_t vol) {
    (void)afx_queue_push(AICAF_CMD_VOLUME, (uint32_t)vol, 0, 0);
}

bool afx_is_playing(void) {
    return get_ipc_status()->arm_status == 1;
}

void afx_seek(uint32_t tick_ms) {
    (void)afx_queue_push(AICAF_CMD_SEEK, tick_ms, 0, 0);
}
