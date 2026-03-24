# Makefile for AICA Flow ARM7 Driver (GCC 8 / C99)
# Toolchain: arm-none-eabi-gcc

include $(KOS_BASE)/Makefile.rules

# Driver configuration
# -marm: Use ARM instruction set (ARM7DI)
# -mcpu=arm7di: Specify the processor found in the AICA
# -O2: Optimize for speed
# -std=c99: Use C99 standard for the driver
# -ffreestanding: No standard library
# -nostdlib: Don't link against standard libraries
# -fno-stack-protector: Ensure no stack-related code is generated
ARM_CFLAGS  = -marm -mcpu=arm7di -O2 -std=c99 -Wall -ffreestanding -fno-stack-protector -I../../include

LDFLAGS = -nostdlib -Wl,-Ttext=0x00000000 -Wl,--build-id=none -Wl,-e,reset

SRCS	= afx_driver.c
BUILD_DIR := build
OBJS	= $(SRCS:%.c=$(BUILD_DIR)/%.o)
CRT0_S  = crt0.s
CRT0_O  = $(BUILD_DIR)/crt0.o

LIBDIR= ../../lib
ELF	 = $(BUILD_DIR)/afx_driver.elf

DRIVER=$(BUILD_DIR)/afx_driver.bin
DRIVER_INCLUDE := ../../include/afx/bin/driver_blob.h

HOSTLIB=$(LIBDIR)/libafx.a

all: $(HOSTLIB)

$(CRT0_O): $(CRT0_S)
	@mkdir -p $(BUILD_DIR)
	$(DC_ARM_CC) $(ARM_CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(BUILD_DIR)
	$(DC_ARM_CC) $(ARM_CFLAGS) -c $< -o $@

$(ELF): $(CRT0_O) $(OBJS)
	$(DC_ARM_CC) $(LDFLAGS) $(CRT0_O) $(OBJS) -o $@

$(DRIVER): $(ELF)
	$(DC_ARM_OBJCOPY) -O binary $< $@


$(DRIVER_INCLUDE): $(DRIVER)
	@mkdir -p $(dir $@)
	$(KOS_BASE)/utils/bin2c/bin2c $< $@ afx_driver

$(HOSTLIB): afx_host_lib.c $(DRIVER_INCLUDE)
	@mkdir -p $(LIBDIR)
	$(KOS_CC) $(KOS_CFLAGS) -I../../include -c $< -o $@

clean:
	rm -f $(HOSTLIB) $(DRIVER_INCLUDE)
	rm -rf $(BUILD_DIR)

.PHONY: all clean
