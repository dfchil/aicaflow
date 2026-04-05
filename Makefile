# AICA Flow Top-Level Makefile

.PHONY: all clean tools driver test test-unit test-integration help submodules examples

all: driver driver submodules terminal tools examples 

terminal:
	@echo "--- Building Terminal Library ---"
	$(MAKE) -C src/terminal

examples:
	@echo "--- Building Examples ---"
	$(MAKE) -C examples/simple

submodules:
	@echo "--- Initializing Git Submodules ---"
	git submodule update --init --recursive

tools: driver
	@echo "--- Building Host Tools (C23) ---"
	$(MAKE) -C src/tools

driver:
	@echo "--- Building ARM7 Driver (C99) ---"
	$(MAKE) -C src/driver

test: test-unit test-integration

test-unit:
	@echo "--- Running Unit Tests ---"
	$(MAKE) -C tests run

test-integration:
	@echo "--- Running Integration Tests ---"
	bash tests/run_integration_tests.sh

clean:
	@echo "--- Cleaning All Components ---"
	$(MAKE) -C src/tools clean
	$(MAKE) -C src/driver clean
	$(MAKE) -C tests clean
	$(MAKE) -C examples/simple clean

help:
	@echo "AICA Flow Build System"
	@echo "Targets:"
	@echo "  all     - Builds tools and the ARM7 driver"
	@echo "  tools   - Builds only the MIDI/Sample tools (C23)"
	@echo "  driver  - Builds only the ARM7 sound driver (C99)"
	@echo "  test    - Runs unit and integration tests"
	@echo "  clean   - Removes all build artifacts"
