#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <afx/driver.h>
#include <afx/host.h>
#include <afx/aica_channel.h>

/**
 * AICA Flow Information Tool (C23)
 * Inspects and summarizes .aicaflow (AICA Flow) files.
 */

typedef struct {
    uint8_t reg;
    const char *name;
    uint32_t count;
} reg_stat_t;

static const char *sample_format_name(uint32_t format) {
    switch (format) {
        case 0: return "PCM16";
        case 1: return "PCM8";
        case 2: return "UNKNOWN2";
        case 3: return "ADPCM";
        default: return "UNKNOWN";
    }
}

// Use 0xFE for KYON and 0xFD for KYOFF pseudo-registers
reg_stat_t stats[] = {
    { 0xFE,             "KYON",     0 },
    { 0xFD,             "KYOFF",    0 },
    { AICA_REG_SA_HI,   "SA_HI/CTL", 0 },
    { AICA_REG_SA_LO,   "SA_LO",    0 },
    { AICA_REG_LSA,      "LSA",      0 },
    { AICA_REG_LEA,      "LEA",      0 },
    { AICA_REG_ENV_AD, "ENV_AD",  0 },
    { AICA_REG_ENV_DR,  "ENV_DR",   0 },
    { AICA_REG_PITCH, "PITCH",  0 },
    { AICA_REG_LFO, "LFO",  0 },
    { AICA_REG_TOT_LVL, "TOT_LVL",  0 },
    { AICA_REG_PAN_VOL, "PAN_VOL",  0 },
    { 0xFF,             "OTHER",    0 }
};

typedef struct {
    uint32_t ts;
    uint8_t slot;
    uint8_t offset;
    uint8_t length;
    uint16_t values[32];
} flow_cmd_t;

static int flow_cmd_cmp(const void *a, const void *b) {
    const flow_cmd_t *ca = (const flow_cmd_t *)a;
    const flow_cmd_t *cb = (const flow_cmd_t *)b;
    if (ca->offset != cb->offset) return (int)ca->offset - (int)cb->offset;
    if (ca->length != cb->length) return (int)ca->length - (int)cb->length;
    if (ca->length == 0) return 0;
    return memcmp(ca->values, cb->values, ca->length * sizeof(uint16_t));
}

static bool decode_slot_payload(uint8_t offset, uint8_t length,
                                const uint16_t *values,
                                aica_chnl_packed_t *out) {
    if (offset != AICA_REG_SA_HI || length == 0 || !values || !out) {
        return false;
    }

    const uint8_t max_words = (uint8_t)(sizeof(*out) / sizeof(uint16_t));
    uint8_t copy_words = (length < max_words) ? length : max_words;
    memset(out, 0, sizeof(*out));
    memcpy(out, values, (size_t)copy_words * sizeof(uint16_t));
    return true;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: %s <file.aicaflow> [wavetables.map]\n", argv[0]);
        return 1;
    }

    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("Failed to open file"); return 1; }

    fseek(f, 0, SEEK_END);
    long total_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    afx_header_t head;
    if (fread(&head, 1, sizeof(head), f) != sizeof(head)) {
        fprintf(stderr, "Error: Could not read header.\n");
        fclose(f);
        return 1;
    }
    if (head.magic != AICAF_MAGIC) {
        fprintf(stderr, "Error: Invalid AICA Flow Magic (Found 0x%08X)\n", head.magic);
        fclose(f);
        return 1;
    }
    if (head.version != AICAF_VERSION) {
        fprintf(stderr, "Error: Unsupported format version %u (expected %u).\n",
                head.version, AICAF_VERSION);
        fclose(f);
        return 1;
    }

    if (head.section_count == 0) {
        fprintf(stderr, "Error: Missing section table.\n");
        fclose(f);
        return 1;
    }

    uint32_t table_size = head.section_count * sizeof(afx_section_entry_t);
    afx_section_entry_t *sections = malloc(table_size);
    if (!sections) {
        fprintf(stderr, "Error: Out of memory reading section table.\n");
        fclose(f);
        return 1;
    }
    fseek(f, sizeof(afx_header_t), SEEK_SET);
    if (fread(sections, 1, table_size, f) != table_size) {
        fprintf(stderr, "Error: Could not read section table.\n");
        free(sections);
        fclose(f);
        return 1;
    }

    const afx_section_entry_t *s_flow = NULL;
    const afx_section_entry_t *s_sdes = NULL;
    const afx_section_entry_t *s_sdat = NULL;

    for (uint32_t i = 0; i < head.section_count; i++) {
        if (sections[i].id == AFX_SECT_FLOW) s_flow = &sections[i];
        if (sections[i].id == AFX_SECT_SDES) s_sdes = &sections[i];
        if (sections[i].id == AFX_SECT_SDAT) s_sdat = &sections[i];
    }

    if (!s_flow || !s_sdes || !s_sdat) {
        fprintf(stderr, "Error: Missing required section(s).\n");
        free(sections);
        fclose(f);
        return 1;
    }

    uint32_t sample_data_off  = s_sdat->offset;
    uint32_t sample_data_size = s_sdat->size;
    uint32_t source_map_off   = s_sdes->offset;
    uint32_t source_map_count = s_sdes->count;
    uint32_t flow_data_off    = s_flow->offset;
    uint32_t flow_data_size   = s_flow->size;
    uint32_t total_ticks      = head.total_ticks;
    uint32_t stored_required_channels = head.required_channels;

    uint32_t flow_cmd_count = s_flow->count;
    double sample_pct = (double)sample_data_size / total_size * 100.0;
    double flow_cmd_pct = (double)flow_data_size / total_size * 100.0;

    flow_cmd_t *flow_cmds = NULL;
    uint32_t flow_cmds_found = 0;
    bool dedup_enabled = false;

    if (flow_cmd_count > 0) {
        flow_cmds = malloc(flow_cmd_count * sizeof(flow_cmd_t));
        if (flow_cmds) {
            dedup_enabled = true;
        } else {
            fprintf(stderr, "Warning: Could not allocate memory for flow command deduplication; skipping this analysis.\n");
        }
    }

    uint32_t total_sec = total_ticks / 1000;
    uint32_t mm = total_sec / 60;
    uint32_t ss = total_sec % 60;

    printf("=== AICA Flow File Info: %s ===\n", argv[1]);
    printf("File Size:  %ld bytes\n", total_size);
    printf("Song Length: %02u:%02u\n", mm, ss);
    printf("Magic:      0x%08X\n", head.magic);
    printf("Version:    %u\n", head.version);
    printf("Sections:   %u\n", head.section_count);
    printf("FLOW Offset: 0x%X (%u)\n", flow_data_off, flow_data_off);
    printf("SDES Offset: 0x%X (%u)\n", source_map_off, source_map_off);
    printf("SDAT Offset: 0x%X (%u)\n", sample_data_off, sample_data_off);
    printf("Samples:    %u bytes (%.1f%%) (at offset 0x%X)\n", sample_data_size, sample_pct, sample_data_off);
    printf("Flow Data:  %u bytes (%.1f%%) (at offset 0x%X)\n", flow_data_size, flow_cmd_pct, flow_data_off);
    printf("Flow Cmds:  %u entries\n", flow_cmd_count);
    printf("Required Channels: %u\n", stored_required_channels);
    printf("--------------------------------------\n");

    // Scan flow commands to gather register usage stats and correlate sample address usage with sample descriptors
    uint32_t slot_addr[64] = {0};
    bool slot_has_addr[64] = {0};
    bool active_slots[64] = {0};
    uint32_t active_slot_count = 0;
    uint32_t derived_required_channels = 0;
    uint32_t *addr_counts = NULL;
    int32_t *sample_formats = NULL;
        afx_sample_desc_t *descs = NULL;
        if (source_map_count > 0) {
            addr_counts    = calloc(source_map_count, sizeof(uint32_t));
            sample_formats = malloc(source_map_count * sizeof(int32_t));
            descs          = malloc(source_map_count * sizeof(afx_sample_desc_t));
            if (sample_formats)
                for (uint32_t i = 0; i < source_map_count; i++) sample_formats[i] = -1;
            if (descs) {
                fseek(f, source_map_off, SEEK_SET);
                if (fread(descs, sizeof(afx_sample_desc_t), source_map_count, f) != source_map_count) {
                    free(descs);
                    descs = NULL;
                }
            }
        }

        if (flow_data_size > 0) {
            fseek(f, flow_data_off, SEEK_SET);
            uint32_t bytes_read = 0;
            while (bytes_read < flow_data_size) {
                uint32_t ts;
                uint16_t bits;
                if (fread(&ts, 4, 1, f) != 1) break;
                bytes_read += 4;
                if (fread(&bits, 2, 1, f) != 1) break;
                bytes_read += 2;
                
                uint8_t slot = bits & 0x3F;
                uint8_t offset = (bits >> 6) & 0x1F;
                uint8_t length = (bits >> 11) & 0x1F;

                uint16_t values[32] = {0};
                uint8_t values_read = 0;
                for (; values_read < length; values_read++) {
                    if (fread(&values[values_read], 2, 1, f) != 1) break;
                    bytes_read += 2;
                }

                aica_chnl_packed_t payload;
                bool has_payload = decode_slot_payload(offset, values_read, values, &payload);

                if (dedup_enabled && flow_cmds_found < flow_cmd_count) {
                    flow_cmd_t *cmd = &flow_cmds[flow_cmds_found++];
                    cmd->ts = ts;
                    cmd->slot = slot;
                    cmd->offset = offset;
                    cmd->length = values_read;
                    for (uint8_t vi = 0; vi < values_read; vi++) cmd->values[vi] = values[vi];
                }

                for (uint8_t l = 0; l < values_read; l++) {
                    uint16_t val = values[l];
                    uint8_t reg = (uint8_t)(offset + l);

                    bool found = false;
                    if (reg == AICA_REG_SA_HI) {
                        uint16_t ctl = has_payload ? payload.play_ctrl.raw : val;
                        uint32_t control_bits = ctl & ((1u << 15) | (1u << 14));
                        if (control_bits & (1u << 15)) {
                            if (!active_slots[slot]) {
                                active_slots[slot] = true;
                                active_slot_count++;
                                if (active_slot_count > derived_required_channels) {
                                    derived_required_channels = active_slot_count;
                                }
                            }
                            stats[0].count++; /* KYON */
                        }
                        if (control_bits & (1u << 14)) {
                            if (active_slots[slot]) {
                                active_slots[slot] = false;
                                if (active_slot_count > 0) active_slot_count--;
                            }
                            stats[1].count++; /* KYOFF */
                        }
                        stats[2].count++; /* Count the register write itself */
                        found = true;
                    } else {
                        if (reg == AICA_REG_SA_LO) {
                            uint16_t sa_lo = has_payload ? payload.sa_low : val;
                            slot_addr[slot] = sa_lo;
                            slot_has_addr[slot] = true;
                        }
                        for (int j = 3; stats[j].reg != 0xFF; j++) {
                            if (stats[j].reg == reg) { stats[j].count++; found = true; break; }
                        }
                    }
                    if (!found) {
                        stats[13].count++; /* Index 13 is "OTHER" */
                    }
                }

                if (values_read < length) break;
                
                uint32_t cmd_size = 6 + (length * 2);
                uint32_t padded_size = (cmd_size + 3) & ~3;
                uint32_t pad_bytes = padded_size - cmd_size;
                
                if (pad_bytes > 0) {
                    uint16_t pad;
                    if (fread(&pad, pad_bytes, 1, f) == 1) {
                        bytes_read += pad_bytes;
                    }
                }
            }
        }
        printf("Required Channels: %u (derived)\n", derived_required_channels);
        printf("Flow Command Register Distribution:\n");
        for (int i = 0; i < 14; i++) {
            if (stats[i].count > 0) {
                if (stats[i].reg != 0xFF && stats[i].reg != 0xFE && stats[i].reg != 0xFD)
                    printf("  [0x%02X] %-10s : %u\n", stats[i].reg, stats[i].name, stats[i].count);
                else
                    printf("  [----] %-10s : %u\n", stats[i].name, stats[i].count);
            }
        }

        if (dedup_enabled && flow_cmds_found > 1) {
            qsort(flow_cmds, flow_cmds_found, sizeof(*flow_cmds), flow_cmd_cmp);
            uint32_t reuse_groups = 0;
            uint32_t total_reused_cmds = 0;
            for (uint32_t i = 0; i < flow_cmds_found; ) {
                uint32_t j = i + 1;
                while (j < flow_cmds_found && flow_cmd_cmp(&flow_cmds[i], &flow_cmds[j]) == 0) {
                    j++;
                }
                uint32_t group_size = j - i;
                if (group_size > 1 && flow_cmds[i].length > 1) {
                    if (reuse_groups == 0) {
                        printf("\nPotential Duplicate Commands (timing/channel ignored):\n");
                    }
                    reuse_groups++;
                    total_reused_cmds += group_size;
                    printf("  Group %u: offset=0x%02X length=%u repeats=%u\n", reuse_groups, flow_cmds[i].offset, flow_cmds[i].length, group_size);
                    for (uint32_t k = i; k < j && k < i + 5; k++) {
                        printf("    instance [%u] slot=%u ts=%u\n", k - i + 1, flow_cmds[k].slot, flow_cmds[k].ts);
                    }
                    if (group_size > 5) {
                        printf("    ...+%u more instances\n", group_size - 5);
                    }
                }
                i = j;
            }
            if (reuse_groups == 0) {
                printf("\nNo command duplicates found when ignoring timing and channel.\n");
            } else {
                printf("\nFound %u duplicate groups, %u total commands are reusable candidates.\n", reuse_groups, total_reused_cmds);
            }
        } else if (dedup_enabled) {
            printf("\nNot enough commands to evaluate duplicate patterns.\n");
        }

        if (flow_cmds) {
            free(flow_cmds);
            flow_cmds = NULL;
        }

        if (source_map_count > 0 && descs) {
            printf("\nSample Descriptors (%u unique samples):\n", source_map_count);
            FILE *f_map = (argc >= 3) ? fopen(argv[2], "rb") : NULL;
            for (uint32_t i = 0; i < source_map_count; i++) {
                const char *fmt_name = sample_format_name(descs[i].format);
                const char *loop_name = (descs[i].loop_mode == AFX_LOOP_NONE)  ? "no loop" :
                                        (descs[i].loop_mode == AFX_LOOP_FWD)   ? "loop-fwd" : "loop-bidir";
                char filename[256] = "Unknown";
                if (f_map) {
                    fseek(f_map, 0, SEEK_SET);
                    char line[512]; uint32_t cur_id = 0;
                    while (fgets(line, sizeof(line), f_map)) {
                        if (strstr(line, "\"id\":")) sscanf(line, " \"id\": %u,", &cur_id);
                        else if (cur_id == descs[i].source_id && strstr(line, "\"filename\":")) {
                            char *s = strstr(line, "\"filename\":") + 11;
                            s = strchr(s, '"'); if (s) { s++; char *e = strchr(s, '"'); if (e) { *e='\0'; strncpy(filename, s, 255); } }
                            break;
                        } else if (cur_id == descs[i].source_id && strchr(line, '}')) break;
                    }
                }
                uint32_t off_file = descs[i].sample_off;
                uint32_t off_sdat = (off_file >= sample_data_off) ? (off_file - sample_data_off) : 0;
                printf("  [%u] Prog %3u  src 0x%08X  off(file) 0x%06X  off(sdat) 0x%06X  %u bytes  %u Hz  root %d  %s  %s  [%s]  [Used %u times]\n",
                    i, descs[i].gm_program, descs[i].source_id,
                    off_file, off_sdat, descs[i].sample_size, descs[i].sample_rate,
                    descs[i].root_note, fmt_name, loop_name, filename,
                    addr_counts ? addr_counts[i] : 0);
            }
            if (f_map) fclose(f_map);
            free(descs);
        }
        if (addr_counts)    free(addr_counts);
        if (sample_formats) free(sample_formats);
        free(sections);
        fclose(f);
        return 0;
    }