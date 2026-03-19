#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <afx/afx.h>

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
#define SPU_RAM_UNCACHED 0xA0000000 /* Adjust if needed for specific KOS setup */

static volatile afx_ipc_status_t *get_ipc_status() {
    return (volatile afx_ipc_status_t *)(SPU_RAM_BASE + AFX_IPC_STATUS_ADDR);
}

bool afx_init(void) {
    volatile afx_ipc_status_t *status = get_ipc_status();
    
    // Safety check: is the ARM driver even responding?
    if (status->magic != AICAF_MAGIC) {
        return false;
    }

    return true;
}

void afx_play(uint32_t song_spu_addr) {
    volatile afx_ipc_status_t *status = get_ipc_status();
    
    // Copy song header from file/memory to let ARM know where stream is
    // Actually, we usually pass the song base address to the ARM7 so it can map pointers
    status->cmd_arg = song_spu_addr;
    status->cmd = AICAF_CMD_PLAY;
}

void afx_stop(void) {
    volatile afx_ipc_status_t *status = get_ipc_status();
    status->cmd = AICAF_CMD_STOP;
}

void afx_pause(void) {
    volatile afx_ipc_status_t *status = get_ipc_status();
    status->cmd = AICAF_CMD_PAUSE;
}

uint32_t afx_get_tick(void) {
    return get_ipc_status()->current_tick;
}

void afx_set_volume(uint8_t vol) {
    volatile afx_ipc_status_t *status = get_ipc_status();
    status->volume = vol;
    status->cmd = AICAF_CMD_VOLUME;
}

bool afx_is_playing(void) {
    return get_ipc_status()->arm_status == 1;
}

void afx_seek(uint32_t tick_ms) {
    volatile afx_ipc_status_t *status = get_ipc_status();
    status->cmd_arg = tick_ms;
    status->cmd = AICAF_CMD_SEEK;
}
