#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <afx/driver.h>
#include <afx/host.h>
#include <math.h>
/**
 * AICA Flow Integrated Tool (C23)
 * Automatic Waveform Selection & MIDI Packing.
 */

uint32_t aica_get_reg_addr(uint8_t slot, uint8_t reg) {
    return (uint32_t)(0x00800000 + (slot * 0x80) + reg);
}

uint32_t aica_pitch_convert(float ratio) {
    float oct_f = log2f(ratio);
    int8_t oct = (int8_t)floorf(oct_f);
    
    float oct_pow = powf(2.0f, (float)oct);
    uint32_t fns = (uint32_t)((1024.0f * (ratio / oct_pow)) - 1024.0f);
    
    if (oct < -8) oct = -8;
    if (oct > 7)  oct = 7;
    if (fns > 1023) fns = 1023;

    return (uint32_t)(((oct & 0xF) << 11) | (fns & 0x3FF));
}

float midi_note_to_freq(uint8_t note) {
    return 440.0f * powf(2.0f, (float)(note - 69) / 12.0f);
}

typedef struct {
    uint32_t addr;
    uint32_t size;
    uint8_t  format; 
    uint32_t source_id;
} patch_info_t;

typedef struct {
    uint16_t note_trim_ms;
    uint16_t min_hold_ms;
    float velocity_gamma;
    float velocity_gain;
    int8_t release_bias;
} playback_policy_t;

patch_info_t instrument_bank[128];
afx_sample_desc_t sample_descs[128];
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
static int get_gm_family(int prog) {
    if (prog < 0 || prog > 127) return -1;
    return prog / 8;
}

static int clamp_int(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static uint8_t apply_velocity_policy(uint8_t velocity, const playback_policy_t *policy) {
    float norm = (float)velocity / 127.0f;
    float curved = powf(norm, policy->velocity_gamma) * policy->velocity_gain;
    int v = (int)lroundf(curved * 127.0f);
    return (uint8_t)clamp_int(v, 1, 127);
}

static playback_policy_t default_policy_from_family_name(const char *family) {
    playback_policy_t p = {0, 16, 1.0f, 1.0f, 0};
    if (!family) return p;

    if (strcmp(family, "keys_plucks") == 0) {
        p = (playback_policy_t){120, 24, 1.2f, 1.0f, -2};
    } else if (strcmp(family, "sustains_pads") == 0) {
        p = (playback_policy_t){0, 40, 0.9f, 1.0f, 3};
    } else if (strcmp(family, "basses") == 0) {
        p = (playback_policy_t){60, 30, 1.05f, 1.0f, 1};
    } else if (strcmp(family, "leads_winds") == 0) {
        p = (playback_policy_t){35, 20, 1.1f, 1.0f, -1};
    } else if (strcmp(family, "drums_percussion") == 0) {
        p = (playback_policy_t){180, 12, 0.95f, 1.05f, -4};
    } else if (strcmp(family, "fx_atmos") == 0) {
        p = (playback_policy_t){20, 30, 1.15f, 0.95f, 2};
    }
    return p;
}

static void init_neutral_policies(playback_policy_t policies[128]) {
    for (int i = 0; i < 128; i++) {
        policies[i] = (playback_policy_t){0, 16, 1.0f, 1.0f, 0};
    }
}

static bool parse_json_string_value(const char *line, char *out, size_t out_sz) {
    const char *colon = strchr(line, ':');
    if (!colon) return false;
    const char *start = strchr(colon, '"');
    if (!start) return false;
    start++;
    const char *end = strchr(start, '"');
    if (!end) return false;
    size_t n = (size_t)(end - start);
    if (n >= out_sz) n = out_sz - 1;
    memcpy(out, start, n);
    out[n] = '\0';
    return true;
}

static bool parse_json_int_value(const char *line, int *out) {
    const char *colon = strchr(line, ':');
    if (!colon) return false;
    while (*colon && (*colon == ':' || *colon == ' ' || *colon == '\t')) colon++;
    if (!*colon || *colon == 'n') return false;
    return sscanf(colon, "%d", out) == 1;
}

static bool parse_json_float_value(const char *line, float *out) {
    const char *colon = strchr(line, ':');
    if (!colon) return false;
    while (*colon && (*colon == ':' || *colon == ' ' || *colon == '\t')) colon++;
    if (!*colon || *colon == 'n') return false;
    return sscanf(colon, "%f", out) == 1;
}

static void load_program_policies_from_map(const char *map_path, playback_policy_t policies[128]) {
    FILE *f = fopen(map_path, "r");
    if (!f) return;

    char line[1024];
    int current_gm = -1;
    char current_family[64] = "";
    int trim_ms = -1;
    int min_hold_ms = -1;
    float velocity_gamma = -1.0f;
    float velocity_gain = -1.0f;
    int release_bias = 0;
    bool release_bias_seen = false;

    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "\"gm_idx\"")) {
            int gm = -1;
            if (parse_json_int_value(line, &gm)) current_gm = gm;
            else current_gm = -1;
        } else if (strstr(line, "\"patch_family\"")) {
            parse_json_string_value(line, current_family, sizeof(current_family));
        } else if (strstr(line, "\"policy_note_trim_ms\"")) {
            (void)parse_json_int_value(line, &trim_ms);
        } else if (strstr(line, "\"policy_min_hold_ms\"")) {
            (void)parse_json_int_value(line, &min_hold_ms);
        } else if (strstr(line, "\"policy_velocity_gamma\"")) {
            (void)parse_json_float_value(line, &velocity_gamma);
        } else if (strstr(line, "\"policy_velocity_gain\"")) {
            (void)parse_json_float_value(line, &velocity_gain);
        } else if (strstr(line, "\"policy_release_bias\"")) {
            if (parse_json_int_value(line, &release_bias)) release_bias_seen = true;
        }

        if (strchr(line, '}')) {
            if (current_gm >= 0 && current_gm < 128) {
                playback_policy_t p = default_policy_from_family_name(current_family);
                if (trim_ms >= 0) p.note_trim_ms = (uint16_t)trim_ms;
                if (min_hold_ms >= 0) p.min_hold_ms = (uint16_t)min_hold_ms;
                if (velocity_gamma > 0.0f) p.velocity_gamma = velocity_gamma;
                if (velocity_gain > 0.0f) p.velocity_gain = velocity_gain;
                if (release_bias_seen) p.release_bias = (int8_t)clamp_int(release_bias, -31, 31);
                policies[current_gm] = p;
            }

            current_gm = -1;
            current_family[0] = '\0';
            trim_ms = -1;
            min_hold_ms = -1;
            velocity_gamma = -1.0f;
            velocity_gain = -1.0f;
            release_bias = 0;
            release_bias_seen = false;
        }
    }
    fclose(f);
}

static char* find_sample_for_program(const char *map_path, uint8_t program, uint32_t *out_id) {
    if (!map_path) return NULL;
    FILE *f = fopen(map_path, "r");
    if (!f) return NULL;
    char line[1024];
    int current_gm = -1;
    uint32_t current_id = 0;
    
    char *best_path = NULL;
    uint32_t best_id = 0;
    int best_gm = -1;
    
    int target_family = get_gm_family(program);
    
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "\"gm_idx\":")) {
            char *p = strchr(line, ':');
            if (p) {
                if (strstr(p, "null")) current_gm = -1;
                else sscanf(p + 1, "%d", &current_gm);
            }
        }
        if (strstr(line, "\"id\":")) {
            char *p = strchr(line, ':');
            if (p) sscanf(p + 1, "%u", &current_id);
        }
        if (strstr(line, "\"rel_path\":")) {
            char *start = strchr(line, ':');
            if (start && current_gm >= 0 && current_gm <= 127) {
                start = strchr(start, '"');
                if (start) {
                    start++;
                    char *end = strrchr(start, '"');
                    if (end) {
                        *end = '\0';
                        
                        if (current_gm == program) {
                            if (best_path) free(best_path);
                            best_path = strdup(start);
                            best_id = current_id;
                            fclose(f);
                            *out_id = best_id;
                            return best_path;
                        }
                        
                        if (!best_path) {
                            best_path = strdup(start);
                            best_id = current_id;
                            best_gm = current_gm;
                        } else {
                            if (get_gm_family(current_gm) == target_family && get_gm_family(best_gm) != target_family) {
                                free(best_path);
                                best_path = strdup(start);
                                best_id = current_id;
                                best_gm = current_gm;
                            } else if (get_gm_family(current_gm) == target_family && get_gm_family(best_gm) == target_family) {
                                if (abs(current_gm - program) < abs(best_gm - program)) {
                                    free(best_path);
                                    best_path = strdup(start);
                                    best_id = current_id;
                                    best_gm = current_gm;
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    fclose(f);
    
    if (best_path) {
        printf("Warning: Program %d missing. Falling back to Program %d for nearest match.\n", program, best_gm);
        *out_id = best_id;
    }
    
    return best_path;
}
static const int adpcm_index_table[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8
};
static const int adpcm_step_table[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 
    253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 
    3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 
    12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};
static uint8_t encode_adpcm_nibble(int16_t sample, int *predicted, int *step_index) {
    int expected = *predicted;
    int step = adpcm_step_table[*step_index];
    int diff = sample - expected;
    int sign = 0;
    if (diff < 0) { sign = 8; diff = -diff; }
    
    int nibble = 0;
    int delta = step >> 3;
    if (diff >= step) { nibble |= 4; diff -= step; delta += step; }
    int half_step = step >> 1;
    if (diff >= half_step) { nibble |= 2; diff -= half_step; delta += half_step; }
    int quarter_step = step >> 2;
    if (diff >= quarter_step) { nibble |= 1; delta += quarter_step; }
    
    nibble |= sign;
    if (sign) *predicted -= delta; else *predicted += delta;
    if (*predicted > 32767) *predicted = 32767;
    if (*predicted < -32768) *predicted = -32768;
    
    *step_index += adpcm_index_table[nibble];
    if (*step_index < 0) *step_index = 0;
    if (*step_index > 88) *step_index = 88;
    return nibble;
}
static void pack_file(const char *path, uint8_t program, uint32_t source_id, FILE *f_out, uint32_t *offset, bool use_adpcm, bool do_trim) {
    FILE *f_wav = fopen(path, "rb");
    if (!f_wav) return;

    char chunk_id[4];
    uint32_t data_size = 0;
    uint32_t sample_rate = 44100;
    fseek(f_wav, 12, SEEK_SET);
    while (fread(chunk_id, 1, 4, f_wav) == 4) {
        uint32_t chunk_sz;
        if (fread(&chunk_sz, 4, 1, f_wav) != 1) break;
        if (memcmp(chunk_id, "fmt ", 4) == 0) {
            if (fseek(f_wav, 4, SEEK_CUR) != 0) break; /* skip audio_fmt + channels */
            uint32_t sr;
            if (fread(&sr, 4, 1, f_wav) != 1) break;
            sample_rate = sr;
            if (fseek(f_wav, 8, SEEK_CUR) != 0) break; /* skip byte_rate + block_align + bits_per_sample */
            if (chunk_sz > 16) fseek(f_wav, chunk_sz - 16, SEEK_CUR);
        } else if (memcmp(chunk_id, "data", 4) == 0) {
            data_size = chunk_sz;
            break;
        } else {
            fseek(f_wav, chunk_sz, SEEK_CUR);
        }
    }
    if (data_size > 0) {
        uint8_t *buf = malloc(data_size);
        if (buf && fread(buf, 1, data_size, f_wav) == data_size) {
            
            if (do_trim) {
                int16_t *pcm16 = (int16_t *)buf;
                uint32_t num_samples = data_size / 2;
                uint32_t trim_end = num_samples;
                // Simple trimming: search from end for any sound
                for (int32_t i = num_samples - 1; i >= 0; i--) {
                    if (abs(pcm16[i]) > 32) {
                        trim_end = i + 4410; // add a short tail
                        break;
                    }
                }
                if (trim_end < num_samples) {
                    data_size = trim_end * 2;
                }
                // Also hard limit duration to ~1 second max (44100 bytes) if trim is on
                if (data_size > 88200) data_size = 88200;
            }
            
            uint32_t padding = (4 - (*offset % 4)) % 4;
            for(uint32_t i=0; i<padding; i++) fputc(0, f_out);
            *offset += padding;
            if (use_adpcm) {
                uint32_t num_samples = data_size / 2;
                uint32_t adpcm_size = num_samples / 2;
                uint8_t *adpcm_buf = malloc(adpcm_size);
                int predicted = 0;
                int step_index = 0;
                int16_t *pcm16 = (int16_t *)buf;
                for (uint32_t i = 0; i < num_samples; i += 2) {
                    uint8_t n1 = encode_adpcm_nibble(pcm16[i], &predicted, &step_index);
                    uint8_t n2 = (i+1 < num_samples) ? encode_adpcm_nibble(pcm16[i+1], &predicted, &step_index) : 0;
                    adpcm_buf[i/2] = n1 | (n2 << 4);
                }
                instrument_bank[program] = (patch_info_t){ .addr = *offset, .size = adpcm_size, .format = AFX_FMT_ADPCM, .source_id = source_id };
                fwrite(adpcm_buf, 1, adpcm_size, f_out);
                *offset += adpcm_size;
                free(adpcm_buf);
                printf("Packed+ADPCM Program %d [%08X] -> %s (%u bytes -> %u bytes)\n", program, source_id, path, data_size, adpcm_size);
            } else {
                instrument_bank[program] = (patch_info_t){ .addr = *offset, .size = data_size, .format = AFX_FMT_PCM16, .source_id = source_id };
                fwrite(buf, 1, data_size, f_out);
                *offset += data_size;
                printf("Packed Program %d [%08X] -> %s (%u bytes)\n", program, source_id, path, data_size);
            }
            sample_descs[program] = (afx_sample_desc_t){
                .source_id   = source_id,
                .gm_program  = program,
                .format      = instrument_bank[program].format,
                .loop_mode   = AFX_LOOP_NONE,
                .root_note   = 60,
                .fine_tune   = 0,
                .sample_off  = instrument_bank[program].addr,
                .sample_size = instrument_bank[program].size,
                .loop_start  = 0,
                .loop_end    = 0,
                .sample_rate = sample_rate,
            };
        }
        if (buf) free(buf);
    }
    fclose(f_wav);
}
/*
 * write_patch_flow_cmds — emit all per-voice register writes for a note-on.
 *
 * SA encoding: both SA_HI and SA_LO initially carry SDAT-local byte offsets.
 * They are converted to file-relative offsets once section layout is finalized.
 */
static void write_patch_flow_cmds(uint32_t timestamp, uint8_t slot, uint8_t program,
                                  uint8_t ar, uint8_t d1r, uint8_t d2r, uint8_t rr, uint8_t dl,
                                  uint8_t velocity, uint8_t midi_pan,
                                  afx_cmd_t *cmds, uint32_t *cmd_idx) {
    patch_info_t p = instrument_bank[program];
    if (p.size == 0) return;

    /* Bit 15: Key On, Bit 9: Loop, Bits [8:7]: Format, Bits [6:0]: SA_HI */
    uint32_t loop_bit = (sample_descs[program].loop_mode != AFX_LOOP_NONE) ? (1u << 9) : 0;
    uint32_t format_bits = (uint32_t)(p.format & 0x3) << 7;
    uint32_t ctl_val = (1u << 15) | loop_bit | format_bits | (p.addr >> 16);

    /* Sample address: SDAT-local offset in both HI and LO flow commands */
    cmds[(*cmd_idx)++] = (afx_cmd_t){ timestamp, slot, AICA_REG_SA_HI, (uint16_t)ctl_val };
    cmds[(*cmd_idx)++] = (afx_cmd_t){ timestamp, slot, AICA_REG_SA_LO, (uint16_t)(p.addr & 0xFFFF) };

    /* Loop registers: only emit when the descriptor has defined loop points.
     * These are 16-bit relative offsets from the Sample Start (SA). 
     * Since the ARM7 calculates absolute SA, we can pre-calculate these offsets here. */
    afx_sample_desc_t *desc = &sample_descs[program];
    if (desc->loop_mode != AFX_LOOP_NONE && desc->loop_end > 0) {
        /* AICA loop registers (LSA/LEA) are 16-bit relative to SA.
         * The hardware automatically adds SA to these. */
        uint32_t lsa = desc->loop_start;
        uint32_t lea = desc->loop_end;

        /* Clip/Warn if offsets exceed 64k (16-bit limit) */
        if (lsa > 0xFFFF) lsa = 0xFFFF;
        if (lea > 0xFFFE) lea = 0xFFFE; /* 0xFFFF is reserved/invalid for LEA */

        cmds[(*cmd_idx)++] = (afx_cmd_t){ timestamp, slot, AICA_REG_LSA, (uint16_t)lsa };
        cmds[(*cmd_idx)++] = (afx_cmd_t){ timestamp, slot, AICA_REG_LEA, (uint16_t)lea };
    }

    /* ADSR Pre-calculated Bitfields:
     * D2R_D1R: D1R (Bits 12:8), AR (Bits 4:0) --> Actually [docs/aica_technical_reference.md] says D1R at bit 8, AR at bit 0.
     * EGH_RR:  D2R (Bits 12:8), DL (Bits 7:3), RR (Bits 4:0)
     */
    uint32_t d2r_d1r_val = ((uint32_t)(d1r & 0x1F) << 8) | (ar & 0x1F);
    uint32_t egh_rr_val  = ((uint32_t)(d2r & 0x1F) << 8) | ((uint32_t)(dl & 0x1F) << 3) | (rr & 0x1F);

    cmds[(*cmd_idx)++] = (afx_cmd_t){ timestamp, slot, AICA_REG_D2R_D1R, (uint16_t)d2r_d1r_val };
    cmds[(*cmd_idx)++] = (afx_cmd_t){ timestamp, slot, AICA_REG_EGH_RR,  (uint16_t)egh_rr_val };

    /* Velocity -> Total Level: TL 0=loudest, 127=nearly silent. Invert vel. */
    cmds[(*cmd_idx)++] = (afx_cmd_t){ timestamp, slot, AICA_REG_TOT_LVL, (uint16_t)(127u - velocity) };

    /* Pan: MIDI 0-127 -> AICA DIPAN 5-bit (stored in bits [20:16] of PAN_VOL). */
    uint8_t aica_pan = (uint8_t)((midi_pan * 31u) / 127u);
    cmds[(*cmd_idx)++] = (afx_cmd_t){ timestamp, slot, AICA_REG_PAN_VOL, (uint16_t)((uint32_t)aica_pan << 20) };
}
typedef struct {
    afx_cmd_t cmd;
    uint32_t seq;
} sort_op_t;
int cmp_flow_cmds(const void *a, const void *b) {
    sort_op_t *oa = (sort_op_t *)a;
    sort_op_t *ob = (sort_op_t *)b;
    if (oa->cmd.timestamp < ob->cmd.timestamp) return -1;
    if (oa->cmd.timestamp > ob->cmd.timestamp) return 1;
    if (oa->seq < ob->seq) return -1;
    if (oa->seq > ob->seq) return 1;
    return 0;
}

static void print_usage(const char *progname) {
    printf("Usage: %s [--trim] [--16bit] <input.mid> <output.afx> <wavetables.map>\n", progname);
    printf("  --trim   Trim trailing silence and clamp long samples before packing\n");
    printf("  --16bit  Disable ADPCM and store samples as 16-bit PCM\n");
    printf("  Default sample encoding is ADPCM when --16bit is not provided\n");
}

int main(int argc, char **argv) {
    bool use_adpcm = true;
    bool do_trim = false;
    const char *input_mid = NULL;
    const char *output_afx = NULL;
    const char *map_path = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--16bit") == 0) {
            use_adpcm = false;
            continue;
        }
        if (strcmp(argv[i], "--trim") == 0) {
            do_trim = true;
            continue;
        }
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        }

        if (!input_mid) input_mid = argv[i];
        else if (!output_afx) output_afx = argv[i];
        else if (!map_path) map_path = argv[i];
        else {
            fprintf(stderr, "Error: Unexpected argument '%s'\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    if (!input_mid || !output_afx || !map_path) {
        print_usage(argv[0]);
        return 1;
    }

    FILE *f_mid = fopen(input_mid, "rb");
    if (!f_mid) { perror("MIDI open failed"); return 1; }
    FILE *f_out = fopen(output_afx, "wb");
    if (!f_out) { fclose(f_mid); perror("Output open failed"); return 1; }
    FILE *f_samples = tmpfile();
    if (!f_samples) {
        fclose(f_mid);
        fclose(f_out);
        perror("tmpfile failed");
        return 1;
    }

    afx_header_t header = {
        .magic = AICAF_MAGIC,
        .version = AICAF_VERSION,
        .section_count = 0,
        .total_ticks = 0,
        .flags = 0,
    };
    bool needed[128] = {0};
    fseek(f_mid, 14, SEEK_SET);
    char chunk[4];
    while(fread(chunk, 1, 4, f_mid) == 4) {
        uint32_t len;
        if (fread(&len, 4, 1, f_mid) != 1) break;
        len = __builtin_bswap32(len);
        if(memcmp(chunk, "MTrk", 4) == 0) {
            long track_end = ftell(f_mid) + len;
            while(ftell(f_mid) < track_end) {
                uint32_t ignored_delta = read_varlen(f_mid);
                (void)ignored_delta;
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
    // Remove needed[0] = true;
    // Only pack instruments actually found in the tracks.
    uint32_t offset = 0;
    uint32_t source_count = 0;
    for(int i=0; i<128; i++) {
        if(needed[i]) {
            uint32_t sid = 0;
            char *path = find_sample_for_program(map_path, (uint8_t)i, &sid);
            if(path) { 
                bool dup = false;
                for(int j=0; j<i; j++) {
                    if (instrument_bank[j].size > 0 && instrument_bank[j].source_id == sid) {
                        instrument_bank[i] = instrument_bank[j];
                        sample_descs[i] = sample_descs[j];
                        sample_descs[i].gm_program = i;
                        dup = true;
                        break;
                    }
                }
                if (!dup) {
                    pack_file(path, i, sid, f_samples, &offset, use_adpcm, do_trim);
                    source_count++;
                } else {
                    printf("Reusing sample for Program %d [%08X]\n", i, sid);
                }
                free(path);
            }
        }
    }
    uint32_t sample_data_size = offset;
    fseek(f_mid, 14, SEEK_SET);
    afx_sample_desc_t *s_entries = malloc(sizeof(afx_sample_desc_t) * source_count);
    int s_idx = 0;
    for(int i=0; i<128; i++) {
        if(instrument_bank[i].size > 0) {
            bool duplicate_entry = false;
            for (int j = 0; j < i; j++) {
                if (instrument_bank[j].size > 0 &&
                    instrument_bank[j].source_id == instrument_bank[i].source_id &&
                    instrument_bank[j].addr == instrument_bank[i].addr) {
                    duplicate_entry = true;
                    break;
                }
            }
            if (duplicate_entry) continue;
            s_entries[s_idx] = sample_descs[i];
            s_idx++;
        }
    }
    
    // Support large MIDI files by buffering flow commands before sorting
    sort_op_t *sort_buffer = malloc(sizeof(sort_op_t) * 1000000);
    afx_cmd_t *flow_cmd_buffer = malloc(sizeof(afx_cmd_t) * 1000000);
    uint32_t flow_cmd_count = 0;
    
    uint16_t ppqn = 96;
    playback_policy_t program_policy[128];
    init_neutral_policies(program_policy);
    load_program_policies_from_map(map_path, program_policy);

    fseek(f_mid, 12, SEEK_SET);
    uint8_t d1 = fgetc(f_mid), d2 = fgetc(f_mid);
    if (!(d1 & 0x80)) ppqn = (d1 << 8) | d2;

    uint8_t default_channel_progs[16] = {0};
    uint8_t default_channel_attack[16];
    uint8_t default_channel_release[16];
    uint8_t default_channel_pan[16];
    bool have_default_prog[16]    = {0};
    bool have_default_attack[16]  = {0};
    bool have_default_release[16] = {0};
    bool have_default_pan[16]     = {0};
    for (int i = 0; i < 16; i++) {
        default_channel_attack[i]  = 31;
        default_channel_release[i] = 15;
        default_channel_pan[i]     = 64; /* MIDI center pan */
    }

    fseek(f_mid, 14, SEEK_SET);
    while(fread(chunk, 1, 4, f_mid) == 4) {
        uint32_t len;
        if (fread(&len, 4, 1, f_mid) != 1) break;
        len = __builtin_bswap32(len);
        if(memcmp(chunk, "MTrk", 4) == 0) {
            long track_end = ftell(f_mid) + len;
            uint8_t running_status = 0;
            while(ftell(f_mid) < track_end) {
                (void)read_varlen(f_mid);
                int status = fgetc(f_mid);
                if(status == EOF) break;
                if(!(status & 0x80)) { fseek(f_mid, -1, SEEK_CUR); status = running_status; }
                else running_status = (uint8_t)status;

                uint8_t type = status & 0xF0;
                uint8_t chan = status & 0x0F;
                if(type == 0xC0) {
                    uint8_t program = fgetc(f_mid);
                    if (!have_default_prog[chan]) {
                        default_channel_progs[chan] = program;
                        have_default_prog[chan] = true;
                    }
                } else if(type == 0xB0) {
                    uint8_t cc = fgetc(f_mid), val = fgetc(f_mid);
                    if (cc == 73 && !have_default_attack[chan]) {
                        default_channel_attack[chan] = (val * 31) / 127;
                        have_default_attack[chan] = true;
                    } else if (cc == 72 && !have_default_release[chan]) {
                        default_channel_release[chan] = (val * 31) / 127;
                        have_default_release[chan] = true;
                    } else if (cc == 10 && !have_default_pan[chan]) {
                        default_channel_pan[chan] = val;
                        have_default_pan[chan] = true;
                    }
                } else if (status == 0xFF) {
                    fgetc(f_mid);
                    uint32_t mlen = read_varlen(f_mid);
                    fseek(f_mid, mlen, SEEK_CUR);
                } else if (status == 0xF0 || status == 0xF7) {
                    uint32_t mlen = read_varlen(f_mid);
                    fseek(f_mid, mlen, SEEK_CUR);
                } else if (type == 0xD0) {
                    fgetc(f_mid);
                } else if (status < 0xF0) {
                    fgetc(f_mid);
                    if (type != 0xD0 && type != 0xC0) fgetc(f_mid);
                } else {
                    uint32_t mlen = read_varlen(f_mid);
                    fseek(f_mid, mlen, SEEK_CUR);
                }
            }
        } else fseek(f_mid, len, SEEK_CUR);
    }

    fseek(f_mid, 14, SEEK_SET);
    uint32_t max_timestamp = 0;
    uint8_t channel_progs[16];
    uint8_t channel_attack[16], channel_release[16], channel_pan[16];
    uint32_t slot_note_on_ms[64] = {0};
    uint8_t slot_note_on_prog[64] = {0};
    bool slot_active[64] = {0};

    for (int i = 0; i < 16; i++) {
        channel_progs[i]   = default_channel_progs[i];
        channel_attack[i]  = default_channel_attack[i];
        channel_release[i] = default_channel_release[i];
        channel_pan[i]     = default_channel_pan[i];
    }

    while(fread(chunk, 1, 4, f_mid) == 4) {
        uint32_t len;
        if (fread(&len, 4, 1, f_mid) != 1) break;
        len = __builtin_bswap32(len);
        if(memcmp(chunk, "MTrk", 4) == 0) {
            long track_end = ftell(f_mid) + len;
            uint32_t current_ms = 0;
            uint32_t tempo_us_pqn = 500000; // 120 BPM default
            uint8_t running_status = 0;
            while(ftell(f_mid) < track_end) {
                uint32_t delta = read_varlen(f_mid);
                
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
                    else if (cc == 10) channel_pan[chan] = val;
                }
                else if(type == 0x90 || type == 0x80) {
                    uint8_t note = fgetc(f_mid), vel = fgetc(f_mid);
                    uint8_t slot = (chan * 4 + (note % 4)) & 0x3F;
                    if(type == 0x90 && vel > 0) {
                        uint8_t prog = channel_progs[chan];
                        playback_policy_t policy = program_policy[prog];
                        uint8_t vel_shaped = apply_velocity_policy(vel, &policy);
                        uint8_t root = sample_descs[prog].root_note ? sample_descs[prog].root_note : 60;
                        float ratio = midi_note_to_freq(note) / midi_note_to_freq(root);
                        int rel = clamp_int((int)channel_release[chan] + (int)policy.release_bias, 0, 31);
                        write_patch_flow_cmds(current_ms, slot, prog,
                                              channel_attack[chan], (uint8_t)15, (uint8_t)15, (uint8_t)rel, (uint8_t)0,
                                              vel_shaped, channel_pan[chan],
                                              flow_cmd_buffer, &flow_cmd_count);
                        flow_cmd_buffer[flow_cmd_count++] = (afx_cmd_t){ current_ms, slot, AICA_REG_FNS_OCT, (uint16_t)aica_pitch_convert(ratio) };
                        slot_note_on_ms[slot] = current_ms;
                        slot_note_on_prog[slot] = prog;
                        slot_active[slot] = true;
                    } else {
                        uint8_t prog = slot_active[slot] ? slot_note_on_prog[slot] : channel_progs[chan];
                        playback_policy_t policy = program_policy[prog];
                        uint32_t off_ms = current_ms;
                        if (policy.note_trim_ms > 0 && off_ms > policy.note_trim_ms) {
                            off_ms -= policy.note_trim_ms;
                        }
                        if (slot_active[slot]) {
                            uint32_t min_off = slot_note_on_ms[slot] + policy.min_hold_ms;
                            if (off_ms < min_off) off_ms = min_off;
                            slot_active[slot] = false;
                        }
                        if (off_ms > max_timestamp) max_timestamp = off_ms;
                        /* Emit Key Off (Bit 14) via SA_HI register */
                        uint32_t sa_hi_val = (instrument_bank[prog].addr >> 16) & 0x7F;
                        flow_cmd_buffer[flow_cmd_count++] = (afx_cmd_t){ off_ms, slot, AICA_REG_SA_HI, (uint16_t)((1u << 14) | sa_hi_val) };
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
    
    for (uint32_t i = 0; i < flow_cmd_count; i++) {
        sort_buffer[i].cmd = flow_cmd_buffer[i];
        sort_buffer[i].seq = i;
    }
    qsort(sort_buffer, flow_cmd_count, sizeof(sort_op_t), cmp_flow_cmds);
    for (uint32_t i = 0; i < flow_cmd_count; i++) {
        flow_cmd_buffer[i] = sort_buffer[i].cmd;
    }
    free(sort_buffer);

    afx_section_entry_t sections[3];
    uint32_t section_count = 0;
    uint32_t cursor = sizeof(afx_header_t) + (uint32_t)(3 * sizeof(afx_section_entry_t));
    cursor = AFX_ALIGN32(cursor);

    uint32_t flow_bytes = flow_cmd_count * (uint32_t)sizeof(afx_cmd_t);
    sections[section_count++] = (afx_section_entry_t){
        .id = AFX_SECT_FLOW,
        .offset = cursor,
        .size = flow_bytes,
        .count = flow_cmd_count,
        .align = 32,
        .flags = 0,
    };
    cursor = AFX_ALIGN32(cursor + flow_bytes);

    uint32_t sdes_bytes = source_count * (uint32_t)sizeof(afx_sample_desc_t);
    sections[section_count++] = (afx_section_entry_t){
        .id = AFX_SECT_SDES,
        .offset = cursor,
        .size = sdes_bytes,
        .count = source_count,
        .align = 32,
        .flags = 0,
    };
    cursor = AFX_ALIGN32(cursor + sdes_bytes);

    sections[section_count++] = (afx_section_entry_t){
        .id = AFX_SECT_SDAT,
        .offset = cursor,
        .size = sample_data_size,
        .count = 0,
        .align = 32,
        .flags = 0,
    };

    header.section_count = section_count;
    header.total_ticks = max_timestamp;

    fseek(f_out, 0, SEEK_SET);
    fwrite(&header, sizeof(header), 1, f_out);
    fwrite(sections, sizeof(afx_section_entry_t), section_count, f_out);

    /* Convert SDAT-local addresses to file-relative offsets for ARM7 runtime. */
    uint32_t sdat_file_off = sections[2].offset;
    for (uint32_t i = 0; i < flow_cmd_count; i++) {
        uint8_t reg = flow_cmd_buffer[i].reg;
        /* Only SA_HI and SA_LO carry absolute-relative offsets.
           LSA and LEA are pre-calculated relative to SA. */
        if (reg == AICA_REG_SA_HI || reg == AICA_REG_SA_LO) {
            flow_cmd_buffer[i].value += sdat_file_off;
        }
    }
    for (uint32_t i = 0; i < source_count; i++) {
        s_entries[i].sample_off += sdat_file_off;
    }

    /* Padding to first section offset (usually 0x60) */
    long pos = ftell(f_out);
    while ((uint32_t)pos < sections[0].offset) {
        fputc(0, f_out);
        pos++;
    }

    fwrite(flow_cmd_buffer, sizeof(afx_cmd_t), flow_cmd_count, f_out);
    pos = ftell(f_out);
    while ((uint32_t)pos < sections[1].offset) {
        fputc(0, f_out);
        pos++;
    }

    fwrite(s_entries, sizeof(afx_sample_desc_t), source_count, f_out);
    pos = ftell(f_out);
    while ((uint32_t)pos < sections[2].offset) {
        fputc(0, f_out);
        pos++;
    }

    fseek(f_samples, 0, SEEK_SET);
    uint8_t copy_buf[4096];
    size_t n = 0;
    while ((n = fread(copy_buf, 1, sizeof(copy_buf), f_samples)) > 0) {
        fwrite(copy_buf, 1, n, f_out);
    }

    free(s_entries);
    free(flow_cmd_buffer);
    fclose(f_samples);
    fclose(f_mid);
    fclose(f_out);
    printf("Success: Generated %s with %u flow commands. Duration: %ums\n", output_afx, flow_cmd_count, max_timestamp);
    return 0;
}