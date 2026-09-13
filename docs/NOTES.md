# Engineering notes

Detail behind the summary in the [README](../README.md): how each piece works, and the
things that cost real time to find. Most of these were discovered by running the thing
rather than by reading the specification.

## Contents

| Section | Covers |
|---------|--------|
| [Protocol](#protocol) | Framing, turnaround, posted reads, byte lanes, CSW |
| [Target control](#target-control) | Halt, reset-halt, stepping, core registers |
| [Flash](#flash) | Unlock, program, sector map, option bytes, protection |
| [Using it](#using-it) | Worked sessions for each feature |
| [Bugs worth remembering](#bugs-worth-remembering) | The ones that cost hours |

## Protocol


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

## Flash


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

## Using it

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
set. Arming a watchpoint without it looks like it worked and never fires.

MASK is how many low address bits to ignore, so it decides how much the watchpoint
actually covers: 0 matches one address, 2 matches an aligned four byte object. Getting
this wrong is quiet. With MASK left at zero, watching a four byte variable catches a word
store to its base address but misses a byte store two bytes in, while the debugger goes on
reporting the whole variable as watched. So `a` takes an optional length, the address must
be aligned to it, and the field is read back after writing because its width is
implementation defined and a value the part cannot hold comes back smaller.

```
> a 20000000 w 4
watchpoint 0 on write at 0x20000000
```

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

### Checking firmware ran, without an LED

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

## Bugs worth remembering

### The read that was never sent

Peripheral reads through gdb started failing with "Cannot access memory", while the same
read typed into the shell returned the right value. The target was plainly fine, so the
fault had to be in the tool, and the obvious suspects were the read path and the serial
framing. Both were wrong.

The packet log settled it: the read packet was not in it. Not failing, absent. gdb had
refused the access itself and never asked.

The cause was the memory map added a few changes earlier. Once a target supplies one, gdb
treats every address outside it as inaccessible and fails the access locally. The map
listed flash and SRAM, so the whole peripheral space, the private peripheral bus and the
system memory region silently became unreachable. Reads had worked before the map existed,
which is exactly the sort of correlation that gets missed when the change looks unrelated
to the symptom.

Two things generalise. A missing request and a rejected request give the user the same
error and completely different logs, so the question worth asking early is what actually
crossed the wire, not what the far end thought of it. And a memory map has to be complete
rather than merely correct: everything worth reading belongs in it, not just everything
worth writing.

### A failed unlock that keeps itself failing

Unlock started returning `ack=0x04`, a bus fault, and kept returning it on every later
attempt and from every tool, long after whatever first upset the target was gone.

`FLASH_KEYR` is not a plain register. It takes two key writes in sequence, and the
reference manual is explicit that a wrong or out of order value raises a bus error and
locks the controller until the next reset. So a run that faults after the first key
leaves the controller waiting for the second one. The next attempt opens with the first
key again, which is now the wrong value, and locks it again. The state that breaks the
unlock is created by the previous failed unlock.

Two things made it worse. Retrying on a fault was exactly the wrong reflex, because the
retry is what re-locks the controller. And nothing upstream reset the target, so a tool
that had worked for months started failing at its first command with no code change
behind it.

The fix is a reset before the unlock, not a retry after the fault. The reset also
re-locks the controller, so it has to come before the unlock and must not be repeated
between the erase and the writes that follow, or the unlock it depends on is thrown away.

The general lesson: a fault code says what happened, not whether repeating the operation
is safe. A stateful register sequence is not idempotent, and retrying one blindly turns a
transient failure into a permanent one.

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

### When target firmware hijacks the SWD pins

If firmware on the STM32 reconfigures PA13/PA14 as ordinary GPIO, the SWD function is
disconnected from those pins and the debug port is unreachable. The pin ends up in a
tug-of-war with the host: driving it low reads as roughly 1.8V instead of 0V, which is a
quick way to spot the problem.

Two ways out. Hold BOOT0 during a power-on reset so the chip runs the ROM system
bootloader, which never touches those pins. Or erase the flash so there is no firmware
left to reconfigure them.

### The first connect after a host reset

The first connect straight after the host resets
always fails, and retrying immediately does not help. Six back-to-back attempts fail and
then every attempt succeeds once about 20ms has passed. Both boards run off the host's 5V
rail, so resetting the host (which avrdude does on every flash) dips the target's
regulator enough to reset it, and the DP does not answer until it has booted. `dp_connect()`
handles this by spacing its retries out rather than hammering.
