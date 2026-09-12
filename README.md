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

The firmware presents a shell over UART at 115200 baud:

```
screen /dev/ttyACM0 115200
```

## Flashing a real image

`target/` holds a small blinky for the STM32, and `tools/swdflash` sends any raw binary
through the shell:

```
make target-flash                        # builds target/blink.bin and sends it
tools/swdflash firmware.bin 08000000     # or send your own
PORT=/dev/ttyUSB0 tools/swdflash fw.bin  # PORT overrides the default
```

The tool drives the same commands you would type, then sends the file itself rather than
handing the port to `sx`. That matters because the shell echoes what it receives, and a
separate sender reads that echo as protocol bytes and gives up. Doing the transfer in one
place avoids the problem entirely.

It waits for the shell's prompt rather than sleeping for a guessed interval, and nudges
with a bare line to get one. Sleeping races two different things: the bootloader, which
runs first and eats anything sent before the firmware exists, and the board not resetting
at all, which is what happens once something has cleared HUPCL on the port.

The blinky is 84 bytes and has no startup code, because nothing in it has an initialiser
to copy and nothing lives in `.bss`, so the reset vector points straight at the loop. Its
vector table is the same two words we were writing by hand earlier: the initial stack
pointer and the reset vector with bit 0 set for Thumb.

### Checking it ran, without watching the LED

Peripheral registers sit at their reset values until something writes them, so the target's
own state is the proof:

```
> r 40023830        RCC_AHB1ENR, bit 2 set means our code enabled the GPIOC clock
0x40023830: 00000004
> r 40020814        GPIOC_ODR, bit 13 flips as the loop runs
0x40020814: 00002000
> r 40020814
0x40020814: 00000000
```

`RCC_AHB1ENR` resets to zero, so bit 2 being set shows the initialisation ran. The output
register changing between two reads shows the loop is executing right now. Reading the
same value twice in a row is normal rather than a failure: the blink half period is about
100ms and a read takes a few milliseconds, so consecutive reads usually land inside the
same half period.

## Command guide

It connects on startup and gives a `>` prompt. Addresses and values are hex, with or
without `0x`. Counts, sizes, register names and slot numbers are decimal. A command is a
single letter apart from `rtt`, and a token that matches nothing is rejected rather than
guessed at, so `pc 20000000` is an error instead of being read as `p` with an argument of
`c`. Single letters ran out at twenty six commands, which is why longer names are matched
whole rather than by their first character.

**Connection and identity**

| | |
|---|---|
| `c` | connect: line reset, switch sequence, power up, MEM-AP, FPB, DWT, flash probe |
| `i` | DPIDR, AP IDR, CPUID, DBGMCU |
| `s` | DHCSR, with halted and lockup decoded |

**Memory**

| | |
|---|---|
| `r <addr> [sz]` | read one value, size 1, 2 or 4 bytes, default 4 |
| `w <addr> <val> [sz]` | write one value, same sizes |
| `d <addr> [n]` | dump n words, four per line, default 8 |

**Execution**

| | |
|---|---|
| `h` | halt |
| `g` | resume |
| `t` | reset and halt, catching the core at the vector fetch |
| `q` | reset and run, letting the target boot its own firmware |
| `n [count]` | step one instruction, or count of them |
| `m [count]` | step with interrupts masked |
| `x` | show all core registers, halted only |
| `x <reg> <val>` | write a register: `r0` to `r12`, `sp`, `lr`, `pc`, `psr` |

**Breakpoints and watchpoints**

| | |
|---|---|
| `b` | list breakpoints, with the FPB revision and slot count |
| `b <addr>` | set a hardware breakpoint in the first free slot |
| `k [slot]` | clear one breakpoint, or all of them |
| `a` | list watchpoints |
| `a <addr> [r\|w\|b]` | watch an address on read, write, or both, default both |
| `j [slot]` | clear one watchpoint, or all of them |

**Flash**

| | |
|---|---|
| `f` | flash size and sector map |
| `u` | unlock the flash controller |
| `e <sector\|addr>` | erase a sector, by number or by an address inside it |
| `p <addr> <val>` | program one flash word |
| `o` | option bytes and readout protection level |
| `z` | mass erase, asks for confirmation |
| `v` | drop readout protection, asks for confirmation |

**Transfer**

| | |
|---|---|
| `l <addr>` | receive a binary over XMODEM and program it |
| `y <addr> <len>` | send memory over XMODEM to a file |
| `rtt [base] [len]` | stream RTT output from the target |

### A worked session

Program a small routine into flash, break on it, and inspect:

```
> u
> e 0
> p 08000000 20001000     vector table: initial stack pointer
> p 08000004 08000009     reset vector, thumb bit set
> p 08000008 BF00BF00     nop, nop
> p 0800000C E7FEBF00     nop, branch to self
> t
> b 0800000A
> g
> x
```

Halting first matters more than it looks. Memory access works on a running core because it
goes through the AP, but register access does not: reading or writing a core register while
the core runs is UNPREDICTABLE, and erasing flash underneath a core that is fetching from
it never completes.

### Reading the debug registers

```
> i
DPIDR  0x2BA01477
AP IDR 0x24770011
CPUID  0x410FC241
DBGMCU 0x10006431
> d E000ED00 4
0xE000ED00: 410FC241 00000803 00000000 FA050000
> t
ok
> s
DHCSR 0x00030003  halted=y lockup=n
```

That dump is CPUID, ICSR, VTOR and AIRCR. Two things in it are worth knowing. AIRCR reads
back `0xFA05` in its top half although writes need `0x05FA`, so a read-modify-write cannot
accidentally supply the key. And ICSR `0x803` is VECTACTIVE=3, a HardFault, which is what
a blank chip does: it fetches `0xFFFFFFFF` for both the stack pointer and the reset vector
and faults immediately.

### Stepping

`C_STEP` in DHCSR runs exactly one instruction. `C_HALT` has to be clear at the same time,
so the core leaves debug state, retires one instruction and halts again. `m` also sets
`C_MASKINTS`, which stops a pending exception from stealing the step and landing you in a
handler instead of the next instruction.

Writing a few instructions into SRAM is an easy way to watch it work. `0xBF00` is `NOP`
and `0xE7FE` is a branch to itself:

```
> w 20000000 BF00BF00
> w 20000004 E7FEBF00
> x pc 20000000
> x sp 20001000
> n
pc 20000002  psr 01000000  exc 0
> n
pc 20000004  psr 01000000  exc 0
> n
pc 20000006  psr 01000000  exc 0
> n
pc 20000006  psr 01000000  exc 0
```

The PC advances two bytes per instruction and then stops moving, because the last one
branches to itself. Set the stack pointer somewhere clear of the code before running
anything.

### Breakpoints

The FPB unit at `0xE0002000` provides hardware breakpoints, so nothing has to be patched
into the code and they work in flash. `FP_CTRL` reports the revision in bits [31:28] and
the comparator count split awkwardly across bits [7:4] and [14:12]. `ENABLE` only takes
effect if `KEY` is written with it.

Revision matters. Version 1 comparators match a word and pick a halfword with the REPLACE
field, which limits them to the code region below `0x20000000`. Version 2 takes a plain
address and covers everything. The driver reads the revision and formats the comparator to
match, and refuses out-of-range addresses on v1 rather than quietly setting a breakpoint
that can never fire. The STM32F411 has v1 with six comparators.

Breaking on code in flash, with a vector table and three NOPs programmed at `0x08000000`:

```
> t
> b 0800000A
breakpoint 0 at 0x0800000A
> g
ok
> s
DHCSR 0x01030003  halted=y lockup=n
> x
... pc 0800000A
```

The core ran from the reset vector, executed the first NOP, and halted before executing
the instruction at the breakpoint address.

### Watchpoints

The DWT unit at `0xE0001000` halts on a data access rather than an instruction address,
which is what catches memory being corrupted by code you have not identified yet. Each
comparator is a 16 byte block of COMP, MASK and FUNCTION, and NUMCOMP in the top nibble of
DWT_CTRL says how many there are. The F411 has four.

The unit is gated behind TRCENA, bit 24 of DEMCR, and does nothing at all until that is
set. Arming a watchpoint without it looks like it worked and never fires. MASK is how many
low address bits to ignore, and is left at zero here so the match is on the exact address.

Watching a word, then running a routine that stores to it:

```
> a 20000000 w
watchpoint 0 on write at 0x20000000
> g
> s
DHCSR 0x01030003  halted=y lockup=n
> x
r0  000000AA  r1  20000000 ... pc  20000108
> r 20000000
0x20000000: 000000AA
> a
4 watchpoint slots
  0  write  0x20000000  matched
```

The core halted just past the store, the value made it to memory, and the comparator
reports that it matched. Nothing was watching the PC.

### Loading a binary

`l <addr>` receives a raw binary over XMODEM and programs it as it arrives, one 128-byte
block at a time. XMODEM rather than something custom, so ordinary tools can send the file
with no host script. The target is halted first, and sticky errors are cleared afterwards
so a programming fault does not leave the link dead.

Unlock first. Sectors are erased as the transfer reaches them, and every block is read
back and compared before the next is accepted.

```
> u
> l 08000000
```

Erasing happens on entering a sector rather than up front, because XMODEM does not say how
long the file is until it ends. The stream only moves forwards, so a sector being entered
has not been written to yet. Note this erases whole sectors, so a load starting partway
into one still discards what came before it in that sector.

Verifying matters more than it sounds. Without it a load can report success while the flash
holds something else, and the only way to find out is to read it back and compare by hand.

Then send the file. In minicom that is `Ctrl-A S`, pick xmodem, space to mark the file,
enter to send. The receiver NAKs once a second for a minute waiting for the sender, so
there is time to find the file. Afterwards minicom shows the transfer result in its own
window and waits for a keypress, which looks like a hang and is not.

Verify with a dump:

```
> d 08000000 8
0x08000000: B0000000 B0000001 B0000002 B0000003
0x08000010: B0000004 B0000005 B0000006 B0000007
```

### RTT

RTT is SEGGER's trick for getting `printf` off a target without a spare pin: the firmware
leaves a control block in RAM holding ring buffers, and the host reads them over the debug
link like any other memory. Everything it needs is already here, so this costs no extra
wire and no target stub beyond the library the firmware already links.

The control block starts with `"SEGGER RTT"` padded to sixteen bytes, then the buffer
counts, then descriptors of name, pointer, size, write offset and read offset. `rtt` scans
RAM for that ID, then polls the first up buffer, printing whatever appears until a key is
pressed.

```
> rtt
searching RAM for the control block
control block at 0x20001000, 1 up buffers
press a key to stop
Hello from RTT!
```

The host owns the read offset and writes it back after draining, which is how the target
knows the space is free again. Skip that and the buffer fills and stalls. The scan defaults
to the 128KB of SRAM at `0x20000000` and takes a couple of seconds; passing a base and
length narrows it.

### Reading memory back out

`y <addr> <len>` sends a region the other way, so an image can be pulled off the target
into a file and compared against what was meant to be there. In minicom that is `Ctrl-A R`.

```
> y 08000000 100
start the receiver now
256 bytes from 0x08000000: ok
```

Receivers open with `C` to ask for CRC and fall back to `NAK` for the plain checksum, so
the sender answers to whichever arrives.

Block reads are pipelined. An AP read is posted, meaning a DRW read returns the previous
access while starting the next, so priming once and taking the last value from RDBUFF
costs about one transaction per word instead of three.

## Code layout

| File | Role |
|------|------|
| `src/swd.c` | Physical layer. Clock/data bit-banging, turnaround, line reset, connect sequence, and the generic DP/AP transfer. |
| `src/dp.c` | Debug Port. Register reads/writes and the debug/system power-up handshake. |
| `src/ap.c` | Access Port. AP register access and MEM-AP setup. |
| `src/cortex.c` | ARMv7-M debug. Halt, resume, reset-halt, stepping and core registers through DHCSR, DEMCR, AIRCR and DCRSR/DCRDR. |
| `src/fpb.c` | Hardware breakpoints through the FPB unit, handling both comparator formats. |
| `src/dwt.c` | Data watchpoints through the DWT unit. |
| `src/rtt.c` | Finds the SEGGER RTT control block in target RAM and drains its ring buffers. |
| `target/` | A small STM32 blinky, for testing the whole chain against real firmware. |
| `tools/swdflash` | Host side flasher: drives the shell and sends a binary over XMODEM. |
| `tools/swdmon` | Dumps whatever the board sends, for when the flasher itself is suspect. |
| `src/flash.c` | STM32F4 flash controller. Unlock, word programming, sector erase. |
| `src/shell.c` | The UART command shell. String literals live in `PROGMEM` so they cost flash rather than the 2KB of SRAM. |
| `src/xmodem.c` | XMODEM receive, programming each block into flash as it arrives. |
| `src/uart.c` | Minimal UART transmit, receive and hex printing. Hand-rolled instead of `stdio.h`, which would cost over a kilobyte of flash. |

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

**Sub-word accesses ride in byte lanes.** The CSW Size field selects byte, halfword or
word, and for anything narrower than a word the data sits in the DRW lane matching the
address rather than at the bottom of the register. A byte at `...03` arrives in bits
[31:24]. Ignore that and every byte read returns the one at offset 0. Size is also allowed
to be read-only, so the driver reads CSW back after changing it rather than assuming the
write took.

**CSW needs its Prot bits set.** Size and auto-increment alone are not enough. Bits
[30:24] are IMPLEMENTATION DEFINED in ADIv5, so the spec cannot tell you what to put
there. For a Cortex-M AHB-AP they carry MasterType=Debug (bit 29) and HPROT[1]=privileged
(bit 25), with bit 24 set by convention, giving `0x23000000`. Leave them clear and the AHB
transaction is rejected, STICKYERR latches, and every AP access afterwards returns FAULT
until it is cleared. The DP-level accesses keep working throughout, which makes it look
like a memory problem rather than a configuration one.

**Sticky errors survive.** They persist until explicitly cleared by a write to the ABORT
register, and a line reset does not touch them. Neither does a target reset, since the
debug logic sits in its own power domain so that debugging can survive a system reset.

**TAR auto-increment stops at 1KB.** With AddrInc enabled the MEM-AP increments TAR after
every DRW access, which is what makes bulk transfers cheap: one transaction per word
instead of three. The spec only guarantees the increment across the bottom 10 bits, so
behaviour at a 1KB boundary is IMPLEMENTATION DEFINED. Bulk writes rewrite TAR at every
1KB crossing rather than trusting it.

## Target control

Two key-protected registers, both of which ignore writes without the right key in bits
[31:16]. That is deliberate: it stops a stray bus write from dropping the core into debug
state or resetting the chip.

| Register | Key | Purpose |
|----------|-----|---------|
| DHCSR `0xE000EDF0` | `0xA05F` | C_DEBUGEN and C_HALT to halt the core, S_HALT to confirm |
| AIRCR `0xE000ED0C` | `0x05FA` | SYSRESETREQ to reset the system |

Halt-on-reset combines those with DEMCR `0xE000EDFC`. Setting VC_CORERESET (bit 0) arms a
vector catch, then SYSRESETREQ resets the chip and the core halts at the vector fetch
before executing an instruction. The system reset does not touch the debug power domain,
so the SWD connection survives it and no NRST wire is needed.

### Core registers

The core registers are not memory mapped, so there is no address to read R0 from. Writing
a selector to DCRSR `0xE000EDF4` moves a value between the register file and DCRDR
`0xE000EDF8`, and S_REGRDY in DHCSR says when the transfer has happened. Setting REGWnR
(bit 16) in the selector makes it a write, in which case DCRDR is loaded first. Selectors
0 to 12 are R0 to R12, then 13 SP, 14 LR, 15 PC, 16 xPSR.

The core has to be halted. Reading these while it runs is UNPREDICTABLE, so the shell
refuses rather than printing something meaningless.

Registers right after a reset-halt on a chip holding a test pattern:

```
> t
ok
> x
r0  00000000  r1  00000000  r2  00000000  r3  00000000
r4  00000000  r5  00000000  r6  00000000  r7  00000000
r8  00000000  r9  00000000  r10 00000000  r11 00000000
r12 00000000  sp  B0000000  lr  FFFFFFFF  pc  B0000000
psr 01000000
```

Every value there can be traced back. SP is the first word of the vector table, which the
core loads at reset, and PC is the second word with bit 0 masked off, because bit 0 of a
reset vector marks Thumb state rather than forming part of the address. The T bit in xPSR
(bit 24) is set to match, and the exception number is 0 for thread mode. LR reads
`0xFFFFFFFF`, its reset value. Nothing has executed.

This is the fix for a target whose firmware reconfigures PA13/PA14. Catch the core before
its firmware runs and the pins stay in SWD mode. Note that DEMCR lives in the debug power
domain: the vector catch survives a system reset but not a power cycle, so arm it and
reset rather than arming it and hoping.

## Flash programming

The flash controller is memory-mapped at `0x40023C00`, so programming is ordinary MEM-AP
writes to peripheral registers.

FLASH_CR is locked at reset. Unlocking means writing `0x45670123` then `0xCDEF89AB` to
FLASH_KEYR, in that order; a wrong sequence locks the controller until the next reset. To
program, set PG with PSIZE=x32 and then write the word to its flash address directly. The
controller intercepts the bus write. Poll BSY (FLASH_SR bit 16) afterwards and check the
error flags.

Flash only changes bits from 1 to 0, so a word has to be erased before it is written.
Programming a non-erased word sets PGSERR.

Halt the core first. An erase blocks access to the flash, and starting one underneath a
core that is fetching instructions from that same flash leaves BSY set and the operation
never finishes. The same commands work fine against a locked-up or halted core, which
makes this an easy one to miss.

### Sector map

Sectors are not uniform: four of 16KB, one of 64KB, then 128KB for the rest. Rather than
hardcoding one part, the size is read from the chip at `0x1FFF7A22` and the map derived
from it, so `f` reports the real geometry:

```
> f
512KB, 8 sectors
  0  0x08000000  16KB
  1  0x08004000  16KB
  2  0x08008000  16KB
  3  0x0800C000  16KB
  4  0x08010000  64KB
  5  0x08020000  128KB
  6  0x08040000  128KB
  7  0x08060000  128KB
```

`e` takes either a sector number or an address, resolving an address to the sector holding
it, and rejects both out-of-range sectors and addresses outside flash.

### Option bytes and readout protection

`o` reports FLASH_OPTCR and decodes the protection level:

```
> o
OPTCR 0x0FFFAAED  optlock=y  nWRP 0xFFF
readout protection: level 0, flash readable
```

RDP lives in bits [15:8]. `0xAA` is level 0, no protection. `0xCC` is level 2. Anything
else is level 1, where flash cannot be read over the debug port but protection can still
be dropped, which mass erases the device as it goes. That erase is the point: the contents
cannot outlive the protection.

Level 2 disables the debug port permanently. There is no recovery, no tool that undoes it,
and the chip can never be debugged again. So this project reports RDP and offers exactly
one change, restoring level 0, and has no way to write an arbitrary RDP value at all. A
value that cannot be typed cannot be typed by mistake, which seemed better than a
confirmation prompt in front of an irreversible operation.

Mass erase and dropping protection both ask for a typed confirmation, and mass erase halts
the core first, for the same reason a sector erase does.

### A dropped byte, five steps from its symptom

A flash through the tool once failed with nothing more than "target cancelled at block 1",
and the chain behind it is worth writing down.

The firmware had no interrupt driven receive. `uart_getc` polled the hardware flag, and the
ATmega holds two bytes, so anything arriving while the firmware was busy elsewhere was
simply gone. At startup it is busy connecting to the target, so an `u` sent by a script
during that window vanished. Flash therefore stayed locked, the first erase inside the
transfer returned `FLASH_LOCKED`, the loader cancelled, and the host reported a cancelled
block. Five steps, and the report named none of them.

It was intermittent for the obvious reason: whether the byte survived depended on what the
firmware happened to be doing when it arrived.

Two fixes, and neither alone was enough. A receive ring filled by `USART_RX_vect` covers
bytes that arrive while the firmware is busy, which is the common case of typing ahead or
pasting. Waiting for the prompt covers the window before the firmware is running at all,
where no amount of buffering helps because nothing is listening yet.

Interrupts being enabled during bit-banging looks alarming and is not: SWD is driven
entirely by the host's clock and has no maximum period, so an interrupt that delays an edge
only stretches that clock cycle. The target samples wherever the edges land.

Two smaller lessons fell out of it. A check that can pass for the wrong reason is barely a
check: the first attempt at verifying the unlock searched for `ok` in a buffer that also
held the banner, which contains `ok`. And clearing HUPCL so that closing the port would not
reset the board mid-transfer also stopped opening it from resetting the board, which
silently removed the banner a later version depended on, from the second run onward.

### Timeouts belong in milliseconds

Flash timeouts were originally poll counts, which really means "however long N SWD
transactions take". Raising the SWD clock from 5kHz to full speed shortened every flash
timeout by about a hundred times without touching a line of flash code, and sector erases,
which need 250 to 400ms for 16KB, started reporting failure after roughly 50ms. They are
now expressed in milliseconds and are independent of link speed.

One detail that looks like a bug and is not: EOP (FLASH_SR bit 0) stays clear after a
successful operation, because it is only set when EOPIE is enabled. Success is the absence
of error bits plus a readback that matches.

A locked controller fails quietly. Writes to FLASH_CR are ignored while LOCK is set, so an
erase requested before unlocking never starts, BSY never rises, no error bit is set, and
the operation reports success having done nothing. Erase and program check LOCK first and
report `0xFC` rather than succeeding at nothing.

The same class of mistake is worth watching for in the driver itself. Dropping PG or SER
at the end of an operation is a bus access like any other, and ignoring its result hides a
link failure until some later command trips over it.

### Speed

Writing a word through `mem_ap_write_word` costs three transactions: bank select, TAR,
then DRW. Bulk writes drop that to roughly one per word by selecting the bank and setting
TAR once and then streaming DRW writes, letting auto-increment walk the address. That is
where nearly all of the speed comes from.

Measured on this hardware at the fastest bit-bang rate, 256 bytes takes 57ms, about
4.4KB/s, so a 64KB image lands in roughly 15 seconds. The per-word path managed about
32ms per word, which works out to around nine minutes for the same image.

The SWD clock itself is not the bottleneck. Going from a 2us half period to no delay at
all only moved a 256-byte write from 97ms to 57ms, because the fixed per-bit overhead of
the bit-bang loop dominates. `swd_set_speed()` takes the half period in roughly 250ns
units, and 0 runs the loop flat out.

## Target gotchas

If firmware on the STM32 reconfigures PA13/PA14 as ordinary GPIO, the SWD function is
disconnected from those pins and the debug port is unreachable. The pin ends up in a
tug-of-war with the host: driving it low reads as roughly 1.8V instead of 0V, which is a
quick way to spot the problem.

Two ways out. Hold BOOT0 during a power-on reset so the chip runs the ROM system
bootloader, which never touches those pins. Or erase the flash so there is no firmware
left to reconfigure them.

The other one worth knowing about: the first connect straight after the host resets
always fails, and retrying immediately does not help. Six back-to-back attempts fail and
then every attempt succeeds once about 20ms has passed. Both boards run off the host's 5V
rail, so resetting the host (which avrdude does on every flash) dips the target's
regulator enough to reset it, and the DP does not answer until it has booted. `dp_connect()`
handles this by spacing its retries out rather than hammering.

## Status

The full stack works: physical layer, SW-DP, both power domains, MEM-AP, AHB memory
access, and core halt. Current output:

```
IDCODE  ack=0x01 = 0x2BA01477
PowerUp ack=0x01
ClrErr  ack=0x01
MEM-AP  ack=0x01
CSW     ack=0x01 = 0x23000052

CPUID   ack=0x01 = 0x410FC241
DHCSR   ack=0x01 = 0x03090000
DBGMCU  ack=0x01 = 0x10006431
FLASH0  ack=0x01 = 0xFFFFFFFF
UID0    ack=0x01 = 0x00230048

halting core
DHCSR write ack=0x01
DHCSR   ack=0x01 = 0x01030003
```

What those values are:

| Value | Meaning |
|-------|---------|
| `0x2BA01477` | ARM SW-DP identification |
| `0xF0000000` (CTRL/STAT) | Both power domains requested and acknowledged |
| `0x24770011` (AP IDR) | JEP-106 identity 0x3B (ARM), class 0x8 (MEM-AP), type 0x1 (AHB), so the AHB-AP |
| `0x23000052` (CSW) | What was written, plus DeviceEn (bit 6) set by hardware |
| `0x410FC241` | Cortex-M4 r0p1 |
| `0x10006431` | DEV_ID 0x431 (STM32F411), REV_ID 0x1000 |
| `0x00230048` | First word of the 96-bit unique device ID |

The two DHCSR reads tell a small story. Before halting it reads `0x03090000`, which has
S_LOCKUP set: the flash is erased, so out of reset the core fetched `0xFFFFFFFF` as its
stack pointer and reset vector, faulted, and locked up. After the halt it reads
`0x01030003`, with S_HALT set and S_LOCKUP cleared.

Halting needs a key. DHCSR writes are ignored unless bits [31:16] are `0xA05F`, so a halt
is `0xA05F0003` (key plus C_DEBUGEN and C_HALT).

Halt-on-reset, flash programming and bulk writes all work. A cold start at full speed:

```
connect  ack=0x01
IDCODE = 0x2BA01477
MEM-AP   ack=0x01
rst+halt ack=0x01
unlock   ack=0x01
erase s0 ack=0x01

plain       ack=0x01 256 bytes in 57 ms  verify OK
across 1KB  ack=0x01 256 bytes in 57 ms  verify OK
```

The second write starts at `0x080003F0` deliberately, so it straddles the 1KB boundary
where TAR auto-increment stops being guaranteed.

Working: connect, power up, read and write memory at byte, halfword and word size, halt,
resume, reset-halt, reset-run, single stepping, core registers, hardware breakpoints, data
watchpoints, flash probing, sector and mass erase, programming with verification, option
bytes and readout protection, loading a binary over serial, reading memory back to a file,
and streaming RTT output.

A round trip through the shell, programming a small routine into flash, breaking on it,
and reading it back, verifies byte for byte against the image that went in.

### Footprint

```
Program:  18616 bytes (56.8% of 32KB flash)
Data:       322 bytes (15.7% of 2KB SRAM)
```

The Uno's bootloader occupies the top of flash, so the usable figure is a little under
32KB. Against that, roughly half the flash and an eighth of the RAM is spent.

RAM was the tighter of the two until string literals moved into `PROGMEM`. Before that
`.data` alone was 1396 bytes, which left barely enough stack for XMODEM's 128 byte block
buffer, and every new message made it worse. Afterwards `.data` is 94 bytes and the cost
moved to flash, which there is far more of. On a part with this ratio of flash to RAM,
that trade is almost always the right one.

## References

- **ARM Debug Interface v5 Architecture Specification** (ARM IHI 0031A). The primary
  reference. DP and AP register maps, packet request format, ACK encoding, parity rules,
  turnaround definition, CTRL/STAT and MEM-AP CSW bit assignments, and the sticky error
  flags. Note that this revision predates the JTAG-to-SWD switch sequence, so `0xE79E` is
  not documented in it.
- **STM32F411xC/E reference manual** (RM0383) **and datasheet**. PA13/PA14 defaulting to
  the SWD alternate function out of reset, the 5V-tolerant pin list, BOOT0 boot mode
  selection, DBGMCU_IDCODE, and the unique device ID location.
- **ARMv7-M Architecture Reference Manual** (ARM DDI 0403). The debug register block at
  `0xE000EDF0`: DHCSR bit assignments, the `0xA05F` write key, and CPUID decoding.
- **ATmega328P datasheet**. DDRB/PORTB/PINB semantics including the input pull-up, USART
  registers, and the U2X baud rate calculation.
