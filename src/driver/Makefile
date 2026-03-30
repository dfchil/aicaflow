# Top-level driver Makefile
# This orchestrates ARM7 and SH4 driver subdirectories.

.PHONY: all clean arm7 sh4

all: arm7 sh4

arm7:
	@echo "--- Building ARM7 driver ---"
	$(MAKE) -C arm7

sh4:
	@echo "--- Building SH4 host lib ---"
	$(MAKE) -C sh4

clean:
	@echo "--- Cleaning ARM7 and SH4 ---"
	$(MAKE) -C arm7 clean
	$(MAKE) -C sh4 clean
