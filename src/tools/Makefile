# Makefile for AICA Flow Host Tools (C23)
CC	  = gcc
CFLAGS  = -std=c2x -O3 -Wall -Wextra -I../../include
LDFLAGS = -lm
OUT_DIR = ../../tools
TOOLS   = $(OUT_DIR)/midi2aicaflow $(OUT_DIR)/afx_info
UTILS   = aicaflow_utils.c

all: $(OUT_DIR) $(TOOLS)

$(OUT_DIR):
	mkdir -p $(OUT_DIR)

$(OUT_DIR)/midi2aicaflow: midi2aicaflow.c $(UTILS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(OUT_DIR)/afx_info: afx_info.c $(UTILS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

clean:
	rm -f $(TOOLS)

.PHONY: all clean