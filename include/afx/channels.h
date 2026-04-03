#ifndef AFX_CHANNELS_H
#define AFX_CHANNELS_H

#include <stdint.h>

uint64_t afx_channels_allocate(uint8_t num_channels);
void afx_channels_release(uint64_t channel_mask);

uint32_t afx_channel_setup_mapping(uint8_t num_channels, uint64_t channel_mask);

void afx_channel_release_mapping(uint8_t num_channels, uint32_t spu_ptr);

#endif /* AFX_CHANNELS_H */