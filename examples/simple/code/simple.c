#include <dc/maple/purupuru.h>
#include <enDjinn/enj_enDjinn.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <afx/afx_driver.h>

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
    void (*on_press_A)(void* data);
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
  sfxhnd_t sounds[6];
} SPE_state_t;

static void play_sfx(void* data) {
  SPE_state_t* state = (SPE_state_t*)data;
  if (state->sounds[state->cursor_pos] != SFXHND_INVALID) {
    enj_sound_play(state->sounds[state->cursor_pos], 192, state->pan);
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
static const int num_sfx_menu_entries = sizeof(sfx_catalog) / sizeof(sfx_catalog[0]);

#define MARGIN_LEFT 30
void render(void* data) {
  SPE_state_t* state = (SPE_state_t*)data;
  enj_qfont_color_set(0x14, 0xaf, 255); /* Light Blue */
  enj_font_scale_set(3);
  const char* title = "Sound Playback Example";
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

  const char* longest_line =
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

void main_mode_updater(void* data) {
  do {
    SPE_state_t* state = (SPE_state_t*)data;
    enj_ctrlr_state_t** ctrl_states = enj_ctrl_get_states();
    int delta = ctrl_states[state->active_controller]->button.UP ==
                        ENJ_BUTTON_DOWN_THIS_FRAME
                    ? -1
                : ctrl_states[state->active_controller]->button.DOWN ==
                        ENJ_BUTTON_DOWN_THIS_FRAME
                    ? 1
                    : 0;
    if (delta) {
      state->cursor_pos = (state->cursor_pos + delta) % num_sfx_menu_entries;
      if (state->cursor_pos < 0) state->cursor_pos = num_sfx_menu_entries - 1;
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

int main(__unused int argc, __unused char** argv) {
  enj_state_init_defaults();
  if (enj_state_startup() != 0) {
    ENJ_DEBUG_PRINT("enDjinn startup failed, exiting\n");
    return -1;
  }
  SPE_state_t rat_state = {
      .cursor_pos = 0,
      .pan = 128,
      .sounds =
          {
              enj_sound_dca_load_blob(wilhelm_adpcm_data),
              enj_sound_dca_load_blob(wilhelm_pcm8_data),
              enj_sound_dca_load_blob(wilhelm_pcm16_data),
              enj_sound_dca_load_blob(clean_test_adpcm),
              enj_sound_dca_load_blob(clean_test_pcm8),
              enj_sound_dca_load_blob(clean_test_pcm16),
          },
  };
  enj_mode_t main_mode = {
      .name = "Main Mode",
      .mode_updater = main_mode_updater,
      .data = &rat_state,
  };
  enj_mode_push(&main_mode);
  enj_state_run();
  return 0;
}
