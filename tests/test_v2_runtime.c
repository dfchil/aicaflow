#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include <afx/afx.h>

static void test_struct_layout(void) {
    assert(sizeof(afx_header_t) == 52);
    assert(sizeof(afx_sample_desc_t) == 32);
    assert(sizeof(afx_opcode_t) == 12);

    assert(AICAF_MAGIC == 0xA1CAF200u);
    assert(AICAF_VERSION == 2u);
    assert(AICAF_CMD_SEEK == 5u);
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
    afx_opcode_t stream[] = {
        { .timestamp = 0 },
        { .timestamp = 10 },
        { .timestamp = 10 },
        { .timestamp = 25 },
        { .timestamp = 40 },
    };

    assert(afx_opcode_lower_bound_by_tick(stream, 5, 0) == 0);
    assert(afx_opcode_lower_bound_by_tick(stream, 5, 1) == 1);
    assert(afx_opcode_lower_bound_by_tick(stream, 5, 10) == 1);
    assert(afx_opcode_lower_bound_by_tick(stream, 5, 11) == 3);
    assert(afx_opcode_lower_bound_by_tick(stream, 5, 39) == 4);
    assert(afx_opcode_lower_bound_by_tick(stream, 5, 40) == 4);
    assert(afx_opcode_lower_bound_by_tick(stream, 5, 41) == 5);
}

int main(void) {
    test_struct_layout();
    test_scale_total_level();
    test_seek_lower_bound();
    puts("PASS: v2 runtime tests");
    return 0;
}
