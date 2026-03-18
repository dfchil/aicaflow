#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <aicaflow/aicaflow.h>

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

static volatile aicaflow_ipc_status_t *get_ipc_status() {
    return (volatile aicaflow_ipc_status_t *)(SPU_RAM_BASE + AICAFLOW_IPC_STATUS_ADDR);
}

bool aicaflow_init(void) {
    volatile aicaflow_ipc_status_t *status = get_ipc_status();
    
    // Safety check: is the ARM driver even responding?
    if (status->magic != AICAF_MAGIC) {
        return false;
    }

    return true;
}

void aicaflow_play(uint32_t song_spu_addr) {
    volatile aicaflow_ipc_status_t *status = get_ipc_status();
    
    // Copy song header from file/memory to let ARM know where stream is
    // Actually, we usually pass the song base address to the ARM7 so it can map pointers
    status->cmd_arg = song_spu_addr;
    status->cmd = AICAF_CMD_PLAY;
}

void aicaflow_stop(void) {
    volatile aicaflow_ipc_status_t *status = get_ipc_status();
    status->cmd = AICAF_CMD_STOP;
}

uint32_t aicaflow_get_tick(void) {
    return get_ipc_status()->current_tick;
}

void aicaflow_set_volume(uint8_t vol) {
    volatile aicaflow_ipc_status_t *status = get_ipc_status();
    status->volume = vol;
    status->cmd = AICAF_CMD_VOLUME;
}

bool aicaflow_is_playing(void) {
    return get_ipc_status()->arm_status == 1;
}
