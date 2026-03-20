#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <afx/driver.h>
#include <afx/host.h>

static void test_struct_layout(void) {
    assert(sizeof(afx_header_t) == 20);
    assert(sizeof(afx_section_entry_t) == 24);
    assert(sizeof(afx_sample_desc_t) == 32);
    assert(sizeof(afx_cmd_t) == 12);

    assert(AICAF_MAGIC == 0xA1CAF200u);
    assert(AICAF_VERSION == 1u);
    assert(AICAF_CMD_SEEK == 5u);
    assert(AFX_SECT_FLOW == 0x574F4C46u);
    assert(AFX_SECT_SDAT == 0x54414453u);

    /* Packed-flow command layout is part of the file ABI. */
    assert(offsetof(afx_cmd_t, timestamp) == 0u);
    assert(offsetof(afx_cmd_t, slot) == 4u);
    assert(offsetof(afx_cmd_t, reg) == 5u);
    assert(offsetof(afx_cmd_t, value) == 8u);
}

static void test_memory_layout_invariants(void) {
    /* Reserved control blocks should be naturally aligned and ordered high->low. */
    assert((AFX_IPC_STATUS_ADDR & 31u) == 0u);
    assert((AFX_PLAYER_STATE_ADDR & 31u) == 0u);
    assert((AFX_IPC_CMD_QUEUE_ADDR & 31u) == 0u);

    assert(AFX_IPC_STATUS_ADDR > AFX_IPC_CMD_QUEUE_ADDR);
    assert(AFX_IPC_CMD_QUEUE_ADDR > AFX_PLAYER_STATE_ADDR);

    /* Queue capacity must match its byte block and element size. */
    assert((AFX_IPC_QUEUE_SZ % sizeof(afx_ipc_cmd_t)) == 0u);
    assert(AFX_IPC_QUEUE_CAPACITY == (AFX_IPC_QUEUE_SZ / sizeof(afx_ipc_cmd_t)));
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
    afx_cmd_t stream[] = {
        { .timestamp = 0 },
        { .timestamp = 10 },
        { .timestamp = 10 },
        { .timestamp = 25 },
        { .timestamp = 40 },
    };

    assert(afx_cmd_lower_bound_by_tick(stream, 5, 0) == 0);
    assert(afx_cmd_lower_bound_by_tick(stream, 5, 1) == 1);
    assert(afx_cmd_lower_bound_by_tick(stream, 5, 10) == 1);
    assert(afx_cmd_lower_bound_by_tick(stream, 5, 11) == 3);
    assert(afx_cmd_lower_bound_by_tick(stream, 5, 39) == 4);
    assert(afx_cmd_lower_bound_by_tick(stream, 5, 40) == 4);
    assert(afx_cmd_lower_bound_by_tick(stream, 5, 41) == 5);
}

static void test_seek_lower_bound_edge_cases(void) {
    afx_cmd_t empty_stream[1] = {0};
    assert(afx_cmd_lower_bound_by_tick(empty_stream, 0, 123) == 0);

    afx_cmd_t one[] = {
        { .timestamp = 7 },
    };
    assert(afx_cmd_lower_bound_by_tick(one, 1, 0) == 0);
    assert(afx_cmd_lower_bound_by_tick(one, 1, 7) == 0);
    assert(afx_cmd_lower_bound_by_tick(one, 1, 8) == 1);

    afx_cmd_t all_same[] = {
        { .timestamp = 100 },
        { .timestamp = 100 },
        { .timestamp = 100 },
    };
    assert(afx_cmd_lower_bound_by_tick(all_same, 3, 99) == 0);
    assert(afx_cmd_lower_bound_by_tick(all_same, 3, 100) == 0);
    assert(afx_cmd_lower_bound_by_tick(all_same, 3, 101) == 3);
}

int main(void) {
    test_struct_layout();
    test_memory_layout_invariants();
    test_scale_total_level();
    test_seek_lower_bound();
    test_seek_lower_bound_edge_cases();
    puts("PASS: runtime tests");
    return 0;
}
