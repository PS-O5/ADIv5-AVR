# ADIv5-AVR

An 8-bit AVR that debugs an ARM Cortex-M4. Bit-banged SWD, bare metal, no probe chip and
no level shifter. One resistor and four jumper wires, plus one more for reset.

```
   PC ──USB/115200── ATmega328P ──SWCLK──▶ STM32F411
                     (Arduino Uno)         (Black Pill)
                          │  ◀──SWDIO──▶       │
                       5V/16MHz              3.3V
```

`20.1KB flash · 339B RAM` on the AVR. Halt, step, breakpoints, watchpoints, flash
programming, RTT.

## Why

Every practical SWD host leans on dedicated hardware: an ST-Link, a CMSIS-DAP probe, or at
minimum a level shifter between 5V and 3.3V logic. This removes all of it and implements
ADIv5 directly against the target's 5V-tolerant pins.

## Related work

Bit-banged SWD is not new. [pirate-swd](https://github.com/willdonnelly/pirate-swd)
drives the protocol from a Bus Pirate with the logic in Python on the PC.
scanlime's ESP8266 implementation, [ported to xpcc](https://github.com/ekiwi/xpcc-swd)
for an STM32F3 host, runs the protocol on the microcontroller itself. Black Magic
Probe is the complete version: a GDB server, flash drivers for many families, on a
32-bit STM32.

What is unusual here is the combination. The host is 8-bit, with 2KB of RAM and no
USB. Everything lives on it: the protocol, the Cortex-M debug logic, the flash
driver, XMODEM and the command line. FPB breakpoints, DWT watchpoints and RTT are
not usually on the list of things that fit alongside all of that in 18KB. And there
is no probe silicon and no level shifter in the path, only a resistor.

## Wiring

```
Arduino Uno                    Black Pill
───────────                    ──────────
5V  ────────────────────────── 5V
GND ────────────────────────── GND
D8  (PB0) ──────────────────── PA14  SWCLK
D9  (PB1) ──────┬───────────── PA13  SWDIO
                │
             [2.2kΩ]
                │
                └───────────── 3V3
D10 (PB2) ──────────────────── R     NRST (optional)
```

- PA13/PA14 are 5V-tolerant, which is what makes the direct connection safe.
- SWCLK is push-pull and always an output.
- SWDIO is never driven high. Low is driven, high is released and the resistor pulls it to
  3.3V, so SWDIO never sees 5V and cannot contend with the target.
- A bad ground makes every logic level meaningless and looks exactly like a protocol bug.
- NRST is optional and only needed for `cr`, connect under reset. It is the pin marked `R`
  on the header, between `A0` and `C15`. It is driven the same way as SWDIO, pulled low or
  released, so the internal pull-up needs no resistor here. That is worth keeping even
  though the pin tolerates 5V: NRST is bidirectional, and the reset button and the chip's
  own reset sources also pull it low, so driving it high would be driving into a short.

## Quick start

```sh
make flash                   # build and flash the AVR host
make target-flash            # build target/blink.bin and program the STM32
screen /dev/ttyACM0 115200   # or drive it by hand
```

Or debug it from gdb, which is the same hardware driven through
[tools/gdbserver](tools/gdbserver):

```sh
make gdbserver                                    # listens on localhost:3333
gdb-multiarch target/blink.elf -ex 'target remote localhost:3333'
```

`load`, `break`, `watch`, `stepi`, `continue` and `info registers` all work.
`DEBUG=1` logs every packet and its reply when something needs tracing.

```
> i
DPIDR  0x2BA01477     ARM SW-DP
AP IDR 0x24770011     AHB-AP
CPUID  0x410FC241     Cortex-M4 r0p1
DBGMCU 0x10006431
DEV    0x00000431  rev 0x00001000  STM32F411  flash: ok
```

## Architecture

```mermaid
flowchart TB
    shell["shell.c · command line over UART"]
    xm["xmodem.c · load and save images"]
    rtt["rtt.c · target printf"]
    cortex["cortex.c · halt, step, registers"]
    fpb["fpb.c · breakpoints"]
    dwt["dwt.c · watchpoints"]
    flash["flash.c · erase, program, verify"]
    ap["ap.c · MEM-AP, memory access"]
    dp["dp.c · Debug Port, power, errors"]
    swd["swd.c · bit-banged physical layer"]
    pins(["PB0 SWCLK · PB1 SWDIO"])

    shell --> xm & rtt & cortex & flash
    cortex --> fpb & dwt
    xm --> flash
    rtt --> ap
    cortex --> ap
    flash --> ap
    fpb --> ap
    dwt --> ap
    ap --> dp --> swd --> pins
```

| Layer | Does |
|-------|------|
| `swd.c` | Clock and data bit-banging, turnaround, line reset, transfers |
| `dp.c` | Debug Port: connect, power domains, sticky error recovery |
| `ap.c` | MEM-AP: memory at byte, halfword and word size, block transfers |
| `cortex.c` | Halt, resume, reset-halt, stepping, core registers |
| `fpb.c` `dwt.c` | Hardware breakpoints and data watchpoints |
| `flash.c` | Geometry probe, sector and mass erase, program, verify, option bytes |
| `xmodem.c` `rtt.c` | Image transfer both ways, and target `printf` |
| `shell.c` | The command line |

## Things that cost time

| | |
|---|---|
| Turnaround is asymmetric | The park bit covers host to target. Target to host needs two clocks. |
| One extra clock breaks the ACK | A real OK reads as FAULT, and everything after it desyncs. |
| CSW needs its Prot bits | Without `0x23000000` the AHB access is rejected and STICKYERR latches. |
| Sub-word data rides in byte lanes | A byte at `...03` arrives in bits [31:24], not at the bottom. |
| TAR auto-increment stops at 1KB | Rewrite it at every boundary. |
| Timeouts belong in milliseconds | Poll counts shrank a hundredfold when the SWD clock went up. |
| Halt before touching flash | An erase never finishes while the core fetches from that flash. |
| A locked flash controller lies | Writes to FLASH_CR are ignored and the operation reports success. |
| Sticky errors outlive line resets | Only a write to ABORT clears them. |
| Dropped UART bytes surface late | No receive interrupt meant a lost command failed five steps later. |

Full detail, and the bugs behind each one, in [docs/NOTES.md](docs/NOTES.md).

## Commands

### Shell, over UART at 115200

Addresses and values are hex. Counts, sizes and slots are decimal. `?` prints this list.

| Connect | | Memory | |
|---|---|---|---|
| `c` | connect | `r <addr> [sz]` | read, size 1, 2 or 4 |
| `cr` | connect holding NRST | `w <addr> <val> [sz]` | write, same sizes |
| `force` | allow flash on an unrecognised part | | |
| `i` | DPIDR, AP IDR, CPUID, DBGMCU | `d <addr> [n]` | dump n words |

| Execution | | Breakpoints and watchpoints | |
|---|---|---|---|
| `h` `g` | halt, resume | `b` / `b <addr>` | list, or set a hardware breakpoint |
| `t` `q` | reset and halt, reset and run | `k [slot]` | clear one breakpoint, or all |
| `n [count]` | step one instruction | `a` / `a <addr> [rwb] [len]` | list, or watch read, write or both |
| `m [count]` | step with interrupts masked | `j [slot]` | clear one watchpoint, or all |
| `s` | status, halted and lockup | | |
| `x` / `x <reg> <val>` | core registers: r0-r12, sp, lr, pc, psr | | |

| Flash | | Transfer | |
|---|---|---|---|
| `u` | unlock | `l <addr>` | load a binary over XMODEM |
| `f` | size and sector map | `y <addr> <len>` | save memory over XMODEM |
| `e <sect\|addr>` | erase a sector, or the one holding addr | `rtt [base] [n]` | stream target output |
| `p <addr> <val>` | program a word | | |
| `o` | option bytes and readout protection | | |
| `z` `v` | mass erase, drop protection, both confirm first | | |

### Host tools

```sh
make flash                      # build and program the AVR host
make target                     # build target/blink.bin and target/steal.bin
make target-flash               # program blink to the STM32
make target-flash BIN=target/steal.bin ADDR=08000000
make gdbserver                  # RSP server on localhost:3333
make clean

tools/swdflash <file.bin> [addr]   # what target-flash runs
tools/swdmon [seconds]             # dump whatever the board sends
tools/gdbserver [port]             # defaults to 3333
```

`PORT` overrides the serial device for all three, default `/dev/ttyACM0`. `BOOT_WAIT`
sets how long `swdflash` waits for a prompt, default 20 seconds. `DEBUG=1` makes
`gdbserver` log every packet and reply.

`swdflash` and `gdbserver` both fall back to `cr` when a plain connect or reset fails,
so a target whose firmware has taken the SWD pins is still reachable.

### From gdb

```sh
gdb-multiarch target/blink.elf -ex 'target remote localhost:3333'
```

| Works | |
|---|---|
| `info registers`, `p $pc`, `set $r0 = ...` | r0-r12, sp, lr, pc, xpsr |
| `x/4xw <addr>`, `set {int}<addr> = ...` | byte, halfword and word access |
| `break *<addr>`, `hbreak` | FPB, flash addresses only, six at once |
| `watch`, `rwatch`, `awatch` | DWT, four slots, length honoured |
| `stepi`, `continue`, `Ctrl-C` | |
| `load` | erases, programs and verifies |
| `detach`, reconnect | |

`break` on a flash address picks a hardware breakpoint on its own, because the memory map
marks flash as flash. Software breakpoints are not supported, so `break` on a RAM address
falls back to gdb patching the instruction, and the undefined instruction it writes faults.
The vector catch stops the core there rather than letting it run off, but gdb will not
recognise it as its own breakpoint. Use `hbreak` for code in RAM, or accept that it lands
as a fault. Source level `step` and `next` need a target built with `-g`, which `blink.c`
is not. There is one core and one thread, so thread commands do nothing.

## Footprint

| | Used | Of |
|---|---|---|
| Flash | 20558 B | 32 KB |
| SRAM | 339 B | 2 KB |

String literals live in `PROGMEM`. Before that, `.data` alone was 1396 bytes and there was
barely enough stack left for XMODEM's 128 byte buffer.

## Scope

- Tested against an STM32F411 (Black Pill). The SWD, DP, AP and Cortex-M layers are
  architectural and should hold for any Cortex-M, but `flash.c` is written to the
  F4 flash controller and nothing else has been tried.
- The device id from DBGMCU decides whether the flash commands run at all. `flash.c`
  implements the F4 sector algorithm and nothing else, and pointed at a part with a
  different flash controller it would write plausible values into registers that mean
  something else. An unrecognised part is reported and refused rather than risked. `force`
  overrides that, for porting.
- `tools/gdbserver` speaks the GDB remote serial protocol on the PC side, driving the
  shell underneath. Registers, memory, breakpoints, watchpoints, continue, step and
  `load`, confirmed against real hardware. The memory map, read live from the chip's own
  sector table, marks flash `type="flash"` with a `blocksize` per sector rather than
  `type="rom"`: ROM tells GDB the region cannot be written at all and `load` refuses
  outright with no attempt, where `flash` gets GDB to use its own
  `vFlashErase`/`vFlashWrite`/`vFlashDone` protocol, unlocking, erasing only the sectors
  actually touched, and programming through the real flash driver rather than a raw bus
  write. A plain memory write into the flash range gets the same treatment. Either way,
  bytes outside the requested range within a word are padded with `0xFF` rather than read
  back, since flash programming only clears bits and an all-ones byte changes nothing.
  Everything written is read back and compared, so a successful `load` means the flash
  matches the image, not merely that the writes were sent and acknowledged.
- One target, one AP, no multidrop and no JTAG.
- SWD only, at whatever rate the bit-bang loop manages. Around 4.4KB/s for bulk
  writes, so a 64KB image takes about 15 seconds.
- RDP level 2 is not reachable from here, on purpose. See the option bytes section
  in the notes.

## Layout

```
src/ include/    AVR firmware
target/          a small STM32 blinky, to test the whole chain
                 and steal.c, which takes the SWD pins, to test `cr`
tools/swdflash   host side flasher: drives the shell, sends the image
tools/swdmon     dumps whatever the board sends
tools/gdbserver  GDB remote serial protocol, bridged onto the shell
docs/NOTES.md    engineering notes
hardware/kicad/  schematic and PCB for the wiring
```

## References

- **ARM Debug Interface v5** (ARM IHI 0031A). DP and AP registers, packet format, ACK
  encoding, parity, turnaround, CSW. Predates the JTAG-to-SWD sequence, so `0xE79E` is not
  in it.
- **ARMv7-M Architecture Reference Manual** (ARM DDI 0403). DHCSR, DEMCR, AIRCR, the
  `0xA05F` key, DCRSR and DCRDR, FPB and DWT.
- **STM32F411 reference manual** (RM0383) **and datasheet**. Flash controller, option
  bytes, sector layout, PA13/PA14 defaults, 5V-tolerant pins, BOOT0.
- **ATmega328P datasheet**. DDR, PORT and PIN semantics, USART, U2X baud.

## License / MIT. See [LICENSE](LICENSE).
