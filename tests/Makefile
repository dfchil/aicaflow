CC      = gcc
CFLAGS  = -std=c2x -O2 -Wall -Wextra -I../include

TARGETS = unit_v2

all: $(TARGETS)

unit_v2: test_v2_runtime.c
	$(CC) $(CFLAGS) $< -o $@

run: all
	./unit_v2

clean:
	rm -f $(TARGETS)
	rm -rf artifacts

.PHONY: all run clean
