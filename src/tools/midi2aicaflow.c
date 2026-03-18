#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <dirent.h>
#include <ctype.h>
#include <aicaflow/aicaflow.h>

/**
 * AICA Flow Integrated Tool (C23)
 * Automatic Waveform Selection & MIDI Packing.
 */

typedef struct {
    uint32_t addr;
    uint32_t size;
    uint8_t  format; 
} patch_info_t;

patch_info_t instrument_bank[128];
const char *prog_names[128] = {
    [0] = "Piano", [1] = "Rhodes", [6] = "Organ", [11] = "Music Box", [12] = "Vibra",
    [13] = "Marimba", [19] = "Organ", [21] = "Accordion", [24] = "Guitar Acoustic",
    [25] = "Nylon Guitar", [26] = "Electric Guitar", [32] = "Acoustic Bass", 
    [34] = "BS Elect", [40] = "Violin", [42] = "Cello", [45] = "Pizzo String", 
    [46] = "Harp", [48] = "String", [56] = "Trumpet", [57] = "Trombone", [60] = "French Horn",
    [61] = "Brass", [65] = "Saxaphone", [68] = "Clarinet", [71] = "Clarinet", [73] = "Flute",
    [104] = "Sitar", [105] = "Banjo", [114] = "Steel Drum"
};

[[nodiscard]] static uint32_t read_varlen(FILE *f) {
    uint32_t value = 0;
    uint8_t byte;
    do {
        int b = fgetc(f);
        if (b == EOF) break;
        byte = (uint8_t)b;
        value = (value << 7) | (byte & 0x7F);
    } while (byte & 0x80);
    return value;
}

static char* find_sample_in_dir(const char *dir_path, const char *keyword) {
    if (!dir_path || !keyword) return NULL;
    DIR *d = opendir(dir_path);
    if (!d) return NULL;
    struct dirent *dir;
    char *found = NULL;
    while ((dir = readdir(d)) != NULL) {
        if (dir->d_type == DT_REG && strstr(dir->d_name, ".wav")) {
            char name_lower[256] = {0}, key_lower[256] = {0};
            for(int i=0; dir->d_name[i] && i < 255; i++) name_lower[i] = tolower((unsigned char)dir->d_name[i]);
            for(int i=0; keyword[i] && i < 255; i++) key_lower[i] = tolower((unsigned char)keyword[i]);
            if (strstr(name_lower, key_lower)) {
                found = malloc(512);
                snprintf(found, 512, "%s/%s", dir_path, dir->d_name);
                break;
            }
        }
    }
    closedir(d);
    return found;
}

static void pack_file(const char *path, uint8_t program, FILE *f_out, uint32_t *offset) {
    FILE *f_wav = fopen(path, "rb");
    if (!f_wav) return;
    
    char chunk_id[4];
    uint32_t data_size = 0;
    fseek(f_wav, 12, SEEK_SET); 
    while (fread(chunk_id, 1, 4, f_wav) == 4) {
        uint32_t chunk_sz;
        fread(&chunk_sz, 4, 1, f_wav);
        if (memcmp(chunk_id, "data", 4) == 0) {
            data_size = chunk_sz;
            break;
        }
        fseek(f_wav, chunk_sz, SEEK_CUR);
    }

    if (data_size > 0) {
        uint32_t padding = (4 - (*offset % 4)) % 4;
        for(uint32_t i=0; i<padding; i++) fputc(0, f_out);
        *offset += padding;

        instrument_bank[program] = (patch_info_t){ .addr = *offset, .size = data_size, .format = 1 };
        uint8_t *buf = malloc(data_size);
        if (buf) {
            fread(buf, 1, data_size, f_wav);
            fwrite(buf, 1, data_size, f_out);
            *offset += data_size;
            free(buf);
            printf("Mapped Program %d -> %s (%u bytes)\n", program, path, data_size);
        }
    }
    fclose(f_wav);
}

static void write_patch_opcodes(FILE *f_out, uint32_t timestamp, uint8_t slot, uint8_t program, uint8_t ar, uint8_t rr, afx_opcode_t *ops, uint32_t *op_idx) {
    patch_info_t p = instrument_bank[program];
    if (p.size == 0) return; 
    ops[(*op_idx)++] = (afx_opcode_t){ timestamp, slot, AICA_REG_SA_HI, 0, (p.addr >> 16) & 0xFF };
    ops[(*op_idx)++] = (afx_opcode_t){ timestamp, slot, AICA_REG_SA_LO, 0, p.addr & 0xFFFF };
    ops[(*op_idx)++] = (afx_opcode_t){ timestamp, slot, AICA_REG_AR_SR, 0, (ar << 8) | 0 };
    ops[(*op_idx)++] = (afx_opcode_t){ timestamp, slot, AICA_REG_EGH_RR, 0, rr };
}

int cmp_opcodes(const void *a, const void *b) {
    afx_opcode_t *oa = (afx_opcode_t *)a;
    afx_opcode_t *ob = (afx_opcode_t *)b;
    if (oa->timestamp < ob->timestamp) return -1;
    if (oa->timestamp > ob->timestamp) return 1;
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 4) {
        printf("Usage: %s <output.aicaflow> <input.mid> <wav_dir>\n", argv[0]);
        return 1;
    }

    FILE *f_mid = fopen(argv[2], "rb");
    if (!f_mid) { perror("MIDI open failed"); return 1; }

    FILE *f_out = fopen(argv[1], "wb");
    if (!f_out) { fclose(f_mid); perror("Output open failed"); return 1; }

    afx_header_t header = { .magic = AICAF_MAGIC, .version = 1 };
    fwrite(&header, sizeof(header), 1, f_out);

    bool needed[128] = {0};
    fseek(f_mid, 14, SEEK_SET);
    char chunk[4];
    while(fread(chunk, 1, 4, f_mid) == 4) {
        uint32_t len;
        fread(&len, 4, 1, f_mid);
        len = __builtin_bswap32(len);
        if(memcmp(chunk, "MTrk", 4) == 0) {
            long track_end = ftell(f_mid) + len;
            while(ftell(f_mid) < track_end) {
                read_varlen(f_mid); 
                int status = fgetc(f_mid);
                if (status == EOF) break;
                if (status == 0xFF) { fgetc(f_mid); uint32_t mlen = read_varlen(f_mid); fseek(f_mid, mlen, SEEK_CUR); }
                else if (status == 0xF0 || status == 0xF7) { uint32_t mlen = read_varlen(f_mid); fseek(f_mid, mlen, SEEK_CUR); }
                else {
                    uint8_t type = status & 0xF0;
                    if (type == 0xC0) { uint8_t p = fgetc(f_mid); if(p < 128) needed[p] = true; }
                    else if (type == 0x80 || type == 0x90 || type == 0xA0 || type == 0xB0 || type == 0xE0) { fgetc(f_mid); fgetc(f_mid); }
                    else if (type == 0xD0) { fgetc(f_mid); }
                }
            }
        } else fseek(f_mid, len, SEEK_CUR);
    }
    needed[0] = true;

    uint32_t offset = 0;
    for(int i=0; i<128; i++) {
        if(needed[i] && prog_names[i]) {
            char *path = find_sample_in_dir(argv[3], prog_names[i]);
            if(path) { pack_file(path, i, f_out, &offset); free(path); }
        }
    }
    header.sample_data_off = sizeof(header);
    header.sample_data_size = offset;

    fseek(f_mid, 14, SEEK_SET);
    header.stream_data_off = header.sample_data_off + header.sample_data_size;
    
    // Support large MIDI files by buffering opcodes before sorting
    afx_opcode_t *op_buffer = malloc(sizeof(afx_opcode_t) * 1000000);
    uint32_t op_count = 0;
    
    uint16_t ppqn = 96;
    fseek(f_mid, 12, SEEK_SET);
    uint8_t d1 = fgetc(f_mid), d2 = fgetc(f_mid);
    if (!(d1 & 0x80)) ppqn = (d1 << 8) | d2;

    fseek(f_mid, 14, SEEK_SET);
    uint32_t max_timestamp = 0;

    while(fread(chunk, 1, 4, f_mid) == 4) {
        uint32_t len; fread(&len, 4, 1, f_mid); len = __builtin_bswap32(len);
        if(memcmp(chunk, "MTrk", 4) == 0) {
            long track_end = ftell(f_mid) + len;
            uint32_t current_ticks = 0;
            uint32_t current_ms = 0;
            uint32_t tempo_us_pqn = 500000; // 120 BPM default

            uint8_t running_status = 0;
            uint8_t channel_progs[16] = {0};
            uint8_t channel_attack[16], channel_release[16];
            for (int i=0; i<16; i++) { channel_attack[i] = 31; channel_release[i] = 15; }

            while(ftell(f_mid) < track_end) {
                uint32_t delta = read_varlen(f_mid);
                current_ticks += delta;
                
                // Update timestamp based on current tempo
                current_ms += (delta * tempo_us_pqn) / (ppqn * 1000);
                if (current_ms > max_timestamp) max_timestamp = current_ms;

                int status = fgetc(f_mid);
                if(status == EOF) break;
                if(!(status & 0x80)) { fseek(f_mid, -1, SEEK_CUR); status = running_status; }
                else running_status = (uint8_t)status;

                uint8_t type = status & 0xF0, chan = status & 0x0F;
                if(type == 0xC0) channel_progs[chan] = fgetc(f_mid);
                else if(type == 0xB0) {
                    uint8_t cc = fgetc(f_mid), val = fgetc(f_mid);
                    if (cc == 73) channel_attack[chan] = (val * 31) / 127;
                    else if (cc == 72) channel_release[chan] = (val * 31) / 127;
                }
                else if(type == 0x90 || type == 0x80) {
                    uint8_t note = fgetc(f_mid), vel = fgetc(f_mid);
                    if(type == 0x90 && vel > 0) {
                        write_patch_opcodes(f_out, current_ms, chan, channel_progs[chan], channel_attack[chan], channel_release[chan], op_buffer, &op_count);
                        op_buffer[op_count++] = (afx_opcode_t){ current_ms, chan, AICA_REG_FNS_OCT, 0, aica_pitch_convert(midi_note_to_freq(note)) };
                        op_buffer[op_count++] = (afx_opcode_t){ current_ms, chan, AICA_REG_CTL, 0, (1 << 14) };
                    } else {
                        op_buffer[op_count++] = (afx_opcode_t){ current_ms, chan, AICA_REG_CTL, 0, 0 };
                    }
                } else if(status < 0xF0) { fgetc(f_mid); if(type != 0xD0) fgetc(f_mid); }
                else if(status == 0xFF) { 
                    uint8_t meta_type = fgetc(f_mid);
                    uint32_t mlen = read_varlen(f_mid); 
                    if (meta_type == 0x51 && mlen == 3) {
                        uint32_t t1 = fgetc(f_mid), t2 = fgetc(f_mid), t3 = fgetc(f_mid);
                        tempo_us_pqn = (t1 << 16) | (t2 << 8) | t3;
                    } else {
                        fseek(f_mid, mlen, SEEK_CUR); 
                    }
                }
                else { uint32_t mlen = read_varlen(f_mid); fseek(f_mid, mlen, SEEK_CUR); }
            }
        } else fseek(f_mid, len, SEEK_CUR);
    }

    qsort(op_buffer, op_count, sizeof(afx_opcode_t), cmp_opcodes);
    fwrite(op_buffer, sizeof(afx_opcode_t), op_count, f_out);
    free(op_buffer);

    header.stream_data_size = op_count;
    header.total_ticks = max_timestamp;
    fseek(f_out, 0, SEEK_SET);
    fwrite(&header, sizeof(header), 1, f_out);
    fclose(f_mid); fclose(f_out);
    printf("Success: Generated %s with %u opcodes. Duration: %ums\n", argv[1], op_count, max_timestamp);
    return 0;
}