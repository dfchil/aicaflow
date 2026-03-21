#ifndef AFX_HOST_H
#define AFX_HOST_H

#include <stdint.h>
#include <stdbool.h>

#include <afx/common.h>
#include <afx/driver.h>

typedef struct {
    uint32_t dynamic_base;
    uint32_t dynamic_cursor;
} aica_state_t;

void aicaplayer_shutdown(void);

void afx_mem_reset(uint32_t dynamic_base);
uint32_t afx_mem_alloc(uint32_t size, uint32_t align);
bool afx_mem_write(uint32_t spu_addr, const void *src, uint32_t size);
uint32_t afx_upload_afx(const void *afx_data, uint32_t afx_size);

bool afx_upload_and_init_firmware(const void *fw_data, uint32_t fw_size);
const aica_state_t *afx_get_state(void);

bool afx_init(void);
void afx_play(uint32_t song_spu_addr);
void afx_stop(void);
void afx_pause(void);
uint32_t afx_get_tick(void);
void afx_set_volume(uint8_t vol);
bool afx_is_playing(void);
void afx_seek(uint32_t tick_ms);

#endif /* AFX_HOST_H */
