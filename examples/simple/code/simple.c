#include <afx/aica_channel.h>
#include <afx/host.h>
#include <enDjinn/enj_enDjinn.h>
#include <enDjinn/ext/dca_file.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AICA_CLOCK_ADDR                                                        \
  0x001FFFE0 /* Reserve uppermost 4 words for clock registers */

#define AICA_HW_CLOCK                                                          \
  ((volatile uint32_t *)(SPU_RAM_BASE_SH4 + AICA_CLOCK_ADDR))
#define AICA_PREV_HW_CLOCK                                                     \
  ((volatile uint32_t *)(SPU_RAM_BASE_SH4 + AICA_CLOCK_ADDR + 4))
#define AICA_VIRTUAL_CLOCK                                                     \
  ((volatile uint32_t *)(SPU_RAM_BASE_SH4 + AICA_CLOCK_ADDR + 8))

#define drv_state_ptr                                                          \
  ((volatile afx_driver_state_t *)(SPU_RAM_BASE_SH4 + AFX_DRIVER_STATE_ADDR))

int fDaValidateHeader(const fDcAudioHeader *dca);
size_t fDaCalcChannelSizeBytes(const fDcAudioHeader *dca);
unsigned fDaConvertFrequency(unsigned int freq_hz);

static int init_afx_driver(void) {
  if (!afx_init()) {
    printf("afx_init failed\n");
    return -1;
  }
  printf("afx_init ok\n");
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

alignas(32) uint8_t wilhelm_adpcm_data[] = {
#embed "../embeds/simple/sfx/ADPCM/Wilhelm_Scream.dca"
};
alignas(32) uint8_t wilhelm_pcm8_data[] = {
#embed "../embeds/simple/sfx/PCM/8/Wilhelm_Scream.dca"
};
alignas(32) uint8_t wilhelm_pcm16_data[] = {
#embed "../embeds/simple/sfx/PCM/16/Wilhelm_Scream.dca"
};
alignas(32) uint8_t clean_test_adpcm[] = {
#embed "../embeds/simple/sfx/ADPCM/clean-audio-test-tone.dca"
};
alignas(32) uint8_t clean_test_pcm8[] = {
#embed "../embeds/simple/sfx/PCM/8/clean-audio-test-tone.dca"
};
alignas(32) uint8_t clean_test_pcm16[] = {
#embed "../embeds/simple/sfx/PCM/16/clean-audio-test-tone.dca"
};

alignas(32) uint8_t afx_test[] = {
#embed "../embeds/simple/afx/grieg.afx"
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
    uint32_t reserved : 12;
  };
  int sounds[6];
  uint32_t flows[6];
} SPE_state_t;

/**
 * Build a single-shot .afx flow in memory for a pre-uploaded sample.
 * Uses AFX_FILE_FLAG_EXTERNAL_SAMPLE_ADDRS so the FLOW commands embed
 * absolute AICA SPU address. Returns flow SPU address, or 0 on failure.
 *
 * returns flow slot index (0-63) on success, or 0 on failure (since slot 0 is
 * used, caller must check for flow upload success to disambiguate)
 */
static uint32_t create_sfx_flow(uint32_t sample_handle) {
  afx_sample_info_t info;
  if (!afx_sample_get_info(sample_handle, &info)) {
    printf("[SFX] sample info lookup failed: handle=%lu\n",
           (unsigned long)sample_handle);
    return 0;
  }

  uint32_t num_samples = (info.bitsize == 16)  ? info.length / 2
                         : (info.bitsize == 8) ? info.length
                                               : info.length * 2;
  if (info.channels > 1)
    num_samples /= info.channels;
  uint32_t duration_ms =
      num_samples ? (num_samples * 1000u) / info.rate + 1u : 1000u;

  uint8_t dipan = 15;

  printf("[SFX] sample=%lu spu=0x%08lx len=%lu rate=%lu bits=%u ch=%u "
         "dipan=%u dur=%lums\n\n",
         (unsigned long)sample_handle, (unsigned long)info.spu_addr,
         (unsigned long)info.length, (unsigned long)info.rate, info.bitsize,
         info.channels, dipan, (unsigned long)duration_ms);

  const uint16_t reg_play_ctrl =
      (uint16_t)(offsetof(aica_chnl_packed_t, play_ctrl) / sizeof(uint16_t));
  const uint16_t startup_reg_count =
      (uint16_t)((offsetof(aica_chnl_packed_t, pan) + sizeof(uint16_t) -
                  offsetof(aica_chnl_packed_t, play_ctrl)) /
                 sizeof(uint16_t));

  alignas(32) uint8_t blob[1024];
  memset(blob, 0, sizeof(blob));

  /* Header */
  afx_header_t *hdr = (afx_header_t *)(blob);
  hdr->magic = AICAF_MAGIC;
  hdr->version = AICAF_VERSION;
  hdr->section_count = 0u; /* No optional sections */
  hdr->total_ticks = duration_ms;
  hdr->flags = AFX_FILE_FLAG_EXTERNAL_SAMPLE_ADDRS;
  hdr->required_channels = 1u;
  

  /* The flow section immediately follows the section table space, but we have 0
     optional sections. We'll still align it for good measure. */
  uint32_t flow_off = AFX_ALIGN32(sizeof(afx_header_t));
  hdr->flow_offset = flow_off;

  uint8_t *cursor = blob + flow_off;

  afx_cmd_t *cmd0 = (afx_cmd_t *)cursor;
  cmd0->timestamp = 0;
  cmd0->pack = 0;
  AFX_CMD_SET_SLOT(cmd0, 0);
  AFX_CMD_SET_OFFSET(cmd0, reg_play_ctrl);
  AFX_CMD_SET_LENGTH(cmd0, startup_reg_count);

  aica_chnl_packed_t *chncfg = (aica_chnl_packed_t *)cmd0->values;

  uint16_t pcms = (info.bitsize == 16) ? 0 : (info.bitsize == 8) ? 1 : 2;
  uint16_t sa_high = (uint16_t)((info.spu_addr >> 16) & 0x7Fu);
  /* Hardware play_ctrl: [15] Key On, [14] Key Off.
     We set KeyOn=1 and KeyOff=0 to start playback. */
  chncfg->play_ctrl.raw = (sa_high) | (pcms << 7) | (0 << 9) | (1 << 15);

  chncfg->sa_low = (uint16_t)(info.spu_addr & 0xFFFFu);
  chncfg->lsa = 0u;
  chncfg->lea =
      num_samples > (1 << 16) - 1 ? (1 << 16) - 1 : (uint16_t)num_samples;

  /* packed: ar[0:4], res[5], d1r[6:10], d2r[11:15] */
  chncfg->env_ad.raw = (31u) | (0 << 6) | (0 << 11);

  /* Sustain Level (DL) to 31 (max) so the sound doesn't cut out after the
   * attack phase */
  /* packed: rr[0:4], dl[5:9], krs[10:13], lpslnk[14] */
  chncfg->env_dr.raw = (15u) | (31 << 5) | (0 << 10) | (0 << 14);

  chncfg->pitch.raw = (uint16_t)fDaConvertFrequency(info.rate);

  /* packed: isel[0:3] (actually isel[0:3]? wait, offset 2-5? No, user had
   * reserved:2, isel:4, tl:8. So: isel[2:5], tl[6:13]) */
  /* user's bitfield: isel[2:5], tl[6:13]. Wait! user's bitfield: res[0:1],
   * isel[2:5], tl[6:13], res2[14:15] */
  chncfg->env_fm.raw = (0u << 6); // tl = 0

  /* packed: dipan[0:4], res[5:7], disdl[8:11], imxl[12:15] */
  chncfg->pan.raw = (dipan) | (0xFu << 8);

  // TODO: investigate if this is stable across arm7<->sh4 toolchains, or if we
  // need to use explicit bit shifts/masks to pack the command values
  // aica_chnl_packed_t *chnlcfg = (aica_chnl_packed_t *)cmd0->values;
  // chnlcfg->play_ctrl.bits = (typeof(chnlcfg->play_ctrl.bits)){
  //   .key_on_ex = 1u,
  //   .key_on = 1u,
  //   .sa_high = (uint16_t)((info.spu_addr >> 16) & 0x7Fu),
  //   .pcms = (info.bitsize == 16) ? 0 : (info.bitsize == 8) ? 1 : 2,
  //   .lpctl = 0u,
  //   .ssctl = 0u
  // };
  // chnlcfg->sa_low = (uint16_t)(info.spu_addr & 0xFFFFu);
  // chnlcfg->lsa = 0u;
  // chnlcfg->lea = num_samples > (1<<16) - 1 ? (1<<16) - 1 :
  // (uint16_t)num_samples; chnlcfg->env_ad.bits =
  // (typeof(chnlcfg->env_ad.bits)){
  //   .ar = 31u,
  //   .d1r = 0u,
  //   .d2r = 0u
  // };
  // chnlcfg->env_dr.bits = (typeof(chnlcfg->env_dr.bits)){
  //   .rr = 15u,
  //   .dl = 0u,
  //   .krs = 0u,
  //   .lpslnk = 0u
  // };
  // chnlcfg->pitch.raw = (uint16_t)fDaConvertFrequency(info.rate);
  // chnlcfg->env_fm.bits = (typeof(chnlcfg->env_fm.bits)){
  //   .tl = 0u,
  //   .fb = 0u,
  // };
  // chnlcfg->pan.bits = (typeof(chnlcfg->pan.bits)){
  //   .dipan = dipan,
  //   .disdl = 0xFu
  // };

  printf("[SFX] regs slot=%u pcms=%u sa_hi=%u sa_lo=0x%04x fns_oct=0x%04x "
         "tl=%u disdl=%u\n",
         AFX_CMD_GET_SLOT(cmd0), pcms, sa_high, chncfg->sa_low,
         chncfg->pitch.raw, 0, 0xF);

  uint32_t cmd0_size = 6 + (AFX_CMD_GET_LENGTH(cmd0) * 2);
  cmd0_size = (cmd0_size + 3) & ~3;
  cursor = blob + flow_off + cmd0_size;

  /* Terminating key-off command at end of sample time */
  afx_cmd_t *cmd1 = (afx_cmd_t *)cursor;
  cmd1->timestamp = duration_ms;
  cmd1->pack = 0;
  AFX_CMD_SET_SLOT(cmd1, 0);
  AFX_CMD_SET_OFFSET(cmd1, reg_play_ctrl);
  AFX_CMD_SET_LENGTH(cmd1, 1);

  aica_chnl_packed_t *keyoff = (aica_chnl_packed_t *)cmd1->values;
  /* Key On = 0, Key Off = 1 to release the note */
  keyoff->play_ctrl.raw = (1 << 14);

  uint32_t cmd1_size = 6 + (AFX_CMD_GET_LENGTH(cmd1) * 2);
  cmd1_size = (cmd1_size + 3) & ~3;
  cursor += cmd1_size;

  uint32_t flow_size = (uint32_t)(cursor - (blob + flow_off));
  hdr->flow_size = flow_size;

  uint32_t flow_spu_addr = afx_upload_afx(blob, (uint32_t)(cursor - blob));
  if (!flow_spu_addr) {
    printf("[SFX] afx_upload_afx failed\n");
    return 0;
  }
  return flow_spu_addr;
}

static void play_sfx(void *data) {
  SPE_state_t *state = (SPE_state_t *)data;

  printf("[SFX] trigger: idx=%d handle=%d\n", state->cursor_pos,
         state->sounds[state->cursor_pos]);
  if (state->sounds[state->cursor_pos] != -1) {

    if (!state->flows[state->cursor_pos]) {
      uint32_t flow_addr =
          create_sfx_flow((uint32_t)state->sounds[state->cursor_pos]);
      if (flow_addr) {
        printf("[SFX] created flow: 0x%08lx\n", (unsigned long)flow_addr);
        state->flows[state->cursor_pos] = flow_addr;
      } else {
        printf("[SFX] play failed: flow create returned 0\n");
        return;
      }
    }

    uint8_t slot = afx_flow_activate(state->flows[state->cursor_pos]);

    printf("[SFX] flow activate returned slot %u\n",
           drv_state_ptr->flow_count_active > 0 ? slot : 0xFFu);
    afx_flow_state_t *flowstate =
        drv_state_ptr->flow_states + slot;
    afx_header_t *hdr =
        (afx_header_t *)(flowstate->afx_base + SPU_RAM_BASE_SH4);
    // if (hdr->magic != AICAF_MAGIC) {
    //   printf("[SFX] invalid flow in slot %d: magic=0x%08X\n", state->cursor_pos,
    //          hdr->magic);
    //   return;
    // }

    afx_flow_play(slot);
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

  const char *longest_line = "Tap X to dump AFX driver state to console";
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
    if (ctrl_states[state->active_controller]->button.X ==
        ENJ_BUTTON_DOWN_THIS_FRAME) {
      afx_driver_state_info(drv_state_ptr, "btn X pressed");
    }
  } while (0);
  enj_render_list_add(PVR_LIST_PT_POLY, render, data);
}

int main(__unused int argc, __unused char **argv) {

  printf("offsetof afx_driver_state_t, flow_states) = %lu\n",
         (unsigned long)offsetof(afx_driver_state_t, flow_states));

  enj_state_init_defaults();
  if (enj_state_startup() != 0) {
    ENJ_DEBUG_PRINT("enDjinn startup failed, exiting\n");
    return -1;
  }

  if (init_afx_driver() != 0) {
    ENJ_DEBUG_PRINT("AFX driver init failed, exiting\n");
    return -1;
  }

  uint32_t song_pos = afx_upload_afx(afx_test, sizeof(afx_test));
  afx_driver_state_info(drv_state_ptr, "after init");
  printf("Uploaded AFX test flow to SPU at 0x%08lx\n", (unsigned long)song_pos);
  uint8_t flow_slot = afx_flow_activate(song_pos);
  afx_flow_play(flow_slot);

  SPE_state_t rat_state = {
      .cursor_pos = 0,
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

  for (int i = 0;
       i < (int)(sizeof(rat_state.sounds) / sizeof(rat_state.sounds[0])); i++) {
    if (rat_state.sounds[i] != -1) {
      afx_sample_free(rat_state.sounds[i]);
    }
  }
  return 0;
}
