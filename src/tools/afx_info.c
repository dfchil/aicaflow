#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <afx/driver.h>
#include <afx/host.h>

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
    { AICA_REG_D2R_D1R, "D2R_D1R",  0 },
    { AICA_REG_EGH_RR,  "EGH_RR",   0 },
    { AICA_REG_AR_SR,   "AR_SR",    0 },
    { AICA_REG_FNS_OCT, "FNS_OCT",  0 },
    { AICA_REG_TOT_LVL, "TOT_LVL",  0 },
    { AICA_REG_PAN_VOL, "PAN_VOL",  0 },
    { 0xFF,             "OTHER",    0 }
};

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
    uint32_t flow_data_size   = s_flow->count;
    uint32_t total_ticks      = head.total_ticks;

    uint32_t flow_cmd_bytes = flow_data_size * sizeof(afx_cmd_t);
    double sample_pct = (double)sample_data_size / total_size * 100.0;
    double flow_cmd_pct = (double)flow_cmd_bytes / total_size * 100.0;

    uint32_t total_sec = total_ticks / 1000;
    uint32_t mm = total_sec / 60;
    uint32_t ss = total_sec % 60;

    printf("=== AICA Flow File Info: %s ===\n", argv[1]);
    printf("File Size:  %ld bytes\n", total_size);
    printf("Song Length: %02u:%02u\n", mm, ss);
    printf("Magic:      0x%08X\n", head.magic);
    printf("Version:    %u\n", head.version);
    printf("Sections:   %u\n", head.section_count);
    printf("FLOW Offset: 0x%02X (%u)\n", flow_data_off, flow_data_off);
    printf("SDES Offset: 0x%02X (%u)\n", source_map_off, source_map_off);
    printf("SDAT Offset: 0x%02X (%u)\n", sample_data_off, sample_data_off);
    printf("Samples:    %u bytes (%.1f%%) (at offset 0x%X)\n", sample_data_size, sample_pct, sample_data_off);
    printf("Flow Cmds:  %u entries, %u bytes (%.1f%%) (at offset 0x%X)\n", flow_data_size, flow_cmd_bytes, flow_cmd_pct, flow_data_off);
    printf("--------------------------------------\n");

    // Scan flow commands to gather register usage stats and correlate sample address usage with sample descriptors
    uint32_t slot_addr[64] = {0};
    bool slot_has_addr[64] = {0};
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
            for (uint32_t i = 0; i < flow_data_size; i++) {
                afx_cmd_t cmd;
                if (fread(&cmd, 1, sizeof(cmd), f) != sizeof(cmd)) break;

                /* SA_HI and SA_LO both store the full file-relative address */
                if (cmd.reg == AICA_REG_SA_LO) {
                    slot_addr[cmd.slot] = cmd.value;
                    slot_has_addr[cmd.slot] = true;
                }

                bool found = false;
                /* Note: In the new architecture, Key On/Off is handled by AICA_REG_SA_HI (Index 0)
                   Bit 15: KYON, Bit 14: KYOFF */
                if (cmd.reg == AICA_REG_SA_HI) {
                    uint32_t control_bits = cmd.value & ((1u << 15) | (1u << 14));
                    if (control_bits & (1u << 15)) {
                        stats[0].count++; /* KYON */
                        found = true;
                        if (descs && addr_counts && slot_has_addr[cmd.slot]) {
                            for (uint32_t s = 0; s < source_map_count; s++) {
                                if (descs[s].sample_off == slot_addr[cmd.slot]) {
                                    addr_counts[s]++;
                                    if (sample_formats && sample_formats[s] < 0)
                                        sample_formats[s] = (int32_t)descs[s].format;
                                    break;
                                }
                            }
                        }
                    } else if (control_bits & (1u << 14)) {
                        stats[1].count++; /* KYOFF */
                        found = true;
                    }
                    /* Always count the SA_HI/CTL register write itself in stats[2] */
                    stats[2].count++;
                } else {
                    for (int j = 3; stats[j].reg != 0xFF; j++) {
                        if (stats[j].reg == cmd.reg) { stats[j].count++; found = true; break; }
                    }
                }
                if (!found && cmd.reg != AICA_REG_SA_HI) stats[12].count++; /* Index 12 is "OTHER" */
            }
        }

        printf("Flow Command Register Distribution:\n");
        for (int i = 0; i < 13; i++) {
            if (stats[i].count > 0) {
                if (stats[i].reg != 0xFF && stats[i].reg != 0xFE && stats[i].reg != 0xFD)
                    printf("  [0x%02X] %-10s : %u\n", stats[i].reg, stats[i].name, stats[i].count);
                else
                    printf("  [----] %-10s : %u\n", stats[i].name, stats[i].count);
            }
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