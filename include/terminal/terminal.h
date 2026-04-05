#ifndef TERMINAL_H
#define TERMINAL_H

#include <stdint.h>

#define TERMINAL_WIDTH 79
#define TERMINAL_LINES_PR_SCREEN 30
#define TERMINAL_LINES_CAPACITY 1024 // some extra lines to allow for scrolling
#define TEXT_TMARGIN_LEFT 10


typedef struct {
  char buffer[TERMINAL_LINES_CAPACITY][TERMINAL_WIDTH+1];
  uint8_t line_lengths[TERMINAL_LINES_CAPACITY];
  uint8_t cur_line;
  uint8_t cur_col;
  int32_t auto_scroll; // in pixels, positive means scrolled up
  int32_t user_scroll; // in pixels, positive means scrolled up, overrides auto_scroll when non-zero
} terminal_buffer_t;


void terminal_clear(terminal_buffer_t *term);
void terminal_write(terminal_buffer_t *term, char *str);
void terminal_writeline(terminal_buffer_t *term, char *str);
void terminal_render(void *data);
void terminal_scroll(terminal_buffer_t *term, int32_t pixels);

// terminal_buffer_t *terminal_create(uint8_t char_width, uint8_t char_lines, size_t max_lines);
// void terminal_destroy(terminal_buffer_t *term);

#endif /* TERMINAL_H */