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

clean:
	rm -rf build src/*.o

.PHONY: all build flash clean
