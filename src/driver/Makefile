# Makefile for AICA Flow ARM7 Driver (GCC 8 / C99)
# Toolchain: arm-none-eabi-gcc

include $(KOS_BASE)/Makefile.rules


AS	  = $(DC_ARM_AS) # Using gcc as assembler wrapper for crt0

# Driver configuration
# -marm: Use ARM instruction set (ARM7DI)
# -mcpu=arm7di: Specify the processor found in the AICA
# -O2: Optimize for speed
# -std=c99: Use C99 standard for the driver
# -ffreestanding: No standard library
# -nostdlib: Don't link against standard libraries
# -fno-stack-protector: Ensure no stack-related code is generated
CFLAGS  = -marm -mcpu=arm7di -O2 -std=c99 -Wall -ffreestanding -fno-stack-protector -I../../include

LDFLAGS = -nostdlib -Wl,-Ttext=0x00000000 -Wl,--build-id=none

SRCS	= afx_driver.c
OBJS	= afx_driver.o
CRT0_S  = crt0.s
CRT0_O  = crt0.o

BINDIR= ../../bin
ELF	 = afx_driver.elf

DRIVER=afx_driver.bin
DRIVER_INCLUDE := ../../include/afx/afx_driver_blob.h

HOSTLIB=$(BINDIR)/afx_host.a

all: $(HOSTLIB)

$(CRT0_O): $(CRT0_S)
	$(DC_ARM_CC) $(CFLAGS) -c $< -o $@

%.o: %.c
	$(DC_ARM_CC) $(CFLAGS) -c $< -o $@

$(ELF): $(CRT0_O) $(OBJS)
	$(DC_ARM_CC) $(LDFLAGS) $(CRT0_O) $(OBJS) -o $@

$(DRIVER): $(ELF)
	$(DC_ARM_OBJCOPY) -O binary $< $@


$(DRIVER_INCLUDE): $(DRIVER)
	$(KOS_BASE)/utils/bin2c/bin2c $< $@ afx_driver

$(HOSTLIB): afx_host_lib.c $(DRIVER_INCLUDE)
	@mkdir -p $(BINDIR)
	$(KOS_CC) $(KOS_CFLAGS) -I../../include -c $< -o $@

clean:
	rm -f $(OBJS) $(CRT0_O) $(ELF) $(DRIVER) $(HOSTLIB) $(DRIVER_INCLUDE)

.PHONY: all clean