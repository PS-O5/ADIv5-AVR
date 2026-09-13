CC      = arm-none-eabi-gcc
OBJCOPY = arm-none-eabi-objcopy
SIZE    = arm-none-eabi-size

CFLAGS  = -mcpu=cortex-m4 -mthumb -Os -Wall -Wextra -ffreestanding \
          -fno-builtin -nostdlib
LDFLAGS = -T blink.ld -Wl,--gc-sections

all: blink.bin steal.bin

# Pattern rules treat the .elf files as intermediate and delete them, but gdb
# needs them for symbols.
.PRECIOUS: %.elf

%.elf: %.c blink.ld
	$(CC) $(CFLAGS) $(LDFLAGS) $< -o $@
	$(SIZE) $@

%.bin: %.elf
	$(OBJCOPY) -O binary $< $@

clean:
	rm -f blink.elf blink.bin steal.elf steal.bin

.PHONY: all clean
