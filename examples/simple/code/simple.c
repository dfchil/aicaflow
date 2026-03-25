#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <stdio.h>
#include <afx/host.h>
#include <enDjinn/enj_enDjinn.h>
#include <enDjinn/ext/dca_file.h>

int fDaValidateHeader(const fDcAudioHeader *dca);
size_t fDaCalcChannelSizeBytes(const fDcAudioHeader *dca);
unsigned fDaConvertFrequency(unsigned int freq_hz);

static int probe_afx_driver_clock(void) {
  uint32_t start_tick = afx_get_tick();
  uint32_t end_tick = start_tick;

  for (volatile uint32_t spin = 0; spin < 5000000u; ++spin) {
    if ((spin & 0x3FFFu) == 0u) {
      end_tick = afx_get_tick();
      if (end_tick != start_tick) {
        break;
      }
    }
  }

  printf("[SFX] driver tick probe: start=%lu end=%lu %s\n",
         (unsigned long)start_tick,
         (unsigned long)end_tick,
         (start_tick != end_tick) ? "(running)" : "(stalled)");
  return start_tick != end_tick;
}

static int init_afx_driver(void) {
  if (!afx_init()) {
    printf("[SFX] afx_init failed\n");
    return -1;
  }
  printf("[SFX] afx_init ok\n");

  if (!probe_afx_driver_clock()) {
    printf("[SFX] driver clock did not advance; ARM driver may not be running\n");
    return -1;
  }

  return 0;
}

static int load_dca_blob(uint8_t *dca_data) {
  fDcAudioHeader *data = (fDcAudioHeader *)dca_data;
  if (fDaValidateHeader(data)) {
    return afx_sample_upload(fDaGetChannelSamples(data, 0),
                                fDaCalcChannelSizeBytes(data),
                                fDaCalcSampleRateHz(data),
                                (uint8_t[]){16, 8, 4}[fDaGetSampleFormat(data)],
                                fDaGetChannelCount(data));
  }
  return -1;
}

uint8_t wilhelm_adpcm_data[] = {
#embed "../embeds/simple/sfx/ADPCM/Wilhelm_Scream.dca"
};
uint8_t wilhelm_pcm8_data[] = {
#embed "../embeds/simple/sfx/PCM/8/Wilhelm_Scream.dca"
};
uint8_t wilhelm_pcm16_data[] = {
#embed "../embeds/simple/sfx/PCM/16/Wilhelm_Scream.dca"
};
uint8_t clean_test_adpcm[] = {
#embed "../embeds/simple/sfx/ADPCM/clean-audio-test-tone.dca"
};
uint8_t clean_test_pcm8[] = {
#embed "../embeds/simple/sfx/PCM/8/clean-audio-test-tone.dca"
};
uint8_t clean_test_pcm16[] = {
#embed "../embeds/simple/sfx/PCM/16/clean-audio-test-tone.dca"
};

typedef struct {
  struct {
    char name[48];
    void (*on_press_A)(void *data);
  };
} SFX_menu_entry_t;
typedef struct {
  struct {
    uint32_t active_controller : 2;
    int32_t cursor_pos : 5;
    int32_t loaded_pattern : 5;
    uint8_t pan : 8;
    uint32_t reserved : 12;
  };
  int sounds[6];
} SPE_state_t;

typedef struct {
  uint32_t flow_spu;
  uint32_t song_spu;
} SFX_active_flow_t;

#define SFX_MAX_ACTIVE_FLOWS 32
static SFX_active_flow_t g_active_flows[SFX_MAX_ACTIVE_FLOWS];

/*
 * Build a single-shot .afx flow in memory for a pre-uploaded sample.
 * Uses AFX_FILE_FLAG_EXTERNAL_SAMPLE_ADDRS so the FLOW commands embed
 * absolute AICA SPU address. Returns flow SPU address, or 0 on failure.
 */
static uint32_t create_sfx_flow(uint32_t sample_handle, uint8_t pan) {
  afx_sample_info_t info;
  if (!afx_sample_get_info(sample_handle, &info)) {
    printf("[SFX] sample info lookup failed: handle=%lu\n", (unsigned long)sample_handle);
    return 0;
  }

  uint8_t pcms = (info.bitsize == 16) ? 0 : (info.bitsize == 8) ? 1 : 2;
  uint32_t n = (info.bitsize == 16) ? info.length / 2
             : (info.bitsize ==  8) ? info.length
             :                        info.length * 2;
  if (info.channels > 1) n /= info.channels;
  uint32_t n_clamped = (n >= 65535u) ? 65534u : n;
  uint32_t duration_ms = n ? (n * 1000u) / info.rate + 1u : 1000u;

  uint8_t dipan;
  if (pan == 0x80u) {
    dipan = 0u;
  } else if (pan < 0x80u) {
    dipan = (uint8_t)(0x10u | ((0x7Fu - pan) >> 3));
  } else {
    dipan = (uint8_t)((pan - 0x80u) >> 3);
  }

    printf("[SFX] sample=%lu spu=0x%08lx len=%lu rate=%lu bits=%u ch=%u pan=%u dipan=%u dur=%lums\n",
      (unsigned long)sample_handle,
      (unsigned long)info.spu_addr,
      (unsigned long)info.length,
      (unsigned long)info.rate,
      info.bitsize,
      info.channels,
      pan,
      dipan,
      (unsigned long)duration_ms);

  const uint16_t reg_play_ctrl =
      (uint16_t)(offsetof(aica_chnl_packed_t, play_ctrl) / sizeof(uint16_t));
  const uint16_t startup_reg_count =
      (uint16_t)((offsetof(aica_chnl_packed_t, pan) + sizeof(uint16_t) -
                  offsetof(aica_chnl_packed_t, play_ctrl)) /
                 sizeof(uint16_t));
  const uint16_t reg_resonance =
      (uint16_t)(offsetof(aica_chnl_packed_t, resonance) / sizeof(uint16_t));

  uint8_t blob[256];
  memset(blob, 0, sizeof(blob));

  /* Header */
  afx_header_t *hdr = (afx_header_t *)(blob);
  hdr->magic         = AICAF_MAGIC;
  hdr->version       = AICAF_VERSION;
  hdr->section_count = 2u;
  hdr->total_ticks   = duration_ms;
  hdr->flags         = AFX_FILE_FLAG_EXTERNAL_SAMPLE_ADDRS;

  /* Section table */
  afx_section_entry_t *sects = (afx_section_entry_t *)(blob + sizeof(afx_header_t));

  uint32_t flow_off = sizeof(afx_header_t) + 2 * sizeof(afx_section_entry_t);
  uint8_t *cursor = blob + flow_off;
  
  afx_cmd_t *cmd0 = (afx_cmd_t *)cursor;
  cmd0->timestamp = 0;
  cmd0->slot = 0;
  cmd0->offset = reg_play_ctrl;
  cmd0->length = startup_reg_count;
  
  aica_chnl_packed_t *chncfg = (aica_chnl_packed_t *)cmd0->values;
  chncfg->play_ctrl.bits.key_on_ex = 1u;
  chncfg->play_ctrl.bits.key_on = 1u;
  chncfg->play_ctrl.bits.sa_high = (uint16_t)((info.spu_addr >> 16) & 0x7Fu);
  chncfg->play_ctrl.bits.pcms = pcms;
  chncfg->play_ctrl.bits.lpctl = 0u;
  chncfg->play_ctrl.bits.ssctl = 0u;
  chncfg->sa_low = (uint16_t)(info.spu_addr & 0xFFFFu);
  chncfg->lsa = 0u;
  chncfg->lea = (uint16_t)n_clamped;
  chncfg->env_ad.bits.ar = 31u;
  chncfg->env_ad.bits.d1r = 0u;
  chncfg->env_ad.bits.d2r = 0u;
  chncfg->env_dr.bits.rr = 15u;
  chncfg->env_dr.bits.dl = 0u;
  chncfg->env_dr.bits.krs = 0u;
  chncfg->env_dr.bits.lpslnk = 0u;
  chncfg->pitch.raw = (uint16_t)fDaConvertFrequency(info.rate);
  chncfg->env_fm.bits.tl = 0u;
  chncfg->pan.bits.dipan = dipan;
  chncfg->pan.bits.disdl = 0xFu;

    printf("[SFX] regs slot=%u pcms=%u sa_hi=%u sa_lo=0x%04x fns_oct=0x%04x tl=%u disdl=%u\n",
      cmd0->slot,
      chncfg->play_ctrl.bits.pcms,
      chncfg->play_ctrl.bits.sa_high,
      chncfg->sa_low,
      chncfg->pitch.raw,
      chncfg->env_fm.bits.tl,
      chncfg->pan.bits.disdl);

  cursor += sizeof(uint32_t) + sizeof(uint16_t) +
            (size_t)cmd0->length * sizeof(uint16_t);
  cursor += ((uintptr_t)cursor & 3u) ? (4u - ((uintptr_t)cursor & 3u)) : 0u;

  /* Match KOS default LPF setup (CHNREG8 at +40 = 0x24). */
  afx_cmd_t *cmd1 = (afx_cmd_t *)cursor;
  cmd1->timestamp = 0;
  cmd1->slot = 0;
  cmd1->offset = reg_resonance;
  cmd1->length = 1;
  cmd1->values[0] = 0x24u;
  cursor += sizeof(uint32_t) + sizeof(uint16_t) +
            (size_t)cmd1->length * sizeof(uint16_t);
  cursor += ((uintptr_t)cursor & 3u) ? (4u - ((uintptr_t)cursor & 3u)) : 0u;

  uint32_t flow_size = (uint32_t)(cursor - (blob + flow_off));

  /* META section */
  uint32_t meta_off = (uint32_t)(cursor - blob);
  afx_meta_t *meta = (afx_meta_t *)cursor;
  meta->version = AFX_META_VERSION;
  meta->required_channels = 1u;
  cursor += sizeof(afx_meta_t);

  /* Patch section table */
  sects[0].id = AFX_SECT_FLOW;
  sects[0].offset = flow_off;
  sects[0].size = flow_size;
  sects[0].count = 2u;
  sects[0].align = 4u;
  
  sects[1].id = AFX_SECT_META;
  sects[1].offset = meta_off;
  sects[1].size = sizeof(afx_meta_t);
  sects[1].count = 1u;
  sects[1].align = 4u;

  uint32_t song_spu = afx_upload_afx(blob, (uint32_t)(cursor - blob));
  if (!song_spu) {
    printf("[SFX] afx_upload_afx failed\n");
    return 0;
  }
  uint32_t flow_spu = afx_create_flow(song_spu);
  if (!flow_spu) {
    printf("[SFX] afx_create_flow failed: song_spu=0x%08lx\n", (unsigned long)song_spu);
    afx_free_afx(song_spu);
    return 0;
  }
  printf("[SFX] flow created: song_spu=0x%08lx flow_spu=0x%08lx flow_size=%lu\n",
         (unsigned long)song_spu,
         (unsigned long)flow_spu,
         (unsigned long)flow_size);
  return flow_spu;
}

/* SPU RAM is mapped at this fixed address on SH4 */
#define SPU_RAM_BASE_SH4 0xA0800000u
/* AICA channel registers are mapped at this fixed address on SH4 */
#define AICA_REG_BASE_SH4 0xA0700000u

/*
 * Track flows locally and retire them when all assigned channels have
 * transitioned to key-off (key_on_ex == 0).
 */
static int flow_all_channels_keyoff(uint32_t flow_spu) {
  const volatile afx_flow_state_t *fs =
      (const volatile afx_flow_state_t *)(SPU_RAM_BASE_SH4 + flow_spu);
  uint64_t mask = fs->assigned_channels;

  for (uint32_t slot = 0; slot < 64u; ++slot) {
    if (!(mask & (1ULL << slot))) {
      continue;
    }

    volatile uint16_t *play_ctrl =
        (volatile uint16_t *)(AICA_REG_BASE_SH4 + (slot << 7));
    if ((*play_ctrl & 0x8000u) != 0u) {
      return 0;
    }
  }

  return 1;
}

static void register_active_flow(uint32_t flow_spu) {
  const volatile afx_flow_state_t *fs =
      (const volatile afx_flow_state_t *)(SPU_RAM_BASE_SH4 + flow_spu);
  for (int i = 0; i < SFX_MAX_ACTIVE_FLOWS; ++i) {
    if (g_active_flows[i].flow_spu == 0) {
      g_active_flows[i].flow_spu = flow_spu;
      g_active_flows[i].song_spu = fs->afx_base;
      return;
    }
  }
  printf("[SFX] warning: active flow table full, flow=0x%08lx\n",
         (unsigned long)flow_spu);
}

static void drain_completed_flows(void) {
  for (int i = 0; i < SFX_MAX_ACTIVE_FLOWS; ++i) {
    uint32_t flow_spu = g_active_flows[i].flow_spu;
    if (!flow_spu) {
      continue;
    }

    if (!flow_all_channels_keyoff(flow_spu)) {
      continue;
    }

    uint32_t song_spu = g_active_flows[i].song_spu;
    printf("[SFX] completed(keyoff): flow_spu=0x%08lx song_spu=0x%08lx\n",
           (unsigned long)flow_spu,
           (unsigned long)song_spu);
    afx_release_channels(flow_spu);
    printf("[SFX] channels freed: flow_spu=0x%08lx\n", (unsigned long)flow_spu);
    afx_free_afx(song_spu);
    afx_mem_free(flow_spu, sizeof(afx_flow_state_t));
    g_active_flows[i].flow_spu = 0;
    g_active_flows[i].song_spu = 0;
  }
}

static int probe_flow_dispatch(uint32_t flow_spu) {
  const volatile afx_flow_state_t *fs =
      (const volatile afx_flow_state_t *)(SPU_RAM_BASE_SH4 + flow_spu);
  uint8_t hw_slot = fs->channel_map[0];
  if (hw_slot == 0xFFu) {
    printf("[SFX] cmd probe: invalid channel map for flow 0x%08lx\n",
           (unsigned long)flow_spu);
    return 0;
  }

    const afx_cmd_t *cmd0 =
      (const afx_cmd_t *)(SPU_RAM_BASE_SH4 + fs->flow_ptr);
    if (cmd0->length < 2u) {
    printf("[SFX] cmd probe: cmd0 too short (len=%u)\n", cmd0->length);
    return 0;
    }

    uint16_t reg_probe = (uint16_t)(cmd0->offset + 1u); /* usually sa_low */
    uint16_t expected = cmd0->values[1];
  volatile uint16_t *probe_reg = (volatile uint16_t *)(
      AICA_REG_BASE_SH4 + ((uint32_t)hw_slot << 7) + ((uint32_t)reg_probe << 2));

    *probe_reg = 0u; /* Sentinel value; flow startup should overwrite this. */
  uint16_t before = *probe_reg;

  afx_play_flow(flow_spu);

  uint16_t after = before;
  for (volatile uint32_t spin = 0; spin < 2000000u; ++spin) {
    after = *probe_reg;
    if (after != before) {
      break;
    }
  }

    printf("[SFX] cmd probe: flow=0x%08lx slot=%u reg=%u expected=0x%04x before=0x%04x after=0x%04x %s\n",
         (unsigned long)flow_spu,
         hw_slot,
      reg_probe,
      expected,
         before,
         after,
      (after == expected) ? "(dispatch ok)" : "(dispatch not observed)");
    return after == expected;
}

static void play_sfx(void *data) {
  drain_completed_flows();
  SPE_state_t *state = (SPE_state_t *)data;
  printf("[SFX] trigger: idx=%d pan=%u handle=%d\n",
         state->cursor_pos,
         state->pan,
         state->sounds[state->cursor_pos]);
  if (state->sounds[state->cursor_pos] != -1) {
    uint32_t flow = create_sfx_flow((uint32_t)state->sounds[state->cursor_pos], state->pan);
    if (!flow) {
      /* Retry once after draining in case channels just became available. */
      drain_completed_flows();
      flow = create_sfx_flow((uint32_t)state->sounds[state->cursor_pos], state->pan);
    }
    if (flow){
      printf("[SFX] play flow: 0x%08lx\n", (unsigned long)flow);
      if (!probe_flow_dispatch(flow)) {
        printf("[SFX] warning: flow dispatch probe failed\n");
      }
      register_active_flow(flow);
    } else {
      printf("[SFX] play failed: flow create returned 0\n");
    }
  } else {
    printf("[SFX] trigger ignored: invalid sample handle\n");
  }
}

static const SFX_menu_entry_t sfx_catalog[] = {
    {.name = "Wilhelm scream, ADPCM encoded", .on_press_A = play_sfx},
    {.name = "Wilhelm scream, PCM 8bit encoded", .on_press_A = play_sfx},
    {.name = "Wilhelm scream, PCM 16bit encoded", .on_press_A = play_sfx},
    {.name = "Clean test tone, ADPCM encoded", .on_press_A = play_sfx},
    {.name = "Clean test tone, PCM 8bit encoded", .on_press_A = play_sfx},
    {.name = "Clean test tone, PCM 16bit encoded", .on_press_A = play_sfx},
    {.name = "Exit example", .on_press_A = enj_state_flag_shutdown},
};
static const int num_sfx_menu_entries =
    sizeof(sfx_catalog) / sizeof(sfx_catalog[0]);

#define MARGIN_LEFT 30
void render(void *data) {
  SPE_state_t *state = (SPE_state_t *)data;
  enj_qfont_color_set(0x14, 0xaf, 255); /* Light Blue */
  enj_font_scale_set(3);
  const char *title = "Sound Playback Example";
  int twidth = enj_font_string_width(title, enj_qfont_get_header());
  int textpos_x = (vid_mode->width - twidth) >> 1;
  int textpos_y = 4;
  enj_qfont_write(title, textpos_x, textpos_y, PVR_LIST_PT_POLY);
  enj_font_scale_set(1);
  textpos_y += 4 * enj_qfont_get_header()->line_height;

  /* show pan */
  enj_qfont_color_set(255, 255, 255); /* White */
  enj_qfont_write("Current pan:", MARGIN_LEFT, textpos_y, PVR_LIST_PT_POLY);
  textpos_y += enj_qfont_get_header()->line_height;
  char pan_str[7];
  snprintf(pan_str, sizeof(pan_str), "<%d>", state->pan - 128);
  int8_t signed_pan = (int8_t)(state->pan - 128);
  uint8_t red = signed_pan > 0 ? 0 : 127 + abs(signed_pan);
  uint8_t green = signed_pan < 0 ? 0 : 127 + abs(signed_pan);
  uint8_t blue = 255 - ((abs(signed_pan) - 1) << 1);

  enj_qfont_color_set(red, green, blue);
  enj_qfont_write(pan_str, (vid_mode->width >> 1) + (signed_pan << 1),
                  textpos_y, PVR_LIST_PT_POLY);
  textpos_y = (vid_mode->height >> 4) * 4;

  /* show menu  */
  enj_qfont_color_set(255, 255, 255); /* White */
  textpos_x = MARGIN_LEFT;

  for (int i = 0; i < num_sfx_menu_entries; i++) {
    if (state->cursor_pos == i) {
      enj_qfont_color_set(0, 255, 0); /* green */
      enj_qfont_write("->", textpos_x - 20,
                      textpos_y + i * enj_qfont_get_header()->line_height,
                      PVR_LIST_PT_POLY);
    } else {
      enj_qfont_color_set(255, 255, 255); /* White */
    }
    enj_qfont_write(sfx_catalog[i].name, textpos_x,
                    textpos_y + i * enj_qfont_get_header()->line_height,
                    PVR_LIST_PT_POLY);
    textpos_y += enj_qfont_get_header()->line_height;
  }
  textpos_y = (vid_mode->height >> 4) * 13;

  /* show instructions */
  enj_qfont_color_set(255, 255, 255); /* White */

  const char *longest_line =
      "Hold X and move stick to set pan, release X to hold pan position";
  textpos_x = vid_mode->width -
              (enj_font_string_width(longest_line, enj_qfont_get_header()) +
               MARGIN_LEFT);
  enj_qfont_write("Use DPAD UP/DOWN to navigate menu", textpos_x, textpos_y,
                  PVR_LIST_PT_POLY);
  textpos_y += enj_qfont_get_header()->line_height;
  enj_qfont_write("Press A to choose", textpos_x, textpos_y, PVR_LIST_PT_POLY);
  textpos_y += enj_qfont_get_header()->line_height;
  enj_qfont_write(longest_line, textpos_x, textpos_y, PVR_LIST_PT_POLY);
}

void main_mode_updater(void *data) {
  do {
    SPE_state_t *state = (SPE_state_t *)data;
    enj_ctrlr_state_t **ctrl_states = enj_ctrl_get_states();
    int delta = ctrl_states[state->active_controller]->button.UP ==
                        ENJ_BUTTON_DOWN_THIS_FRAME
                    ? -1
                : ctrl_states[state->active_controller]->button.DOWN ==
                        ENJ_BUTTON_DOWN_THIS_FRAME
                    ? 1
                    : 0;
    if (delta) {
      state->cursor_pos = (state->cursor_pos + delta) % num_sfx_menu_entries;
      if (state->cursor_pos < 0)
        state->cursor_pos = num_sfx_menu_entries - 1;
    }
    if (ctrl_states[state->active_controller]->button.A ==
        ENJ_BUTTON_DOWN_THIS_FRAME) {
      sfx_catalog[state->cursor_pos].on_press_A(data);
    }
    if (ctrl_states[state->active_controller]->button.X == ENJ_BUTTON_DOWN) {
      state->pan = (uint8_t)(ctrl_states[state->active_controller]->joyx + 128);
    }
  } while (0);
  enj_render_list_add(PVR_LIST_PT_POLY, render, data);
}

int main(__unused int argc, __unused char **argv) {
  enj_state_init_defaults();
  if (enj_state_startup() != 0) {
    ENJ_DEBUG_PRINT("enDjinn startup failed, exiting\n");
    return -1;
  }

  if (init_afx_driver() != 0) {
    ENJ_DEBUG_PRINT("AFX driver init failed, exiting\n");
    return -1;
  }

  SPE_state_t rat_state = {
      .cursor_pos = 0,
      .pan = 128,
      .sounds =
          {
              load_dca_blob(wilhelm_adpcm_data),
              load_dca_blob(wilhelm_pcm8_data),
              load_dca_blob(wilhelm_pcm16_data),
              load_dca_blob(clean_test_adpcm),
              load_dca_blob(clean_test_pcm8),
              load_dca_blob(clean_test_pcm16),
          },
  };
  enj_mode_t main_mode = {
      .name = "Main Mode",
      .mode_updater = main_mode_updater,
      .data = &rat_state,
  };
  enj_mode_push(&main_mode);
  enj_state_run();

  for (int i = 0; i < (int)(sizeof(rat_state.sounds) / sizeof(rat_state.sounds[0])); i++) {
    if (rat_state.sounds[i] != -1) {
      afx_sample_free(rat_state.sounds[i]);
    }
  }
  aica_shutdown();
  return 0;
}