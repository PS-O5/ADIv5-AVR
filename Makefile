MCU      = atmega328p
F_CPU    = 16000000UL
PORT     = /dev/ttyACM0
BAUD     = 115200

CC       = avr-gcc
OBJCOPY  = avr-objcopy
SIZE     = avr-size

CFLAGS   = -mmcu=$(MCU) -DF_CPU=$(F_CPU) -Os -std=gnu11 -Wall -Wextra -Iinclude

SRC      = $(wildcard src/*.c)
OBJ      = $(SRC:.c=.o)
TARGET   = build/firmware

all: $(TARGET).hex

build:
	mkdir -p build

%.o: %.c | build
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET).elf: $(OBJ) | build
	$(CC) $(CFLAGS) $(OBJ) -o $@
	$(SIZE) $@

$(TARGET).hex: $(TARGET).elf
	$(OBJCOPY) -O ihex -R .eeprom $< $@

flash: $(TARGET).hex
	avrdude -c arduino -p $(MCU) -P $(PORT) -b $(BAUD) -D -U flash:w:$<:i

# Build and flash a target image through the shell: make target-flash
BIN  ?= target/blink.bin
ADDR ?= 08000000

target:
	$(MAKE) -C target

target-flash: target
	tools/swdflash $(BIN) $(ADDR)

gdbserver:
	tools/gdbserver

clean:
	rm -rf build src/*.o
	$(MAKE) -C target clean

.PHONY: all build flash clean target target-flash gdbserver
