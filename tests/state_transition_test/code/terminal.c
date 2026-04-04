#include <enDjinn/enj_enDjinn.h>
#include <terminal.h>

void terminal_clear(terminal_buffer_t *term) {
  for (int y = 0; y < TERMINAL_LINES_CAPACITY; y++) {
    term->line_lengths[y] = 0;
  }
  term->cur_line = 0;
  term->cur_col = 0;
  term->auto_scroll = 0;
  term->user_scroll = 0;
}

static inline void terminal_newline(terminal_buffer_t *term, char *str) {
  term->line_lengths[term->cur_line] = term->cur_col;
  term->cur_line = (term->cur_line + 1) % TERMINAL_LINES_CAPACITY;
  term->cur_col = 0;
  term->auto_scroll++;
}

void terminal_write(terminal_buffer_t *term, char *str) {
  while (*str != '\0') {
    if (*str == '\n' || term->cur_col >= TERMINAL_WIDTH) {
      terminal_newline(term, str);
      if (*str != '\n') {
        // search for previous space to break line, to avoid breaking words when
        // possible
        char *prev_space = str;
        uint8_t prev_space_col = term->line_lengths[term->cur_line - 1];
        while (*prev_space != ' ' && prev_space_col > 0) {
          prev_space--;
          prev_space_col--;
        }
        if (*prev_space == ' ' && prev_space_col > 0) {
          str = prev_space; // break at previous space if found, otherwise break
                            // at current position
          term->line_lengths[term->cur_line - 1] =
              prev_space_col; // adjust previous line length if we broke at a
                              // previous space
        }
        continue;
      }
    } else {
      term->buffer[term->cur_line][term->cur_col] = *str;
      term->cur_col++;
    }
    str++;
  }
  term->line_lengths[term->cur_line] = term->cur_col;
}

void terminal_writeline(terminal_buffer_t *term, char *str) {
  printf("%s\n", str); // also print to stdout for debugging
  terminal_write(term, str);
  terminal_newline(term, str);
}
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

void terminal_render(void *data) {
  terminal_buffer_t *term = (terminal_buffer_t *)data;
  enj_font_header_t *font_hdr = enj_qfont_get_header();

  int scroll = (MAX(0, term->auto_scroll - TERMINAL_LINES_PR_SCREEN) *
                font_hdr->line_height) +
               term->user_scroll;

  int first_line = scroll / font_hdr->line_height;
  int last_line = first_line + TERMINAL_LINES_PR_SCREEN + 1;

  int render_offset_y = -(scroll % font_hdr->line_height);
  for (int y = first_line; y < last_line; y++) {
    if (term->line_lengths[y] > 0) {
      term->buffer[y][term->line_lengths[y]] = '\0';
      enj_qfont_write(term->buffer[y], TEXT_TMARGIN_LEFT, render_offset_y,
                      PVR_LIST_PT_POLY);
    }
    render_offset_y += font_hdr->line_height;
  }
}

void terminal_scroll(terminal_buffer_t *term, int32_t pixels) {
  term->user_scroll += pixels;
  if (term->user_scroll > 0) {
    term->user_scroll = 0;
  }
  int32_t auto_scroll_limit =
      -( MAX(0, term->auto_scroll - TERMINAL_LINES_PR_SCREEN) * enj_qfont_get_header()->line_height);

  if (term->user_scroll < auto_scroll_limit) {
    // limit user scroll to not scroll past the point where auto scroll is
    term->user_scroll = auto_scroll_limit;
  }
}

// terminal_buffer_t *terminal_create(uint8_t char_width, uint8_t char_lines,
// size_t max_lines)
// {

// }

// void terminal_destroy(terminal_buffer_t *term) {
//   free(term);
// }
