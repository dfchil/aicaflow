#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <afx/driver.h>
#include <afx/host.h>


static inline uint32_t afx_scale_total_level(uint32_t tl, uint32_t volume) {
  uint32_t x = (255u - (tl & 0xFFu)) * (volume & 0xFFu);
  uint32_t scaled = (x + 1u + (x >> 8)) >> 8;
  return 255u - scaled;
}


static void test_struct_layout(void) {
    assert(sizeof(afx_header_t) == 24u);
    assert(sizeof(afx_section_entry_t) == 24);
    assert(sizeof(afx_sample_desc_t) == 32);
    /* Variable-length command header is 6 bytes + values array */
    assert(sizeof(afx_cmd_t) == 6u); 

    assert(AICAF_MAGIC == 0xA1CAF100u);
    assert(AICAF_VERSION == 1u);
    assert(AFX_META_VERSION == 1u);
    assert(AICAF_CMD_SEEK_FLOW == 6u);
    assert(AFX_SECT_SDAT == 0x54414453u);

    /* Packed-flow command layout is part of the file ABI. */
    assert(offsetof(afx_cmd_t, timestamp) == 0u);
}

static void test_driver_state_abi(void) {
    printf("(offsetof(afx_driver_state_t, flow_states) = %zu)\n", offsetof(afx_driver_state_t, flow_states));
    /* Driver runtime state now uses array flow entries and counters. */
    assert(sizeof(afx_driver_state_t) == 3664u);
    assert(offsetof(afx_driver_state_t, stack_canary) == 0u);
    assert(offsetof(afx_driver_state_t, mini_stack) == 4u);
    assert(offsetof(afx_driver_state_t, flow_states) == 1616u);

    /* Flow state carries optional per-flow LUT pointer. */
    assert(offsetof(afx_flow_state_t, tl_scale_lut_ptr) == 20u);
}

static void test_memory_layout_invariants(void) {
    /* Reserved control blocks should be naturally aligned and ordered high->low. */
    assert((AFX_DRIVER_STATE_ADDR & 31u) == 0u);
}

static void test_scale_total_level(void) {
    for (uint32_t tl = 0; tl <= 255; ++tl) {
        for (uint32_t vol = 0; vol <= 255; ++vol) {
            uint32_t expected = 255u - (((255u - tl) * vol) / 255u);
            uint32_t actual = afx_scale_total_level(tl, vol);
            if (actual != expected) {
                fprintf(stderr,
                        "scale mismatch: tl=%u vol=%u expected=%u actual=%u\n",
                        tl, vol, expected, actual);
                assert(actual == expected);
            }
        }
    }

    assert(afx_scale_total_level(0, 255) == 0);
    assert(afx_scale_total_level(0, 0) == 255);
    assert(afx_scale_total_level(255, 255) == 255);
}

static void test_seek_lower_bound(void) {
    uint8_t stream[256] = {0};
    uint32_t ptr = 0;
    
    // Command 0: T=0, L=1 (8 bytes total: 4B timestamp + 2B header + 2B val)
    ptr = 0;
    // timestamp = 0
    stream[ptr+0] = 0x00; stream[ptr+1] = 0x00; stream[ptr+2] = 0x00; stream[ptr+3] = 0x00;
    // slot=0, offset=0, length=1
    // length is the high 5 bits of the 16-bit word. 1 << 11 = 0x0800
    // In little-endian: [0x00, 0x08]
    stream[ptr+4] = 0x00; stream[ptr+5] = 0x08; 
    ptr += 8;
    
    // Command 1: T=10, L=2 (12 bytes total: 4B T + 2B H + 4B vals + 2B padding)
    // timestamp = 10
    stream[ptr+0] = 0x0A; stream[ptr+1] = 0x00; stream[ptr+2] = 0x00; stream[ptr+3] = 0x00;
    // slot=0, offset=0, length=2. 2 << 11 = 0x1000 -> [0x00, 0x10]
    stream[ptr+4] = 0x00; stream[ptr+5] = 0x10;
    ptr += 12;

    // Command 2: T=25, L=1 (8 bytes)
    // timestamp = 25
    stream[ptr+0] = 0x19; stream[ptr+1] = 0x00; stream[ptr+2] = 0x00; stream[ptr+3] = 0x00;
    stream[ptr+4] = 0x00; stream[ptr+5] = 0x08;
    ptr += 8;

    uint32_t r0 = afx_cmd_lower_bound_by_offset(stream, ptr, 3, 0);
    uint32_t r5 = afx_cmd_lower_bound_by_offset(stream, ptr, 3, 5);
    uint32_t r10 = afx_cmd_lower_bound_by_offset(stream, ptr, 3, 10);
    uint32_t r20 = afx_cmd_lower_bound_by_offset(stream, ptr, 3, 20);
    uint32_t r25 = afx_cmd_lower_bound_by_offset(stream, ptr, 3, 25);
    uint32_t r30 = afx_cmd_lower_bound_by_offset(stream, ptr, 3, 30);
    
    // T=0 is at offset 0
    assert(r0 == 0);
    // T=5 -> search for >=5 -> Command 1 (T=10) at offset 8
    assert(r5 == 8);
    // T=10 is at offset 8
    assert(r10 == 8);
    // T=20 -> search for >=20 -> Command 2 (T=25) at offset 20 (8+12)
    assert(r20 == 20);
    // T=25 is at offset 20
    assert(r25 == 20);
    // T=30 -> end of stream at offset 28 (20+8)
    assert(r30 == 28);
}

int main(void) {
    test_struct_layout();
    test_driver_state_abi();
    test_memory_layout_invariants();
    test_scale_total_level();
    test_seek_lower_bound();
    puts("PASS: runtime tests");
    return 0;
}
