#ifndef AFX_CHANNELS_H
#define AFX_CHANNELS_H

#include <stdint.h>

uint64_t afx_channels_allocate(uint8_t num_channels);
void afx_channels_release(uint64_t channel_mask);



#endif /* AFX_CHANNELS_H */