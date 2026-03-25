#ifndef AFX_MEMORY_H
#define AFX_MEMORY_H

#include <stdint.h>
#include <stdbool.h>
#include <afx/driver.h>

extern aica_state_t g_aica_state;
extern uint64_t g_host_available_channels;

void afx_mem_reset(uint32_t dynamic_base);
uint32_t afx_mem_alloc(uint32_t size, uint32_t align);
bool afx_mem_free(uint32_t spu_addr, uint32_t size);
bool afx_mem_write(uint32_t spu_addr, const void *src, uint32_t size);

uint32_t afx_upload_afx(const void *afx_data, uint32_t afx_size);
bool afx_free_afx(uint32_t spu_addr);

uint32_t afx_upload_tl_scale_lut(const uint8_t lut[256]);
uint32_t afx_create_tl_scale_lut(uint8_t volume);

uint32_t afx_sample_upload(const char *buf, size_t len,
                           uint32_t rate, uint8_t bitsize, uint8_t channels);
bool afx_sample_free(uint32_t sample_handle);
uint32_t afx_sample_get_spu_addr(uint32_t sample_handle);
bool afx_sample_get_info(uint32_t sample_handle, afx_sample_info_t *out_info);

uint64_t afx_channels_allocate(uint32_t num_channels);
void afx_channels_release(uint8_t flow_slot);

#endif /* AFX_MEMORY_H */