CC      = arm-none-eabi-gcc
OBJCOPY = arm-none-eabi-objcopy
SIZE    = arm-none-eabi-size

CFLAGS  = -mcpu=cortex-m4 -mthumb -Os -Wall -Wextra -ffreestanding \
          -fno-builtin -nostdlib
LDFLAGS = -T blink.ld -Wl,--gc-sections

all: blink.bin

blink.elf: blink.c blink.ld
	$(CC) $(CFLAGS) $(LDFLAGS) blink.c -o $@
	$(SIZE) $@

blink.bin: blink.elf
	$(OBJCOPY) -O binary $< $@

clean:
	rm -f blink.elf blink.bin

.PHONY: all clean
