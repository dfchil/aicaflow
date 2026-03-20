CC      = gcc
CFLAGS  = -std=c2x -O2 -Wall -Wextra -I../include

TARGETS = unit_runtime

all: $(TARGETS)

unit_runtime: test_runtime.c
	$(CC) $(CFLAGS) $< -o $@

run: all
	./unit_runtime

clean:
	rm -f $(TARGETS)
	rm -rf artifacts

.PHONY: all run clean
