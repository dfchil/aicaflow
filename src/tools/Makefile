# Makefile for AICA Flow Host Tools (C23)
CC	  = gcc
CFLAGS  = -std=c2x -O3 -Wall -Wextra -I../../include
LDFLAGS = -lm
OUT_DIR = ../../tools
TOOLS   = $(OUT_DIR)/midi2afx $(OUT_DIR)/afx_info

all: $(OUT_DIR) $(TOOLS)

$(OUT_DIR):
	mkdir -p $(OUT_DIR)

# $(TOOL_DEP):
# 	@echo "Building ARM7 driver blob dependency..."
# 	$(MAKE) -C ../driver/arm7

$(OUT_DIR)/midi2afx: midi2afx.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(OUT_DIR)/afx_info: afx_info.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

clean:
	rm -f $(TOOLS)

.PHONY: all clean