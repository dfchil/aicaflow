#ifndef AFX_HOST_H
#define AFX_HOST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <afx/common.h>
#include <afx/driver.h>

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

void afx_mem_reset(uint32_t dynamic_base);
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

const aica_state_t *afx_get_state(void);

bool afx_init(void);

/* Flow-based playback API */
uint32_t afx_create_flow(uint32_t song_spu_addr);
bool afx_set_flow_tl_scale_lut(uint32_t flow_spu_addr, uint32_t lut_spu_addr);
void afx_play_flow(uint32_t flow_spu_addr);

/* Channel management API */
uint64_t afx_allocate_channels(uint32_t num_channels);
void afx_release_channels(uint32_t flow_spu_addr);
void afx_pause_flow(uint32_t flow_spu_addr);
void afx_resume_flow(uint32_t flow_spu_addr);
void afx_stop_flow(uint32_t flow_spu_addr);
void afx_seek_flow(uint32_t flow_spu_addr, uint32_t tick_ms);
bool afx_poll_completed_flow(uint32_t *flow_spu_addr, uint32_t *last_seq);

/* Global controls */
uint32_t afx_get_tick(void);
void afx_set_volume(uint8_t vol);

#endif /* AFX_HOST_H */
