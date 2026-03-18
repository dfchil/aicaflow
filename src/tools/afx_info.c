#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <aicaflow/aicaflow.h>

/**
 * AICA Flow Information Tool (C23)
 * Inspects and summarizes .aicaflow (AICA Flow) files.
 */

typedef struct {
    uint8_t reg;
    const char *name;
    uint32_t count;
} reg_stat_t;

reg_stat_t stats[] = {
    { AICA_REG_CTL,     "CTL/KYON", 0 },
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
        printf("Usage: %s <file.aicaflow>\n", argv[0]);
        return 1;
    }

    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("Failed to open file"); return 1; }

    afx_header_t head;
    if (fread(&head, 1, sizeof(head), f) != sizeof(head)) {
        fprintf(stderr, "Error: Could not read header.\n");
        fclose(f); return 1;
    }

    if (head.magic != AICAF_MAGIC) {
        fprintf(stderr, "Error: Invalid AICA Flow Magic (Found 0x%08X)\n", head.magic);
        fclose(f); return 1;
    }

    printf("=== AICA Flow File Info: %s ===\n", argv[1]);
    printf("Magic:      0x%08X\n", head.magic);
    printf("Version:    %u\n", head.version);
    printf("Samples:    %u bytes (at offset 0x%X)\n", head.sample_data_size, head.sample_data_off);
    printf("Opcodes:    %u entries (at offset 0x%X)\n", head.stream_data_size, head.stream_data_off);
    printf("--------------------------------------\n");

    // Scan Opcodes
    if (head.stream_data_size > 0) {
        fseek(f, head.stream_data_off, SEEK_SET);
        for (uint32_t i = 0; i < head.stream_data_size; i++) {
            afx_opcode_t op;
            if (fread(&op, 1, sizeof(op), f) != sizeof(op)) break;
            
            bool found = false;
            for (int j = 0; stats[j].reg != 0xFF; j++) {
                if (stats[j].reg == op.reg) {
                    stats[j].count++;
                    found = true;
                    break;
                }
            }
            if (!found) stats[13].count++; // OTHER
        }

        printf("Opcode Register Distribution:\n");
        for (int i = 0; i < 14; i++) {
            if (stats[i].count > 0) {
                printf("  [0x%02X] %-10s : %u\n", stats[i].reg, stats[i].name, stats[i].count);
            }
        }
    }

    fclose(f);
    return 0;
}