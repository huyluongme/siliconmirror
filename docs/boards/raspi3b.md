# Raspberry Pi 3 Model B (`raspi3b`)

```sh
smirror -M raspi3b -kernel kernel8.img
```

## Hardware

| Part | Real board                   | Modelled                      |
|------|------------------------------|-------------------------------|
| SoC  | Broadcom BCM2837             | Partly, see the memory map    |
| CPU  | 4 Cortex-A53 r0p4, Armv8.0-A | AArch64 only, with FEAT_CRC32 |
| RAM  | 1 GB                         | Everything below 0x3f000000   |

The Cortex-A53 of the Raspberry Pi 3 has the CRC32 instructions but not
the Cryptographic Extension; the model follows it.

## The cores

Each core reads the identification registers of a Cortex-A53 r0p4 as
the BCM2837 configures it, from the core's Technical Reference Manual:

- `MIDR_EL1` is `0x410fd034`, and `MPIDR_EL1` is `0x80000000` plus the
  core number, 0 to 3: one cluster of four cores.
- The ID registers show the Advanced SIMD and floating-point unit and
  CRC32, but no Cryptographic Extension and no GIC CPU interface.
- The cache registers describe 32 KB L1 instruction and data caches and
  a 512 KB L2 cache. The caches themselves are not modelled: memory
  behaves as if there were none. The cache maintenance instructions (`DC`,
  `IC`) check their traps and, for those that take an address, translate
  it, which can fault; then they do nothing. `DC ZVA` writes zeros to its
  64-byte block, as `DCZID_EL0` says.
- The MMU does stage 1 translation for EL1&0, EL2 and EL3 as Armv8.0 has
  it, with what `ID_AA64MMFR0_EL1` gives: 40-bit physical addresses and
  the 4 KB and 64 KB granules; the Cortex-A53 has no 16 KB granule. Like
  the real core, it does not set the Access flag itself: a descriptor
  without it gives an Access flag fault. `AT` translates addresses into
  `PAR_EL1`, in the layout of the Cortex-A53 manual.
- Each core has a TLB, which keeps translations until a `TLBI`
  instruction, a reset or a write to a register that controls
  translation (`SCTLR_ELx`, `TCR_ELx`, `TTBRn_ELx`, `MAIR_ELx`, `HCR_EL2`,
  `SCR_EL3`) empties it. Every `TLBI` empties the TLBs of all four cores,
  which the architecture allows: invalidating more than asked is
  permitted. So, as on the real board, software that changes a
  translation table without the TLB maintenance the architecture asks
  for can still see the old translation.
- The Cortex-A53 registers for its own configuration exist:
  `CPUACTLR_EL1`, `CPUECTLR_EL1`, `L2CTLR_EL1`, `L2ECTLR_EL1`,
  `L2ACTLR_EL1` and `CBAR_EL1`. They hold what is written to them,
  except `L2CTLR_EL1`, whose fields software cannot change, and the
  read-only `CBAR_EL1`; writing them changes nothing else.
- The memory error registers `CPUMERRSR_EL1` and `L2MERRSR_EL1` record
  no errors: memory never fails in smirror. A write clears them, as on
  the real core.
- As on the real chip, the four cores share one L2 memory system, and
  so one copy of its registers: `L2CTLR_EL1`, `L2ECTLR_EL1`,
  `L2ACTLR_EL1` and `L2MERRSR_EL1`. A value that one core writes there,
  the others read. These registers reset with the cluster, not when a
  single core resets. The manual says so outright for `L2CTLR_EL1` and
  `L2ECTLR_EL1`; for the other two, which control and report on the
  same L2, smirror assumes it.

A System register that the Cortex-A53 has but smirror does not
implement yet, such as `PMCR_EL0` of the performance monitors, stops the
machine with the register's name. Encodings that the core does not have
are UNDEFINED.

## Memory map

Addresses as the Arm cores see them. The peripherals sit at bus address
`0x7e000000` and up, which the Arm cores see at `0x3f000000`.

| Address      | Size    | Contents                                                 | Modelled                   |
|--------------|---------|----------------------------------------------------------|----------------------------|
| `0x00000000` | 1008 MB | RAM                                                      | Yes                        |
| `0x3f000000` | 16 MB   | Peripherals                                              | Partly                     |
| `0x3f00b200` | 512 B   | Interrupt controller of the GPU's and ARM's peripherals  | Yes, up to `0x3f00b227`    |
| `0x3f200000` | 4 KB    | GPIO, 54 pins                                            | Yes, up to `0x3f2000a0`    |
| `0x3f201000` | 4 KB    | UART0, a PL011 with 16-entry FIFOs                       | Yes                        |
| `0x3f215000` | 256 B   | Auxiliary peripherals: UART1, the mini UART; SPI 1 and 2 | The mini UART              |
| `0x40000000` | 256 B   | ARM local peripherals: core timer, interrupts, mailboxes | Yes, but `0x04` and `0x28` |

Accessing an address where nothing is modelled stops the machine with
an error that names the access: what the real board does there is not
known. So does a register of a device that is not modelled yet.

## The Generic Timer

The Generic Timers of the four cores count from one 64-bit counter: the
core timer of the ARM local peripherals (BCM2836 ARM-local peripherals,
section 3.1), at `0x40000000`. It counts the edges of a clock through a
prescaler that divides by 2^31 / prescaler:

| Register     | Offset | Meaning                                                            |
|--------------|--------|--------------------------------------------------------------------|
| Control      | `0x00` | bit 8: the APB clock instead of the crystal; bit 9: steps of 2     |
| Prescaler    | `0x08` | `0x80000000` divides by 1; 0 stops the timer                       |
| Timer, low   | `0x1c` | reading it latches the high half; writing it holds the value       |
| Timer, high  | `0x20` | reads the latched half; writing it sets the timer to both halves   |

All four reset to 0, so the timer is stopped until the boot stub writes
the prescaler. The crystal runs at 19.2 MHz. The APB clock runs at half
the ARM clock, which is 1.2 GHz on the Raspberry Pi 3 Model B: 600 MHz.

smirror's time is virtual: one instruction takes 1 ns, and when every
core waits, time jumps to the next thing that can wake one. A program
that counts 2,000,000 instructions sees the counter go up by 38,400
ticks of 19.2 MHz, but on the host it takes as long as smirror needs
to run those instructions: a loop that waits for one second of the
counter takes longer than a second.

Each core has the four timers of an Armv8.0 core with EL3 and EL2
(`CNTP_*`, `CNTV_*`, `CNTHP_*` and `CNTPS_*`), the counter registers,
`CNTKCTL_EL1`, `CNTHCTL_EL2` and `CNTVOFF_EL2`, with the access rules of
the Arm Architecture Reference Manual, and the two event streams that
wake `WFE`. A timer's interrupt output follows `ENABLE`, `IMASK` and
`ISTATUS`, and goes to the ARM local peripherals: see Interrupts.

The document does not say how the prescaler works inside. smirror
treats it as an accumulator: each clock edge adds the prescaler, and
each time the sum reaches 2^31 the timer steps. Its other choices:

- Writing the high half "sets the pre-scaler to zero" (section 3.1.2).
  smirror reads this as the divider starting over, not as the prescaler
  register becoming 0, which would stop the timer.
- A prescaler above 2^31, which the document says not to use, stops the
  machine, and so do accesses other than 32-bit ones.
- The cores take turns running through the same stretch of virtual
  time, up to 1 µs. When a core accesses the block at an earlier time
  than the last change, the block uses the time of that change: its
  time never goes back.

## Interrupts

Each core has the interrupt inputs of a Cortex-A53: `nIRQ`, `nFIQ`, and
the virtual `nVIRQ` and `nVFIQ`. The ARM local peripherals drive
`nIRQ` and `nFIQ` of each core; their document shows no way to drive
`nVIRQ` and `nVFIQ`, so on this board virtual interrupts come only from
`HCR_EL2.VI` and `VF`.

The cores take IRQs and FIQs as the Arm Architecture Reference Manual
says (D1.4.6.4 and Table D1-22): `SCR_EL3.IRQ` and `FIQ` send them to
EL3, `HCR_EL2.IMO`, `FMO` and `TGE` to EL2, and `PSTATE.I` and `F` mask
them at their target level and on the way from EL0 to EL1. An
interrupt that is pending and not masked is taken before the next
instruction. `WFI` does not wait while an interrupt is pending, and an
interrupt ends its wait; `WFE` waits on, unless the interrupt is one
that the core takes. `ISR_EL1` shows what is pending.

The ARM local peripherals (BCM2836 document, sections 3.2, 3.3 and 4.4
to 4.11) route these interrupts to each core's IRQ or FIQ:

| Source                             | Goes to                                   | Routing register  |
|------------------------------------|-------------------------------------------|-------------------|
| The four timers of a core          | that core                                 | `0x40` + 4 × core |
| The four mailboxes of a core       | that core                                 | `0x50` + 4 × core |
| The performance monitors of a core | that core                                 | `0x10`, `0x14`    |
| GPU IRQ, GPU FIQ                   | the core in the routing                   | `0x0c`            |
| The local timer                    | the IRQ or FIQ of the core in the routing | `0x24`            |
| The AXI outstanding interrupt      | the IRQ of core 0                         | none              |

`0x60` and `0x70` + 4 × core show the IRQ and FIQ sources of each core.
The mailboxes, at `0x80` (write to set bits) and `0xc0` (read, write to
clear bits), interrupt while not 0, which makes them the doorbells that
one core rings for another. The local timer counts down 38.4 million
times a second, on each edge of the 19.2 MHz crystal.

The GPU IRQ and GPU FIQ come from the interrupt controller at
`0x3f00b200` (BCM2835 ARM Peripherals, chapter 7). It takes the 64
interrupts of the GPU's peripherals, among them the mini UART as
interrupt 29, the GPIO as 49 to 52 and UART0 as 57, and the 8 of the
ARM's own, such as the ARM timer and mailbox. An
interrupt is pending while it is raised and enabled; the GPU IRQ is set
while any is pending. Pending 1 and 2 show the GPU's, and the basic
pending register the ARM's, eleven selected GPU interrupts again (7, 9,
10, 18, 19, 53 to 57 and 62) in bits 10 to 20, and in bits 8 and 9
whether pending 1 or 2 has others. The GPU FIQ follows the one
interrupt that the FIQ register selects, whether its IRQ is enabled or
not. Of its sources, the mini UART, the GPIO and UART0 are modelled
yet.

The documents leave some things open; smirror chooses:

- Reads of write-only registers and of reserved bits give 0; writes to
  read-only registers and reserved bits are ignored. The PMU routing
  registers, write-set at `0x10` and write-clear at `0x14`, both read
  as the routing.
- `0x18` and `0x3c`, unused, read as 0 and ignore writes. The document
  does not give the contents of the deprecated `0x04` and of the
  routing of local interrupts 8 to 15 at `0x28`: they stop the machine.
- In the mailbox routing, the document says that bit 4 overrides bit 1;
  every other FIQ bit overrides the IRQ bit 4 below it, and smirror
  does the same for bit 5.
- The local timer sets its flag every reload-value pulses, and a reload
  value of 0 acts as 1.
- The AXI outstanding interrupt comes when no core has had an AXI
  transfer for the time-out. smirror has no caches, so a core that runs
  makes transfers all the time: the bus is quiet only while every core
  waits, and an access to the ARM local peripherals is a transfer. The
  document does not say how the interrupt ends, and reading the source
  registers is itself a transfer, so smirror keeps it until software
  clears its enable bit. The outstanding counters read as one read: the
  read of them.
- The disable registers of the interrupt controller read as the enable
  bits, as the enable registers do. Its document marks the FIQ enable
  bit as read-only, but software sets it to enable the FIQ: it is
  read-write. Enabling the FIQ with a source of 72 to 127, which the
  document says not to use, stops the machine, and so does an access to
  the rest of the controller's 512 bytes, after `0x3f00b224`.
- When several interrupts are pending, the architecture leaves the
  order to the core (D1.4.6.5). smirror takes a physical FIQ, then a
  physical IRQ, then a virtual FIQ, then a virtual IRQ, each before the
  instruction at the PC and any exception that it would raise.

## The serial console

As on the board, `-serial` is a serial cable on the header: pin 8 is
GPIO 14, which the board sends on, and pin 10 is GPIO 15, which it
receives on. Which UART the cable reaches depends on the functions of
these pins, in `GPFSEL1`: UART0, the PL011, has them as ALT0, and
UART1, the mini UART, as ALT5 (BCM2835 ARM Peripherals, section 6.2).
What a UART sends reaches `-serial` while GPIO 14 carries its TXD, and
it receives from `-serial` while GPIO 15 carries its RXD. Bytes that
come while no UART has GPIO 15 are lost, as on the board.

The other pins that can carry the UARTs go to no serial device that is
modelled: GPIO 32 and 33 go to the Bluetooth module, which takes
nothing and keeps its TX line, GPIO 33, high. A UART whose RXD no pin
carries, or whose pin is low, has its receive line held low.

On a Raspberry Pi 3, the console is the mini UART, and the firmware
sets it up when `config.txt` has `enable_uart=1`, which keeps the core
clock at 250 MHz (Raspberry Pi documentation, "Configure UARTs").
U-Boot then uses it as it is, after checking that GPIO 15 has ALT5.
smirror starts the machine, and resets it, in that state: GPIO 14 and
15 at ALT5, the mini UART enabled, with 8-bit characters at 115313
baud (`AUX_MU_BAUD_REG` 270). The firmware's own register writes are
not documented. A program that wants UART0 instead gives it the pins,
writing ALT0 for GPIO 14 and 15 in `GPFSEL1`, as a program on the real
board must.

## GPIO

The GPIO block at `0x3f200000` (BCM2835 ARM Peripherals, chapter 6)
has 54 pins. Each has a function (`GPFSELn`), an output latch
(`GPSETn`, `GPCLRn`) and a pull, which `GPPUD` and `GPPUDCLKn` set; after
reset every pin is an input with the pull of the table of section 6.2.
`GPLEVn` reads, for an output, its latch; for an alternate function
whose peripheral drives the pin, what it drives, such as a UART's TX
line, high while idle; else what drives the pin from outside the chip,
such as the serial cable on GPIO 15, or else its pull.

Edges and levels set `GPEDSn` as `GPRENn`, `GPFENn`, `GPHENn`, `GPLENn`,
`GPARENn` and `GPAFENn` ask, and writing 1 clears them; a level stays
recorded while it lasts. The interrupts follow `GPEDSn`, as section 5.1
of the BCM2711 ARM Peripherals describes the same block: interrupt 49
for pins 0 to 27, 50 for 28 to 45, 51 for 46 to 53, and 52 for all.

The documents leave some things open; smirror chooses:

- A pin that nothing drives and that has no pull reads as 0.
- The output latch is 0 after reset.
- A write of `GPPUDCLKn` clocks `GPPUD` into its pins at once: the waits
  of 150 cycles are not needed.
- Pins change level at once, so the synchronous edge detection sees
  every edge, like the asynchronous one.
- Accesses other than 32-bit ones, to the test register at `0xb0`, to
  reserved offsets, writes of `GPLEVn`, reads of `GPSETn` and `GPCLRn`,
  and clocking the reserved `GPPUD` value 3 into a pin stop the machine.

## The mini UART

UART1, the mini UART at `0x3f215000` (BCM2835 ARM Peripherals,
chapter 2), runs from the 250 MHz core clock: a bit lasts 8 ×
(`AUX_MU_BAUD_REG` + 1) cycles, and a frame has a start bit, 7 or 8
data bits and a stop bit, so a byte takes 86.7 µs at 115313 baud. Its
FIFOs hold 8 bytes. It sends and receives as UART0 does, below, without
parity, framing errors or a receive timeout. A receive line held low
brings 0x00 after 0x00, as section 2.1.1 describes; once the FIFO is
full and the overrun set, more of them would change nothing that can be
seen, so they wait until a read makes room.

Its interrupt, interrupt 29 of the interrupt controller, is high while
the receive interrupt is enabled and a byte waits, or the transmit
interrupt is enabled and the transmit FIFO is empty. `AUX_MU_IIR_REG`
names it, the receiver first, and `AUX_IRQ` shows it.

The documents leave some things open, or get them wrong; smirror
chooses:

- In `AUX_MU_IER_REG`, bit 0 enables the receive interrupt and bit 1
  the transmit interrupt, as in a 16550 and as Linux's driver uses them
  on the real chip: both the BCM2835 and the BCM2711 documents swap
  them.
- `AUX_MU_STAT_REG` bit 9 is the AND of bits 3 and 8, as its text says.
- The CTS input is asserted; RTS goes nowhere. `AUX_MU_STAT_REG` bit 6
  reads 1 while RTS is asserted.
- A write to a full transmit FIFO is lost, and a disabled transmitter,
  receiver or mini UART finishes its byte.
- The document says the registers of a disabled module cannot be read
  or written, but not what happens: such an access stops the machine.
  So do accesses other than 32-bit ones, writes of the read-only
  registers, offsets the document does not describe, the SPI masters,
  and transmit flow control with CTS, which nothing connects.

## UART0

UART0 is a PL011 with 16-entry FIFOs (BCM2835 ARM Peripherals, chapter
13) and a 48 MHz reference clock, `UARTCLK`, the default of
`init_uart_clock` in `config.txt`. Sending a byte takes the time of its
frame at the baud rate: a start bit, 5 to 8 data bits, a parity bit if
enabled, and 1 or 2 stop bits, each bit lasting 16 × (`UARTIBRD` +
`UARTFBRD`/64) periods of `UARTCLK`. At 115200 baud (`UARTIBRD` 26,
`UARTFBRD` 3), an 8-bit character with one stop bit takes 86.8 µs. The
byte reaches `-serial`, if GPIO 14 carries it, when its last stop bit
is done. As in the PL011
manual, the divisor and the frame format take effect at the next write
of `UARTLCR_H`, a disabled UART finishes the byte it is sending, and
`BUSY` stays set until the last stop bit.

The UART's interrupt, `UARTINTR`, goes to interrupt 57 of the interrupt
controller. Its transmit interrupt follows the transmit FIFO through
the trigger level that `UARTIFLS` selects, as the PL011 manual
describes: it sets when a byte leaving the FIFO takes it from above the
level to the level or below, and clears when writes take it above.
Without FIFOs, it sets when the holding register empties.

The manual calls a divisor below 1, or 65535 with a fraction, invalid,
and its trigger levels `0b101` to `0b111` reserved. Sending or receiving
with such a divisor, or with such a level while the FIFOs are on, stops
the machine. Reading `UARTDR` with the receive FIFO empty gives 0.

While GPIO 15 carries its RXD, UART0 receives what comes from
`-serial`: with `stdio`, the keys typed
on the terminal, or the bytes of standard input. They come one after
the other on the line, each taking the time of a frame in the format
that UART0 uses, as if the other end used the same settings; so they
are never in error, and only the low data bits of each are kept. At the
end of its frame, a byte goes into the receive FIFO with the PL011's
four error bits, or, when the FIFO is full, is lost: the overrun error
then sets, in `UARTRSR` at once and in `UARTDR` with the next byte
stored. Bytes that come while the UART or its receiver is disabled are
lost, as on a real line, where no one listens then. There is no flow
control: input that comes faster than the guest reads it overruns the
FIFO, as on the real board.

The receive interrupt sets when the receive FIFO goes up through its
trigger level, or without FIFOs when a byte comes, and clears when
reads take it below. The receive timeout interrupt sets when the FIFO
holds data and no byte has come for 32 bit periods, and clears when the
FIFO empties. The overrun interrupt sets with the overrun error. The
framing, parity and break interrupts never come, as the bytes are never
in error. The BCM2835's PL011 has no DSR, DCD or RI inputs, and its CTS
input comes through a GPIO pin that is not modelled, so the modem
status interrupts never come either. Loopback is not modelled yet.

## Boot with `-kernel`

For `kernel8.img`, the Raspberry Pi firmware does the following:

1. It loads the image at `0x80000`, and a small stub of Arm code,
   armstub8, at address 0. It writes the addresses of the kernel and of
   the device tree into the stub.
2. It starts all four cores at address 0, at EL3, in the stub.
3. The stub sets up the registers that only EL3 can write, and moves
   each core to EL2 with D, A, I and F masked.
4. Core 0 jumps to the image with X0 holding the address of the device
   tree and X1 to X3 zero.
5. Cores 1 to 3 wait in a loop. Core *n* reads the 64-bit word at
   `0xd8 + 8 * n`, its entry in the spin table. When the operating system
   writes an address there and executes `SEV`, the core jumps to that
   address with X0 to X3 zero.

smirror does the same with a stub of its own,
[raspi3b_stub.s](../../src/machine/raspi3b_stub.s), written from what
armstub8 does. To leave the cores exactly as the real stub does, it
executes the same instructions in the same order and with the same
registers, so a kernel also finds the same values in X4 to X6. Its
labels, comments and constant names are smirror's own. The cores start
in it from reset, so resetting the machine runs it again. Like armstub8,
it is 256 bytes long and ends with its data:

| Address | Size  | Contents                                                       |
|---------|-------|----------------------------------------------------------------|
| `0xd8`  | 4 × 8 | the spin table, one entry for each core (core 0's is not used) |
| `0xf0`  | 4     | the magic number `0x5afe570b`                                  |
| `0xf4`  | 4     | the stub's version, 0                                          |
| `0xf8`  | 4     | the address of the device tree                                 |
| `0xfc`  | 4     | the address of the kernel                                      |

The magic number and the version overlap the spin table entry of
core 3. After loading the stub, the board clears them, as armstub8's
comments say the firmware does, and writes `0x80000` and 0 (no device
tree yet) into the last two words.

At EL3, every core writes these System registers, in this order:

| Register       | Value              | Meaning                                                      |
|----------------|--------------------|--------------------------------------------------------------|
| `L2CTLR_EL1`   | bits 5 and 1 set   | asks for slower L2 RAM; the Cortex-A53 ignores it            |
| `CNTFRQ_EL0`   | `19200000`         | tells software the counter's frequency                       |
| `CNTVOFF_EL2`  | `0`                | the virtual count is the physical count                      |
| `CPTR_EL3`     | `0`                | no traps to EL3, of FP and SIMD or anything else             |
| `SCR_EL3`      | `0x5b1`            | EL2 and EL1 are Non-secure and AArch64; `HVC` on, `SMC` off  |
| `ACTLR_EL3`    | `0x73`             | EL2 may write the Cortex-A53 configuration registers         |
| `CPUECTLR_EL1` | `0x40`             | SMPEN: the core takes part in cache coherency                |
| `SCTLR_EL2`    | `0x30c50830`       | MMU, caches and alignment checks off, stack alignment too    |
| `SPSR_EL3`     | `0x3c9`            | for the `ERET` to EL2: EL2h, with D, A, I and F masked       |

Before them, each core starts the core timer of the ARM local
peripherals: it writes 0 to the control register at `0x40000000`, to
count the crystal by 1, and `0x80000000` to the prescaler at
`0x40000008`, to divide by 1.

On EL2, core 0 reads the two addresses from the stub's data and jumps.
Cores 1 to 3 take their number from `MPIDR_EL1` and, as in armstub8,
execute `WFE` before each read of their spin table entry. The `ERET` to
EL2 has set the Event Register, so the first `WFE` does not wait and the
entry is read once before the core sleeps.

smirror is built from the stub's bytes, kept in `raspi3b_stub.inc`, so
that building it needs no AArch64 tools; `make stub` updates them after
a change to the stub (see CONTRIBUTING.md).

The machine stops when every core waits in `WFI` or `WFE` and nothing can
wake them, so a program that ends with a `WFI` loop makes smirror exit.
Input on the UART that has GPIO 15 can wake a core, so while it listens
and more input can come, smirror waits for it instead: press Ctrl-A x
to quit.

## Differences from the real board

- No device tree is passed yet: X0 is 0.
- Time is virtual, as described under The Generic Timer: the counter
  follows the instructions that smirror runs, not the host's clock.
- Of the interrupt controller's sources, only the mini UART, the GPIO
  and UART0 are modelled yet.
- The cores have no performance monitors, so their interrupt never
  comes.
- A core sees an interrupt from another core, such as a mailbox, at its
  next instruction. The cores take turns through the same 1 µs of
  virtual time, so a core that runs later in the turn can see it up to
  1 µs before it was sent. smirror notices that the bus has gone quiet
  at the end of such a turn, so the AXI outstanding interrupt can come
  up to 1 µs late.
- The GPU's share of the RAM, a firmware setting, is not modelled: all
  of the RAM below the peripherals is free.
- The serial lines carry bytes, not bits: the GPIO sees no edges on
  GPIO 14 and 15 while bytes go, the mini UART's break is not sent, and
  UART0 sees an idle line, not a break, when its receive line is held
  low. The Bluetooth module and the other devices on the GPIO pins are
  not modelled.
- The Cortex-A53 manual says which `ACTLR_EL3` and `ACTLR_EL2` bits
  allow a lower Exception level to write the configuration registers,
  but not what happens when the bit is 0. smirror traps the write to
  the Exception level whose `ACTLR` forbids it.
- Stage 2 translation is not implemented yet: running at EL1 or EL0 with
  `HCR_EL2.VM` or `HCR_EL2.DC` set stops the machine. It is needed only
  to run virtual machines, as KVM does.
- The TLB is not the Cortex-A53's: it holds 256 entries of 4 KB, without
  ASIDs. A program that follows the architecture's TLB maintenance rules
  sees no difference.
- The manual leaves some MMU behavior to the implementation, and the
  Cortex-A53 manual does not say what the core does. Where it would
  change what a translation table means, smirror stops with a message
  instead of guessing: a `TCR_ELx.TGn` granule that the core does not
  have, or a `TCR_ELx.TnSZ` outside 16 to 39. Elsewhere smirror chooses:
  `DC` and `IC` instructions take Access flag faults like loads and
  stores do; an instruction fetch from Device memory does not fault;
  `IC IVAU` at EL0 needs no read permission; and `PAR_EL1.NS` is 1 after
  a Non-secure translation, where the manual says it is UNKNOWN.
- The BCM2837 has one physical address space for Secure and Non-secure
  accesses, so the NS bit of a descriptor only changes what `PAR_EL1`
  reports and what `SCR_EL3.SIF` allows.
- smirror sets up the mini UART as the firmware does with
  `enable_uart=1`, whatever `config.txt` says, as it reads no
  `config.txt` yet. A program that uses UART0 must give it GPIO 14 and
  15 and set it up itself. The PL011 resets to a baud rate divisor of 0,
  which is invalid, and to 5-bit characters: set `UARTIBRD` and
  `UARTFBRD` (26 and 3 for 115200 baud), then `UARTLCR_H` to 8 bits,
  before sending.

## Sources

- Linux device trees
  [bcm2837.dtsi](https://github.com/torvalds/linux/blob/master/arch/arm/boot/dts/broadcom/bcm2837.dtsi)
  and
  [bcm283x.dtsi](https://github.com/torvalds/linux/blob/master/arch/arm/boot/dts/broadcom/bcm283x.dtsi):
  peripheral addresses, cores and their spin table entries, UART0.
- [Arm Cortex-A53 MPCore Processor Technical Reference Manual](https://developer.arm.com/documentation/ddi0500/j/),
  ARM DDI 0500J, chapter 4: identification and reset values, the
  translation granules and physical address size (`ID_AA64MMFR0_EL1`
  4.3.21), the layout of `PAR_EL1` (4.3.68), the
  configuration registers and the `ACTLR` bits that control them
  (`ACTLR_EL3` 4.3.33, `L2CTLR_EL1` 4.3.64, `CPUECTLR_EL1` 4.3.79),
  and the reset values of the Generic Timer registers (4.3); appendix
  A.5: the interrupt inputs `nIRQ`, `nFIQ`, `nVIRQ` and `nVFIQ`.
- `/proc/cpuinfo` of a Raspberry Pi 3, for example in
  [this gist](https://gist.github.com/fawick/d142633de52b31d0dade):
  CPU part `0xd03`, revision 4, so a Cortex-A53 r0p4.
- [BCM2835 ARM Peripherals](https://datasheets.raspberrypi.com/bcm2835/bcm2835-peripherals.pdf):
  the auxiliary peripherals and the mini UART (chapter 2), the GPIO,
  its registers and the alternate functions and pulls of its pins
  (chapter 6), the interrupt controller (chapter 7, sections 7.1 to
  7.5) and its table of interrupts; the PL011, its FIFOs, its
  interrupts and the signals it lacks (chapter 13, sections 13.1 to
  13.3).
- [BCM2711 ARM Peripherals](https://datasheets.raspberrypi.com/bcm2711/bcm2711-peripherals.pdf),
  Raspberry Pi Ltd, 2022: the same mini UART with the titles of
  `AUX_MU_IER_REG` and `AUX_MU_IIR_REG` fixed (chapter 2), and the
  banks of the GPIO and their interrupt lines (section 5.1).
- Raspberry Pi documentation,
  [Configure UARTs](https://www.raspberrypi.com/documentation/computers/configuration.html#configure-uarts):
  the mini UART is the console on GPIO 14 and 15 of a Raspberry Pi 3,
  UART0 goes to Bluetooth, and `enable_uart=1` keeps the core clock at
  250 MHz.
- U-Boot,
  [serial_bcm283x_mu.c](https://github.com/u-boot/u-boot/blob/master/drivers/serial/serial_bcm283x_mu.c):
  it uses the mini UART only when GPIO 15 has ALT5, as the firmware set
  it up; Linux,
  [8250_bcm2835aux.c](https://github.com/torvalds/linux/blob/master/drivers/tty/serial/8250/8250_bcm2835aux.c):
  it drives the mini UART as a 16550, so with the 16550's bits of
  `AUX_MU_IER_REG`; the Raspberry Pi device tree
  [bcm2710-rpi-3-b.dts](https://github.com/raspberrypi/linux/blob/rpi-6.6.y/arch/arm/boot/dts/bcm2710-rpi-3-b.dts):
  what the GPIO pins of the board connect to.
- [PrimeCell UART (PL011) Technical Reference Manual](https://developer.arm.com/documentation/ddi0183/g/),
  ARM DDI 0183G: transmitting, receiving and the character frame
  (2.4.3, 2.4.5), the interrupts (2.8), `UARTDR` and its error bits
  (3.3.1), `UARTRSR` and `UARTECR` (3.3.2), the flags (3.3.3), the baud
  rate divisor and its limits (3.3.5, 3.3.6), `UARTLCR_H` updating the
  divisor (3.3.7), `UARTCR` (3.3.8), the trigger levels (3.3.9) and the
  interrupt registers (3.3.10 to 3.3.13).
- Raspberry Pi documentation, `init_uart_clock` in the
  [legacy config.txt options](https://github.com/raspberrypi/documentation/blob/master/documentation/asciidoc/computers/legacy_config_txt/boot.adoc):
  UART0's clock, 48 MHz by default.
- [BCM2836 ARM-local peripherals](https://datasheets.raspberrypi.com/bcm2836/bcm2836-peripherals.pdf)
  (Quad-A7 control, rev 3.4): the core timer (sections 3.1, 4.2 and
  4.3), the interrupt routing and the mailboxes (3.2, 3.3 and 4.4 to
  4.10), the local timer (4.11). The BCM2837 has the same block.
- [Trusted Firmware-A, Raspberry Pi 3](https://github.com/ARM-software/arm-trusted-firmware/blob/master/docs/plat/rpi3.rst):
  RAM size, and the default stub that starts `kernel8.img` at EL2.
- The default stub,
  [armstub8.S](https://github.com/raspberrypi/tools/blob/master/armstubs/armstub8.S),
  (BSD-3-Clause, copyright Raspberry Pi (Trading) Ltd. and Stephen
  Warren): smirror's stub follows its instructions, register values and
  data layout one for one, as described above; the source text is
  smirror's own.
- [Arm Architecture Reference Manual for A-profile architecture](https://developer.arm.com/documentation/ddi0487/latest),
  ARM DDI 0487 issue M.d: `SCR_EL3` (D24.2.168), `SCTLR_EL2`
  (D24.2.175), `CPTR_EL3` (D24.2.38), `MPIDR_EL1` (D24.2.137),
  `SPSR_EL3` (C5.2.23) and the Event Register (D1.7.1.1); the Generic
  Timer (D12) and its registers (D24.10); interrupts: their masking
  and targets (D1.4.6.4, Table D1-22) and order (D1.4.6.5),
  `AArch64_TakePhysicalIRQException()` and the functions around it
  (J1.2.2.16 to J1.2.2.20), `Hint_WFI()` and
  `AArch64_InterruptPending()`, the wakeup events of `WFE` and `WFI`
  (D1.7.1.4, D1.7.2.1), and `ISR_EL1` (D24.2); the MMU: VMSAv8-64
  (D8), TLB maintenance and the rule that permits invalidating more
  entries (D8.17, R_MMZTY), and `AArch64_S1Translate()` with the
  functions it calls (J1.2.4); `DC`, `IC`, `AT` and `TLBI` (C5.3 to
  C5.5), and `DCZID_EL0`.
- Raspberry Pi documentation, `kernel_address` in the
  [2021 version of the config.txt boot options](https://github.com/raspberrypi/documentation/blob/16480247dcac12d1f828c0f2556a3bc430de3c90/configuration/config-txt/boot.md):
  64-bit kernels load at `0x80000`.
- Raspberry Pi documentation, `arm_freq` in
  [config.txt, Overclocking options](https://www.raspberrypi.com/documentation/computers/config_txt.html#overclocking-options):
  the Raspberry Pi 3 runs its cores at 1200 MHz.
- Raspberry Pi documentation,
  [Configure UARTs](https://www.raspberrypi.com/documentation/computers/configuration.html#configure-uarts):
  which UART is on GPIO 14 and 15.
