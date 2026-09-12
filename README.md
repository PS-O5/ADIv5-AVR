# ADIv5-AVR

A bare-metal SWD (Serial Wire Debug) host built from scratch on an ATmega328P (Arduino
Uno), talking to an STM32F411CEU6 (WeAct Black Pill). No debug probe chip, no level
shifter. One resistor and four jumper wires.

## Why

Every practical SWD host leans on dedicated hardware: an ST-Link, a CMSIS-DAP probe, a
J-Link, or at minimum a level shifter between 5V and 3.3V logic. This project removes all
of that and implements ADIv5 directly, bit by bit, against the target's 5V-tolerant pins.

## Hardware

- **Host**: Arduino Uno, ATmega328P at 16MHz, 5V logic
- **Target**: STM32F411CEU6 (WeAct Black Pill), 3.3V logic, powered from the Uno's 5V rail
  through the board's onboard LDO

### Wiring

| Uno Pin       | STM32 Pin | Function       |
|---------------|-----------|----------------|
| 5V            | 5V        | Power          |
| GND           | GND       | Ground         |
| PB0 (D8)      | PA14      | SWCLK          |
| PB1 (D9)      | PA13      | SWDIO          |
| PB5 (D13)     | *n/a*     | Status LED     |

Plus one 2.2kΩ resistor from PA13 (SWDIO) to the Black Pill's 3.3V pin.

### Electrical notes

PA13 and PA14 are 5V-tolerant (FT) pins, which is what makes a direct connection to 5V
AVR GPIOs safe. SWCLK is always an output, driven push-pull, swinging the full 5V.

SWDIO is bidirectional and the AVR never drives it high. Low is driven directly. High is
released: the AVR switches SWDIO to a floating input and the 2.2kΩ resistor pulls the line
to 3.3V on its own. So SWDIO never sees 5V at all, and there is no contention when the
target drives the line during a turnaround.

The shared ground matters more than it looks. With a bad GND link the two boards have no
common reference and every logic level on the wire is meaningless, which produces symptoms
that look like protocol bugs.

## Build and flash

```
make            # build
make flash      # flash via avrdude (set PORT in the Makefile)
```

Requires `avr-gcc`, `avr-libc` and `avrdude`. No Arduino core, no framework.

Diagnostics come out over UART at 115200 baud:

```
screen /dev/ttyACM0 115200
```

## Code layout

| File | Role |
|------|------|
| `src/swd.c` | Physical layer. Clock/data bit-banging, turnaround, line reset, connect sequence, and the generic DP/AP transfer. |
| `src/dp.c` | Debug Port. Register reads/writes and the debug/system power-up handshake. |
| `src/ap.c` | Access Port. AP register access and MEM-AP setup. |
| `src/uart.c` | Minimal UART transmit and hex printing. Hand-rolled instead of `stdio.h`, which would cost over a kilobyte of flash. |

## Protocol notes

Things that cost real time to get right, recorded here so they don't have to be
rediscovered.

**Everything is LSB first.** The request byte, the ACK field, the 32-bit data, all of it.

**The request byte** is 8 bits: Start (1), APnDP, RnW, A2, A3, Parity, Stop (0), Park (1).
Parity is the XOR of APnDP, RnW, A2 and A3. Reading IDCODE works out to `0xA5`.

**Connect sequence**: idle low, 56 clocks with SWDIO high (line reset), the 16-bit
JTAG-to-SWD value `0xE79E` LSB first, another 56-clock line reset, then idle low. The
first transaction afterwards must be an IDCODE read. The spec is explicit that this is
what confirms packet frame alignment.

**Turnaround is asymmetric, and this is the part that bites.** Host to target needs no
extra clock, because the park bit already leaves the line undriven for a cycle. Target to
host needs two. One clock is the protocol turnaround and the second covers the phase
offset between the target releasing SWDIO and this bit-bang loop picking it up.

Getting that wrong is nasty because the failure does not look like a timing problem. With
one clock too many before the ACK, the host samples ACK[1], ACK[2] and then the first data
bit. A real OK response of `100` plus IDCODE bit 0 (always 1) reads back as `001`, which
decodes as FAULT. The host then skips the data phase while the target is still clocking
out 31 more bits, and every transaction after that is desynced. The result is a failure
that is perfectly reproducible, independent of which register you touch and independent of
how long you wait, which sends you looking in all the wrong places.

The write side has its own tell. Too few or too many turnaround clocks corrupts the write
data phase, and the target reports it: WDATAERR (bit 7) shows up in CTRL/STAT. That flag
is a reliable way to check the count without guessing, since it is the target's own
verdict on the data it received.

**Idle cycles are driven low after every transaction.** The host never drives SWDIO high,
so the target can only frame a start bit if the line was actively low beforehand. Leaving
it floating high between packets means the start bit produces no edge and the target never
sees a new packet.

**AP reads are posted.** An AP read returns the result of the *previous* AP access, so
every `ap_read()` follows up with a read of the DP RDBUFF register to collect the value it
actually asked for.

## Target gotchas

If firmware on the STM32 reconfigures PA13/PA14 as ordinary GPIO, the SWD function is
disconnected from those pins and the debug port is unreachable. The pin ends up in a
tug-of-war with the host: driving it low reads as roughly 1.8V instead of 0V, which is a
quick way to spot the problem.

Two ways out. Hold BOOT0 during a power-on reset so the chip runs the ROM system
bootloader, which never touches those pins. Or erase the flash so there is no firmware
left to reconfigure them.

## Status

DP and MEM-AP are up. Current output:

```
--- ADIv5-AVR ---
IDCODE    ack=0x01 data=0x2BA01477
PowerUp   ack=0x01
CTRL/STAT ack=0x01 data=0xF0000000
CSW write ack=0x01
AP select ack=0x01
AP IDR    ack=0x01 data=0x24770011
--- done ---
```

`0x2BA01477` is the ARM SW-DP. `0xF0000000` is both power domains requested and
acknowledged (CSYSPWRUPREQ/ACK and CDBGPWRUPREQ/ACK). `0x24770011` decodes as JEP-106
identity 0x3B (ARM), class 0x8 (MEM-AP), type 0x1 (AMBA AHB), which is the AHB-AP.

Next: memory reads and writes through TAR and DRW, then halting the core via DHCSR.

## References

- **ARM Debug Interface v5 Architecture Specification** (ARM IHI 0031A). The primary
  reference. DP and AP register maps, packet request format, ACK encoding, parity rules,
  turnaround definition, CTRL/STAT and MEM-AP CSW bit assignments, and the sticky error
  flags. Note that this revision predates the JTAG-to-SWD switch sequence, so `0xE79E` is
  not documented in it.
- **STM32F411xC/E reference manual** (RM0383) **and datasheet**. PA13/PA14 defaulting to
  the SWD alternate function out of reset, the 5V-tolerant pin list, and BOOT0 boot mode
  selection.
- **ATmega328P datasheet**. DDRB/PORTB/PINB semantics including the input pull-up, USART
  registers, and the U2X baud rate calculation.
