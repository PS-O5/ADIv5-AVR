# ADIv5-AVR

A bare-metal SWD (Serial Wire Debug) programmer/debugger host, built from scratch on an
ATmega328P (Arduino Uno), used to talk to an STM32F411CEU6 (WeAct Black Pill) target —
with zero external components. No level shifters, no resistors. Just wires.

## Why

Most SWD host implementations lean on a dedicated debug probe chip (ST-Link, CMSIS-DAP,
J-Link) or at least a level shifter between the 5V AVR and the 3.3V STM32. This project
strips that away to see how far the ADIv5 protocol can be implemented directly against
silicon, using only the target's 5V-tolerant GPIOs and AVR register-level bit-banging.

## Hardware

- **Host**: Arduino Uno (ATmega328P @ 16MHz, 5V logic)
- **Target**: STM32F411CEU6 / WeAct Black Pill (3.3V logic, powered from Uno 5V via
  onboard LDO)

### Wiring

| Uno Pin       | STM32 Pin | Function       |
|---------------|-----------|----------------|
| 5V            | 5V        | Power          |
| GND           | GND       | Ground         |
| PB0 (D8)      | PA14      | SWCLK          |
| PB1 (D9)      | PA13      | SWDIO          |
| PB5 (D13)     | —         | Status LED     |

### Electrical notes

- PA13/PA14 are 5V-tolerant (FT) pins on the STM32F411, which is what makes direct
  connection to 5V AVR GPIOs safe.
- SWCLK is host-driven push-pull, always an output.
- SWDIO is bidirectional. The AVR never drives it HIGH — HIGH is released via a
  pseudo open-drain scheme (DDR switched to input with internal pull-up enabled),
  LOW is driven directly. This avoids bus contention when the target drives SWDIO
  during a read turnaround.

## Toolchain

- `avr-gcc` / `avr-libc`
- `avrdude` for flashing
- Bare-metal C — no Arduino core, no framework

## Status

Work in progress, built up from scratch. See commit history for progress; concepts
and protocol details are documented alongside the code as they're implemented.
