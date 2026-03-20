#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <afx/afx.h>

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
    { AICA_REG_CTL,     "CTL",      0 },
    { AICA_REG_SA_HI,   "SA_HI",    0 },
    { AICA_REG_SA_LO,   "SA_LO",    0 },
    { AICA_REG_LSA_HI,  "LSA_HI",   0 },
    { AICA_REG_LSA_LO,  "LSA_LO",   0 },
    { AICA_REG_LEA_HI,  "LEA_HI",   0 },
    { AICA_REG_LEA_LO,  "LEA_LO",   0 },
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

    /* --- Read and validate header --- */
    uint32_t magic = 0, version = 0;
    fread(&magic, 4, 1, f);
    fread(&version, 4, 1, f);
    fseek(f, 0, SEEK_SET);

    if (magic != AICAF_MAGIC) {
        fprintf(stderr, "Error: Invalid AICA Flow Magic (Found 0x%08X)\n", magic);
        fclose(f); return 1;
    }

    afx_header_t head;
    if (fread(&head, 1, sizeof(head), f) != sizeof(head)) {
        fprintf(stderr, "Error: Could not read header.\n"); fclose(f); return 1;
    }

    uint32_t sample_data_off  = head.sample_data_off;
    uint32_t sample_data_size = head.sample_data_size;
    uint32_t source_map_off   = head.sample_desc_off;
    uint32_t source_map_count = head.sample_desc_count;
    uint32_t flow_data_off  = head.flow_data_off;
    uint32_t flow_data_size = head.flow_data_size;
    uint32_t total_ticks      = head.total_ticks;

    uint32_t flow_cmd_bytes = flow_data_size * sizeof(afx_flow_cmd_t);
    double sample_pct = (double)sample_data_size / total_size * 100.0;
    double flow_cmd_pct = (double)flow_cmd_bytes / total_size * 100.0;

    uint32_t total_sec = total_ticks / 1000;
    uint32_t mm = total_sec / 60;
    uint32_t ss = total_sec % 60;

    printf("=== AICA Flow File Info: %s ===\n", argv[1]);
    printf("File Size:  %ld bytes\n", total_size);
    printf("Song Length: %02u:%02u\n", mm, ss);
    printf("Magic:      0x%08X\n", magic);
    printf("Version:    %u\n", version);
    printf("Samples:    %u bytes (%.1f%%) (at offset 0x%X)\n", sample_data_size, sample_pct, sample_data_off);
    printf("Flow Cmds:  %u entries, %u bytes (%.1f%%) (at offset 0x%X)\n", flow_data_size, flow_cmd_bytes, flow_cmd_pct, flow_data_off);
    printf("--------------------------------------\n");

    // Scan Opcodes
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
            fseek(f, source_map_off, SEEK_SET);
            fread(descs, sizeof(afx_sample_desc_t), source_map_count, f);
        }

        if (flow_data_size > 0) {
            fseek(f, flow_data_off, SEEK_SET);
            for (uint32_t i = 0; i < flow_data_size; i++) {
                afx_flow_cmd_t cmd;
                if (fread(&cmd, 1, sizeof(cmd), f) != sizeof(cmd)) break;

                /* In v2, SA_HI and SA_LO both store the full blob-local address */
                if (cmd.reg == AICA_REG_SA_LO) {
                    slot_addr[cmd.slot] = cmd.value;
                    slot_has_addr[cmd.slot] = true;
                }

                bool found = false;
                if (cmd.reg == AICA_REG_CTL) {
                    uint32_t control_bits = cmd.value & ((1 << 15) | (1 << 14));
                    if (control_bits == (1 << 15)) {
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
                    } else if (control_bits == (1 << 14)) {
                        stats[1].count++; found = true; /* KYOFF */
                    } else {
                        stats[2].count++; found = true; /* CTL other */
                    }
                } else {
                    for (int j = 3; stats[j].reg != 0xFF; j++) {
                        if (stats[j].reg == cmd.reg) { stats[j].count++; found = true; break; }
                    }
                }
                if (!found) stats[15].count++;
            }
        }

        printf("Opcode Register Distribution:\n");
        for (int i = 0; i < 16; i++) {
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
                printf("  [%u] Prog %3u  src 0x%08X  off 0x%06X  %u bytes  %u Hz  root %d  %s  %s  [%s]  [Used %u times]\n",
                    i, descs[i].gm_program, descs[i].source_id,
                    descs[i].sample_off, descs[i].sample_size, descs[i].sample_rate,
                    descs[i].root_note, fmt_name, loop_name, filename,
                    addr_counts ? addr_counts[i] : 0);
            }
            if (f_map) fclose(f_map);
            free(descs);
        }
        if (addr_counts)    free(addr_counts);
        if (sample_formats) free(sample_formats);
        fclose(f);
        return 0;
    }