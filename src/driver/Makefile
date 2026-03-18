# Makefile for AICA Flow ARM7 Driver (GCC 8 / C99)
# Toolchain: arm-none-eabi-gcc

include $(KOS_BASE)/Makefile.rules


CC	  = $(DC_ARM_CC)
OBJCOPY = $(DC_ARM_OBJCOPY)
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

SRCS	= aica_driver.c
OBJS	= aica_driver.o
CRT0_S  = crt0.s
CRT0_O  = crt0.o

TARGET  = aica_driver.bin
ELF	 = aica_driver.elf

all: $(TARGET)

$(CRT0_O): $(CRT0_S)
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

$(ELF): $(CRT0_O) $(OBJS)
	$(CC) $(LDFLAGS) $(CRT0_O) $(OBJS) -o $@

$(TARGET): $(ELF)
	$(OBJCOPY) -O binary $< $@

clean:
	rm -f $(OBJS) $(CRT0_O) $(ELF) $(TARGET)

.PHONY: all clean