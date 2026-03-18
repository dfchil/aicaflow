# AICA Flow Top-Level Makefile

.PHONY: all clean tools driver help submodules

all: submodules tools driver

submodules:
	@echo "--- Initializing Git Submodules ---"
	git submodule update --init --recursive

tools:
	@echo "--- Building Host Tools (C23) ---"
	$(MAKE) -C src/tools

driver:
	@echo "--- Building ARM7 Driver (C99) ---"
	$(MAKE) -C src/driver

clean:
	@echo "--- Cleaning All Components ---"
	$(MAKE) -C src/tools clean
	$(MAKE) -C src/driver clean
	rm -rf tools/

help:
	@echo "AICA Flow Build System"
	@echo "Targets:"
	@echo "  all     - Builds tools and the ARM7 driver"
	@echo "  tools   - Builds only the MIDI/Sample tools (C23)"
	@echo "  driver  - Builds only the ARM7 sound driver (C99)"
	@echo "  clean   - Removes all build artifacts"
