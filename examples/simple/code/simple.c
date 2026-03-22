#include <enDjinn/enj_enDjinn.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <afx/afx_driver_blob.h>


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
    uint32_t reserved : 20;
  };
} SPE_state_t;

static void play_sfx(void* data) {
  // SPE_state_t* state = (SPE_state_t*)data;
}
static const SFX_menu_entry_t sfx_catalog[] = {
    {.name = "Bach - Cello Suite No. 1 in G Major", .on_press_A = play_sfx},
    {.name = "Chopin - Nocturnes Opus 27, No. 1 in C# Minor", .on_press_A = play_sfx},
    {.name = "Grieg - In The Hall of The Mountain King", .on_press_A = play_sfx},
    {.name = "Exit example", .on_press_A = enj_state_flag_shutdown},
};
static const int num_sfx_menu_entries = sizeof(sfx_catalog) / sizeof(sfx_catalog[0]);

#define MARGIN_LEFT 30
void render(void* data) {
  SPE_state_t* state = (SPE_state_t*)data;
  enj_qfont_color_set(0x14, 0xaf, 255); /* Light Blue */
  enj_font_scale_set(3);
  const char* title = "Music Playback Example";
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
  // enj_qfont_write(longest_line, textpos_x, textpos_y, PVR_LIST_PT_POLY);
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
