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
| `0x3f00b880` | 64 B    | Mailboxes between the Arm and the VideoCore              | Yes                        |
| `0x3f100000` | 276 B   | Power manager: power domains, reset lines, watchdog      | The watchdog               |
| `0x3f200000` | 4 KB    | GPIO, 54 pins                                            | Yes, up to `0x3f2000a0`    |
| `0x3f201000` | 4 KB    | UART0, a PL011 with 16-entry FIFOs                       | Yes                        |
| `0x3f202000` | 256 B   | SDHOST, the SD card's controller                         | Yes, but its DMA requests  |
| `0x3f215000` | 256 B   | Auxiliary peripherals: UART1, the mini UART; SPI 1 and 2 | The mini UART              |
| `0x3f300000` | 256 B   | EMMC, the Arasan SD controller of the WiFi chip          | Yes, without the chip      |
| `0x3f980000` | 64 KB   | USB, a Synopsys DWC2 controller                          | As a host                  |
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

## The mailboxes and the firmware

The Arm cores and the VideoCore, where the firmware runs, talk through
two mailboxes at `0x3f00b880`. Mailbox 1 carries messages to the
VideoCore, mailbox 0 back to the Arm; each holds eight 32-bit words. A
message is a channel in bits 3:0 and data in bits 31:4.

| Offset | Register                   | Arm access                          |
|--------|----------------------------|-------------------------------------|
| `0x00` | Mailbox 0 read             | Read: takes the oldest word         |
| `0x10` | Mailbox 0 peek             | Read: the oldest word, left in      |
| `0x18` | Mailbox 0 status           | Read: bit 31 full, bit 30 empty     |
| `0x1c` | Mailbox 0 configuration    | Bit 0: interrupt while it has data  |
| `0x20` | Mailbox 1 write            | Write: adds a word                  |
| `0x38` | Mailbox 1 status           | Read: bit 31 full, bit 30 empty     |

The interrupt is ARM interrupt 1 of the interrupt controller ("ARM
Mailbox"). The BCM2835 datasheet does not describe these registers: the
firmware wiki and Linux's driver do, as listed under Sources. smirror
stops on what they leave open: reading an empty mailbox or writing a
full one, the sender registers at `0x14` and `0x34`, the Arm reading
mailbox 1 or writing mailbox 0, the other configuration bits, and
accesses that are not 32-bit. The low bits of a status register read
as 0.

The firmware takes each message 1 µs after it could start on it, one at
a time; the real firmware's time is not documented, and this is short
enough for every timeout of the software that waits for it. It answers
only channel 8, the property channel: any other channel stops the
machine.

On the property channel, the Arm puts a buffer of tags in memory and
sends its address; the firmware answers in the buffer and sends the
same address back. The address may carry a bus alias in bits 31:30, as
Linux's does (`0xc0000000`, uncached): the firmware drops it. smirror
follows the wiki's format: the answer to each tag is cut to its value
buffer while its length says how long it is, unknown tags are left
unanswered, and a buffer without an end tag gets the code `0x80000001`.

| Tag          | Answer on smirror's board                                          |
|--------------|--------------------------------------------------------------------|
| `0x00000001` | Firmware revision `0x6ae89328`, when the firmware was built        |
| `0x00000002` | Firmware variant 1: `start`, as `start.elf` is                     |
| `0x00000003` | The firmware's Git hash, `13c4219f...572512bb`, in five words      |
| `0x00010002` | Board revision `0xa02082`: 3 Model B 1.2, 1 GB, Sony UK            |
| `0x00010003` | MAC address `b8:27:eb:ff:ee:03`                                    |
| `0x00010004` | Serial number `0x00000000c0ffee03`                                 |
| `0x00010005` | Arm memory: from 0, 948 MB (1024 MB less `gpu_mem`, 76 MB)         |
| `0x00010006` | VideoCore memory: the 76 MB above it                               |
| `0x00050001` | The kernel's command line, without a terminating 0                 |
| `0x00028001` | Power on or off: the state comes back; devices 9 and up are absent |
| `0x00020001` | Power state, once the Arm has set it                               |
| `0x00030002` | Clock rate; `0x00030004` the maximum, `0x00030007` the minimum     |
| `0x00038002` | Set a clock rate, kept within the clock's range                    |
| `0x00030009` | Turbo, 0 at boot; `0x00038009` sets it                             |
| `0x00038042` | Set SDHOST's clock: the fastest not above the one asked            |
| `0x00040003` | Display size: 0 by 0, as no display is connected                   |
| `0x00030046` | Throttled: 0, as the power supply is good and the board never hot  |
| `0x00048020` | The virtual GPIOs' buffer: 0, success; nothing reads it            |
| `0x00030043` | The GPIO expander: a line's direction; `0x00038043` sets it        |
| `0x00030041` | The GPIO expander: a line's state; `0x00038041` sets an output's   |

The GPIO expander has 8 lines that only the firmware drives, 128 to 135
in its tags: BT_ON, WL_ON, STATUS_LED, LAN_RUN, HDMI_HPD_N, CAM_GPIO0,
CAM_GPIO1 and PWR_LOW_N, as the board's device tree names them. Only
Linux's `gpio-raspberrypi-exp.c` describes the tags; the firmware
answers 0 in the GPIO's place for success. The lines start as `gpioinfo`
shows them on a Pi 3B+ and a Pi 3A+, whose expanders share five of
them: outputs, but for HDMI_HPD_N and PWR_LOW_N, inputs. Their levels
are smirror's choice, after what each line does: Bluetooth off, WiFi
and the LAN9514 powered, the activity LED and the camera off, no
display, a good power supply. A reset brings them back. Other GPIOs, a
polarity or termination, and setting an input stop the machine.

The firmware smirror plays is the `start.elf` of raspberrypi/firmware
commit 896fc67: its revision, variant and hash are those its strings
give (`VC_BUILD_ID_TIME`, `VC_BUILD_ID_VARIANT`, `VC_BUILD_ID_VERSION`).

The rates are the defaults of `config.txt` on a Raspberry Pi 3: the
Arm runs at 600 to 1200 MHz, the core at 250 to 400 MHz (250 MHz with
`enable_uart=1`, as smirror sets up the console), V3D, H.264 and ISP at
250 to 400 MHz, the SDRAM at 400 to 450 MHz, UART0's clock at 48 MHz,
and the EMMC clock of the Arasan controller at 200 MHz, which the
firmware sets from its commit 7fdcd00e on (U-Boot's
`bcm2835_sdhci.c`). HEVC does not exist on the Pi 3: its rate is 0. A new rate for
the Arm changes nothing in the emulation, whose time does not depend on
it; a new rate for the core or for UART0's clock, or turbo while the
core clock may rise, stops the machine, as the UARTs would change speed.

The firmware knows many more tags (the wiki and Linux's
`raspberrypi-firmware.h` list them): one that smirror does not answer
stops the machine, and so do values that nothing documents for this
board: the power state of a device before the Arm sets it, the time a
device takes to power up, the rates of the PIXEL and PWM clocks, and
the range of the UART and EMMC clocks. A reset of the board starts the firmware again: the clocks,
turbo and the power states are back to those of boot.

## The SD card

`-sd FILE` is the card in the board's microSD slot, wired to GPIO 48 to
53 (CLK, CMD, DAT0 to DAT3). It is a card of the SD Physical Layer
specification, version 3.01, as big as its image: an SDSC card up to
2 GB, SDHC up to 32 GB, SDXC above, as real cards are made, so the
image must be a multiple of 512 KB. It speaks SD mode at 3.3 V, on 1 or
4 data lines, at default or high speed (CMD6), and does what a host
needs of it: identification, reads and writes of one block or more,
CMD23, erase (the blocks then read as 0), the SD status and the SCR.
Writes go to the image at once. Its CID names no real manufacturer:
`MID` 0, `OID` "SM", product "SMSD1". The card answers at once; it
is ready 1 ms after the first `ACMD41`. Commands that a card must have
but smirror does not emulate stop the machine: locking (`CMD42`),
`PROGRAM_CSD`, `GEN_CMD` and the security commands. A reset of the
board puts the card back in the idle state, as the boot ROM's `CMD0`
does; it keeps its power.

SDHOST, the BCM2835's own SD controller, drives it; the Arasan
controller (EMMC) goes to the WiFi chip, as described below. No
datasheet describes SDHOST: smirror follows the drivers, Raspberry Pi's
`bcm2835-sdhost.c`, Linux's `bcm2835.c` and U-Boot's
`bcm2835_sdhost.c`.

| Offset | Register | What smirror does                                                     |
|--------|----------|-----------------------------------------------------------------------|
| `0x00` | `SDCMD`  | Command index, flags; `NEW` starts it and clears when it is done      |
| `0x04` | `SDARG`  | Argument                                                              |
| `0x08` | `SDTOUT` | Data and busy timeout, in SD clocks                                   |
| `0x0c` | `SDCDIV` | Clock divider: core clock / (`SDCDIV` + 2); 507 at reset              |
| `0x10` | `SDRSP0` | Response; a long one in `SDRSP0` (bits 31:0) to `SDRSP3`              |
| `0x20` | `SDHSTS` | Errors and interrupts, write 1 to clear; bit 0, `DATA_FLAG`, the FIFO |
| `0x30` | `SDVDD`  | Power: commands with it off stop the machine                          |
| `0x34` | `SDEDM`  | FIFO thresholds; state machine and FIFO fill, read-only               |
| `0x38` | `SDHCFG` | Bus width, `SLOW_CARD`, interrupt enables                             |
| `0x3c` | `SDHBCT` | Bytes a block                                                         |
| `0x40` | `SDDATA` | The FIFO, 16 words                                                    |
| `0x50` | `SDHBLC` | Blocks of the transfer                                                |

Commands, responses and data take their bits' time at the SD clock: 48
bits a command, 48 or 136 a response 2 clocks later, 32 clocks a word
on 1 line or 8 on 4, a block's data 2 clocks after the response, its
CRC and end bit, and after a written block the CRC status and 8 clocks
of busy (our choice), as after an R1b command. The clock stops while
the FIFO is full on a read or empty on a write, as on the real
controller. With `SLOW_CARD`, which every driver sets, the divider is
the whole of `SDCDIV`; without it, in data mode (after `CMD3`), only
its low 3 bits count, as the driver's comment says. A command the card
does not answer fails after 64 clocks with `CMD_TIME_OUT`; data that
does not come fails after `SDTOUT` clocks with `REW_TIME_OUT`. `DATA_FLAG`
is set while the FIFO holds the read threshold of `SDEDM`, or the end of
a block, or has room for the write threshold; `BLOCK_IRPT` comes at the
end of each block, `BUSY_IRPT` at the end of a busy period. The
interrupt, GPU interrupt 56, is high while one of them is set and
enabled.

Without the card, or while GPIO 48 to 53 are not all ALT0, commands
time out, as with an empty slot. CRC errors never happen, but a block
of another length than `SDHBCT`, or a card on another number of lines
than `WIDE_EXT_BUS`, ends with `CRC16_ERROR`; a response of another
length than `LONG` asks for ends with `CRC7_ERROR`. What the drivers do
not show stops the machine: other offsets, accesses that are not
32-bit, reading an empty FIFO or writing a full one, a command while
another runs or, but for `CMD12`, while data moves, and bits the
drivers never set. The DMA request (DREQ 13) is not modelled yet.

The firmware leaves SDHOST at its reset values, with the card in the
idle state; drivers set it up again anyway.

## The WiFi controller (EMMC)

The BCM2835's External Mass Media Controller, an Arasan SD host
controller, is at `0x3f300000` with GPU interrupt 62, as chapter 5 of
the BCM2835 datasheet describes it. On the Pi 3 Model B it goes to the
WiFi chip, a BCM43438, which is an SDIO card only (`mmcnr`,
`non-removable` in `bcm2710-rpi-3-b.dts`). smirror models the
controller but not the chip: a command that sets up a memory card,
which the chip does not answer, ends with a command timeout after its
48 bits and the 64 clocks of Ncr, as on the real board; an SDIO command
(`CMD5`, `CMD52`, `CMD53`), which the chip would answer, stops the
machine. Its base clock is the firmware's EMMC clock, 200 MHz.

The registers are those of the datasheet, accessed 32 bits wide. The
datasheet leaves out the capability registers at `0x40` and `0x44`,
which drivers read: they hold the values that Linux's `sdhci-iproc.c`
gives this controller, as it does not trust the hardware's (blocks of
1024 bytes, 3.3 V, high speed; driver types A and C). `SLOTISR_VER`
reads SD Host Controller version 3.00, whose 10-bit clock divider and
UHS mode bits the datasheet describes, and vendor 0, its documented
reset value. Commands start when `CMDTM` is written; the software
resets and the internal clock's stability are immediate.

## USB

The USB controller at `0x3f980000`, GPU interrupt 9, is a Synopsys
DesignWare USB 2.0 On-The-Go controller (DWC2), core 2.80a. Synopsys
does not publish its databook: smirror follows Linux's and U-Boot's
`dwc2` drivers. Its identity registers hold the values that TinyUSB
measured on the BCM2711's controller, the same core: `GSNPSID`
`0x4f54280a`, `GHWCFG2` `0x228ddd50` (8 host channels, internal DMA),
`GHWCFG3` `0x0ff000e8`, `GHWCFG4` `0x1ff00020`, `GUID` `0x2708a000`.

It works as a host, as on the Pi 3, whose port is wired to the LAN9514
hub. smirror models the core's resets and FIFO flushes, the FIFO sizes,
the root port (connected once powered, enabled at high speed when
software ends its reset), the frame number, and the host channels with
buffer DMA, whose addresses are bus addresses: a transfer goes to the
device packet by packet and ends, after the time its packets take at
480 Mb/s (a packet's token, handshake and gaps count as 16 bytes, our
choice), with the transfer done, a NAK, a STALL, a babble, or a
transaction error when no device answers, as the drivers expect. Device
mode, the data FIFOs, split transactions and isochronous endpoints stop
the machine.

The USB devices follow chapter 9 of the USB 2.0 specification (device
states, control transfers, the standard requests), and the hub chapter
11 (the hub descriptor and class requests, port power and a 10 ms port
reset, the status change endpoint). On the root port is the LAN9514's
hub, as its datasheet sets it up without an EEPROM, as on the board:
vendor `0x0424`, product `0x9514`, 5 ports with port 1 not removable,
self-powered, a compound device with a transaction translator a port.
Its device release depends on the chip's revision; smirror says 2.00.
The other four ports have nothing attached.

## Ethernet

On the hub's port 1 is the LAN9514's Ethernet controller, a USB device
with a 10/100 MAC and PHY, which Linux and U-Boot drive with
`smsc95xx`. Microchip documents its core in the LAN950x datasheet;
smirror follows it, without the LAN9500A's additions:

- USB: vendor `0x0424`, product `0xec00`, self-powered with remote
  wakeup, 2 mA (the LAN9514's defaults without an EEPROM); endpoint 1
  bulk IN, 2 bulk OUT, 3 interrupt IN every 4 ms; no strings. The device
  release, 2.00, and the device protocol, 1, are those a LAN9512 board's
  `lsusb` shows (the same core); `ID_REV` holds chip ID `0xec00` (Linux's
  `ID_REV_CHIP_ID_9512_`) and revision 0, which is unknown.
- The vendor commands that read and write a register or give the
  statistics; the system and MAC registers; the PHY's through
  `MII_ACCESS`, identity `0x0007c0c3` (the LAN9500's).
- The resets: a USB reset or a lite reset puts every register back, and
  the PHY powers down after a USB reset; the PHY's reset through
  `PMT_CTL` takes 2 ms, while every transfer gets a NAK, and its soft
  reset 256 µs.
- No EEPROM, as on the board: the MAC address registers hold
  `FF:FF:FF:FF:FF:FF` and the firmware's MAC address comes through the
  device tree or U-Boot's `usbethaddr`. The EEPROM's data pin has a
  pull-down, so a read gives 0 after its 20 clocks of 1.12 µs and a
  write times out after 30 ms.
- Transmit: the TX commands A and B, frames split into buffers, padding
  to 64 bytes and the FCS, and the errors that stall bulk OUT; an 8 KB
  FIFO that NAKs when less than 512 bytes are free.
- Receive: address filtering (perfect, hash, inverse, multicast,
  promiscuous), the RX status word, errors, VLAN tags, pad stripping; a
  20 KB FIFO; one frame a bulk IN burst or several (MEF), the burst cap,
  a ZLP or a NAK when empty; the interrupt endpoint and the MAC's
  loopback.

No cable is plugged in, as nothing on the host carries the frames yet:
the link stays down, and U-Boot's `dhcp` says `Waiting for Ethernet
connection... unable to connect.` as a board without a cable does. The
model has a cable for its tests: a partner that negotiates 10 or 100
Mb/s, half or full duplex, with the link up 2 s after a reset (our
choice), and frames that cross it at once. Not modelled, and the machine
stops when software uses them: the soft reset `SRST` (a USB disconnect),
the data port, the debug registers, wake-up frames and wake-up
detection, the checksum offload engines, pause frames on demand, and
the PHY's loopback and collision test. The GPIO pins are inputs that
read 1 and never interrupt. The bulk IN delay is not waited.

## The watchdog

The power manager at `0x3f100000` holds the power domains, some reset
lines and a watchdog (its devicetree binding, `brcm,bcm2835-pm`).
Broadcom does not document it; smirror follows how Linux's and U-Boot's
drivers use its three watchdog registers:

- Every write carries the password `0x5a` in bits 31:24; a write
  without it does nothing.
- `WDOG` (`0x24`) holds the count, 20 bits of ticks at 65536 a second
  (Linux and U-Boot turn a second into `x << 16` ticks, at most
  `0xfffff`, just under 16 s). It runs down while the `WRCFG` field of
  `RSTC` (`0x1c`, bits 5:4) is `0b10`, a full reset; reading it gives the
  ticks left. When it reaches 0 the whole board resets: every device,
  the cores, and the firmware, which boots again.
- `RSTS` (`0x20`) survives that reset. The firmware reads there, in bits
  0, 2, 4, 6, 8 and 10, the partition to boot from (Linux's
  `bcm2835_wdt.c`). 0 is the usual boot; 63 asks it not to boot, which is
  how Linux powers the board off: smirror then stops, as `shutdown`.
  Another partition is not emulated and stops the machine.

U-Boot's `reset` writes 10 ticks and a full reset: the board boots again
about 153 µs later. What `RSTC` and `RSTS` hold at power on, and the
other bits of `RSTS` after a reset, are not documented: smirror starts
them at 0 and sets nothing in `RSTS`. The power domains and reset lines,
and the other kinds of watchdog reset (`WRCFG` `0b01` and `0b11`), stop
the machine.

## Boot from the SD card

On the real board, the VideoCore boots first: its boot ROM loads
`bootcode.bin` from the SD card, which loads the firmware, `start.elf`,
which sets the board up, loads the kernel and starts the Arm cores.
smirror does what that firmware does, without running it:

1. `-sd FILE` is the card. Its partition table must be an MBR, and its
   first partition a FAT12, FAT16 or FAT32 file system, the boot
   partition. Long file names work, as on the real card.
2. The boot partition must hold `bootcode.bin` and the start file, as
   on the real board, which stops without them: smirror does not run
   them, so any file of that name will do. The start file is
   `start.elf`, or what `start_file`, `start_debug=1`, `start_x=1` or
   `gpu_mem=16` choose.
3. `config.txt`, with the files it includes, is read as the
   documentation describes it: one `property=value` a line, `#`
   comments, 98 characters a line. Its filters see a Pi 3 Model B:
   `[pi3]`, `[board-type=0x8]` and `[0xc0ffee03]` (the serial number)
   match, `[gpioN=0|1]` reads the pins, `[cust_otpN...]` sees OTP rows
   that are not programmed and `[partition...]` a first boot; `[EDID=]`
   and `[tryboot]` never match, as no display is connected and nothing
   asked for a try boot. After a filter smirror does not know, nothing
   applies until `[all]`, with a warning.
4. The memory split: `gpu_mem` (76 MB by default, 16 at least),
   `gpu_mem_1024` and `total_mem`, from `config.txt` itself and not from
   an included file, as `bootcode.bin` reads them. The Arm gets the rest,
   948 MB by default, which the mailboxes and the device tree report.
5. The pins, as the firmware's `dt-blob.dts` sets them for the Pi 3
   Model B rev 1.2: GPIO 14 and 15 to UART0 (ALT0), 15 pulled up; 34 to
   39 inputs pulled up (the WiFi chip's, before Linux takes them); 40
   and 41 to PWM (ALT0, the audio jack), 42 and 43 to GPCLK1 and 2
   (ALT0) pulled down; 44 to 47 inputs pulled up; 48 to 53 to the SD
   card's controller (ALT0), pulled up. With `enable_uart=1`, GPIO 14
   and 15 go to the mini UART instead (ALT5), which is turned on at
   115200 baud, and the core clock stays at 250 MHz. `enable_uart` is 0
   by default on the Pi 3, as its console is the mini UART.
6. The kernel: `kernel=`, or `kernel8.img` with `arm_64bit=1`. On the
   Pi 3 `arm_64bit` is 0 by default, which boots `kernel7.img` as a
   32-bit kernel: smirror runs only 64-bit kernels, and stops with a
   message instead. A kernel named `kernel7.img`, `kernel7l.img` or
   `kernel.img` is 32-bit too, as the documentation says. A kernel
   that is a gzip file, as the `kernel8.img` of Raspberry Pi OS is, is
   decompressed first: the documentation says that the firmware knows
   one by its first bytes. smirror's own decompressor
   (`util/inflate.c`, after RFC 1951 and 1952) takes one gzip member
   and checks its CRC and size; anything else stops the machine with
   why. `os_prefix` is put
   before the names of the kernel, the initramfs, `cmdline.txt` and the
   device tree, if the kernel and the device tree are there with it.
7. Where the kernel goes: `kernel_address`, or, for an image with the
   Linux arm64 header (the magic `ARM\x64` at byte 56), its
   `text_offset`, 2 MB higher if that would overlap the stub; so Linux
   5.8 and later go to `0x200000`. Other images, such as U-Boot or a
   bare-metal program, go to `0x80000`.
8. The initramfs: `initramfs FILES ADDRESS|followkernel`, `ramfsfile`
   with `ramfsaddr`, or `auto_initramfs=1`. Several files, separated by
   commas, are loaded one after the other. `followkernel` puts them
   after the kernel and the memory it takes, at the next 4 KB: for a
   Linux image, its header's `image_size`, which counts its BSS, as
   Linux's `booting.rst` asks ("At least image_size bytes from the start
   of the image must be free for use by the kernel"); a kernel clears
   its BSS, which would destroy an initramfs there. The firmware's own
   alignment is not documented.
9. The command line: the first line of `cmdline.txt`, or of the file
   `cmdline=` names.
10. The device tree: `device_tree=`, or `bcm2710-rpi-3-b.dtb`, or else
    the upstream `bcm2837-rpi-3-b.dtb`; `device_tree=` with no name boots
    without one. The firmware fills in the memory node (`reg`, after
    `#address-cells` and `#size-cells` of the root), adds the command
    line after the tree's own `/chosen/bootargs`, sets
    `/chosen/linux,initrd-start` and `linux,initrd-end`, and gives the
    node that the alias `ethernet0` names the board's MAC address in
    `local-mac-address`. It goes at `device_tree_address`, or ends just
    below `0x2f000000` (or the end of the Arm's memory, or
    `device_tree_end`), on 256 bytes: U-Boot on a real Pi 3 found it at
    `0x2effb300`, but the firmware's rule is not documented.
11. The stub, as with `-kernel` below, with the kernel's address and the
    device tree's.

What `config.txt` sets but smirror does not apply, such as `dtoverlay`,
`dtparam` or the video settings, is listed in a warning, and the boot
goes on. A reset of the board runs this boot again, from the card as it
is then.

With `-kernel`, the card is not read: the kernel comes from the host,
goes where the rules of 7 put it, and boots without a device tree; the
settings are those of an empty `config.txt` with `enable_uart=1`. The
card, if any, stays in its slot.

## Boot with `-kernel`

For `kernel8.img`, the Raspberry Pi firmware does the following:

1. It loads the image at its address (see above), and a small stub of
   Arm code, armstub8, at address 0. It writes the addresses of the
   kernel and of the device tree into the stub.
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
comments say the firmware does, and writes the addresses of the kernel
and of the device tree (0 for none) into the last two words.

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

- With `-kernel`, no device tree is passed: X0 is 0.
- The firmware reads the card from its image, not through SDHOST, and
  leaves SDHOST at its reset values: what the real firmware leaves there
  is not documented.
- After a watchdog reset the firmware boots again at once: the time the
  real bootcode.bin and start.elf take is not modelled, as at power on.
  Only partition 0 boots; 63 halts.
- SDHOST's DMA requests, which Linux uses, are not modelled yet, nor is
  the DMA controller.
- The WiFi chip behind the EMMC controller is not modelled. Full- and
  low-speed devices, which need the hub's transaction translators,
  cannot be attached yet.
- The Ethernet has no cable: nothing on the host carries its frames yet.
  Its checksum offload engines, which Linux turns on, stop the machine.
- The firmware's device tree overlays and parameters (`dtoverlay`,
  `dtparam`) are not applied, nor are the other settings of `config.txt`
  that smirror does not list above; a warning names each. The firmware
  also adds things of its own to the command line, which are not
  documented, and smirror does not.
- Time is virtual, as described under The Generic Timer: the counter
  follows the instructions that smirror runs, not the host's clock.
- Of the interrupt controller's sources, only the mini UART, the GPIO,
  UART0, the mailboxes, SDHOST, the EMMC controller and USB are
  modelled yet.
- The cores have no performance monitors, so their interrupt never
  comes.
- A core sees an interrupt from another core, such as a mailbox, at its
  next instruction. The cores take turns through the same 1 µs of
  virtual time, so a core that runs later in the turn can see it up to
  1 µs before it was sent. smirror notices that the bus has gone quiet
  at the end of such a turn, so the AXI outstanding interrupt can come
  up to 1 µs late.
- The firmware gives the Arm 948 MB, but nothing uses the GPU's 76 MB
  above it: the Arm can use all of the RAM below the peripherals, as
  on the real board, without the GPU changing it.
- Turning a power domain on or off through the firmware changes
  nothing yet.
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
- With `enable_uart=0`, the real core clock rises to 400 MHz under
  load, which changes the mini UART's speed: smirror keeps it at
  250 MHz, as when idle. A program that uses UART0 must set it up
  itself; with `enable_uart=0` GPIO 14 and 15 are already UART0's. The PL011 resets to a baud rate divisor of 0,
  which is invalid, and to 5-bit characters: set `UARTIBRD` and
  `UARTFBRD` (26 and 3 for 115200 baud), then `UARTLCR_H` to 8 bits,
  before sending.

## Sources

- BCM2835 ARM Peripherals, chapter 5: the EMMC controller's registers.
  Linux,
  [sdhci-iproc.c](https://github.com/torvalds/linux/blob/master/drivers/mmc/host/sdhci-iproc.c):
  the capabilities it gives the BCM2835's controller; U-Boot,
  [bcm2835_sdhci.c](https://github.com/u-boot/u-boot/blob/master/drivers/mmc/bcm2835_sdhci.c)
  and its history: the firmware sets the EMMC clock to 200 MHz from its
  commit 7fdcd00e on.
- [Universal Serial Bus Specification, Revision 2.0](https://www.usb.org/document-library/usb-20-specification):
  the device framework (chapter 9), control transfers (8.5.3), port
  reset times (7.1.7.5) and the hub (chapter 11).
- [LAN9514/LAN9514i datasheet](https://www.microchip.com/en-us/product/lan9514),
  Microchip DS00002306A: the hub's configuration without an EEPROM
  (Tables 3-3 to 3-5), the Ethernet controller's defaults (Table
  3-11), the EEPROM pins (Table 2-1) and timing (Table 4-8).
- The power manager's watchdog: Linux's
  [bcm2835_wdt.c](https://github.com/torvalds/linux/blob/master/drivers/watchdog/bcm2835_wdt.c)
  (registers, password, ticks, the partition in `RSTS` and 63 for a
  halt), U-Boot's
  [bcm2835_wdt.c](https://github.com/u-boot/u-boot/blob/master/drivers/watchdog/bcm2835_wdt.c)
  and
  [reset.c](https://github.com/u-boot/u-boot/blob/master/arch/arm/mach-bcm283x/reset.c),
  and the devicetree binding
  [brcm,bcm2835-pm.yaml](https://github.com/torvalds/linux/blob/master/Documentation/devicetree/bindings/soc/bcm/brcm%2Cbcm2835-pm.yaml)
  with the node in
  [bcm2835-common.dtsi](https://github.com/torvalds/linux/blob/master/arch/arm/boot/dts/broadcom/bcm2835-common.dtsi)
  (`0x7e100000`, 0x114 bytes).
- [LAN950x datasheet](https://www.microchip.com/content/dam/mchp/documents/UNG/ProductDocuments/DataSheets/LAN950x-Data-Sheet-DS00001875D.pdf),
  Microchip DS00001875D: the Ethernet controller's endpoints,
  descriptors and vendor commands (5.3), its FIFOs and frame formats
  (5.4), address filtering and VLAN tags (5.5), the EEPROM controller
  (5.7), the resets (5.13), and its registers and the PHY's (chapter 7).
- A LAN9512 board's `lsusb -v`
  ([a Volumio forum post](https://community.volumio.com/uploads/short-url/lOB1cc2lCcK5LTQ7RxJU6oPWK5l.txt)):
  the Ethernet controller's device release and protocol. Linux's
  [smsc95xx.h](https://github.com/torvalds/linux/blob/master/drivers/net/usb/smsc95xx.h):
  its chip ID, `ID_REV_CHIP_ID_9512_`.
- USB: Linux's
  [drivers/usb/dwc2](https://github.com/torvalds/linux/tree/master/drivers/usb/dwc2)
  (`hw.h`, `core.c`, `hcd.c`) and U-Boot's
  [dwc2.c](https://github.com/u-boot/u-boot/blob/master/drivers/usb/host/dwc2.c):
  the controller's registers as they use them; TinyUSB's
  [dwc2_info.md](https://github.com/hathach/tinyusb/blob/master/src/portable/synopsys/dwc2/dwc2_info.md):
  the identity registers of the BCM2711's controller.
- The display: the firmware reports none when none is connected
  ([raspberrypi/firmware issue #1157](https://github.com/raspberrypi/firmware/issues/1157)),
  which U-Boot's commit 970baf16 follows.

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
- Raspberry Pi firmware wiki:
  [Mailboxes](https://github.com/raspberrypi/firmware/wiki/Mailboxes)
  and
  [Accessing mailboxes](https://github.com/raspberrypi/firmware/wiki/Accessing-mailboxes):
  the mailbox registers, their depth and flags, the channels;
  [Mailbox property interface](https://github.com/raspberrypi/firmware/wiki/Mailbox-property-interface):
  the buffer, its tags and their values.
- Linux,
  [bcm2835-mailbox.c](https://github.com/torvalds/linux/blob/master/drivers/mailbox/bcm2835-mailbox.c):
  bit 0 of the configuration register enables the interrupt;
  [raspberrypi-firmware.h](https://github.com/torvalds/linux/blob/master/include/soc/bcm2835/raspberrypi-firmware.h):
  the tags that the firmware knows;
  [raspberrypi.c](https://github.com/torvalds/linux/blob/master/drivers/firmware/raspberrypi.c):
  the firmware revision is the time it was built.
- Raspberry Pi documentation,
  [revision codes](https://github.com/raspberrypi/documentation/blob/master/documentation/asciidoc/computers/raspberry-pi/revision-codes.adoc):
  `a02082`, and serial numbers of 64 bits;
  [gpu_mem](https://github.com/raspberrypi/documentation/blob/master/documentation/asciidoc/computers/legacy_config_txt/memory.adoc):
  76 MB by default with 1 GB;
  [overclocking options](https://github.com/raspberrypi/documentation/blob/master/documentation/asciidoc/computers/config_txt/overclocking.adoc):
  the default clock rates of the Raspberry Pi 3.
- The [IEEE OUI registry](https://standards-oui.ieee.org/oui/oui.txt):
  `B8-27-EB` is the Raspberry Pi Foundation's.
- Raspberry Pi documentation, `kernel_address` in the
  [2021 version of the config.txt boot options](https://github.com/raspberrypi/documentation/blob/16480247dcac12d1f828c0f2556a3bc430de3c90/configuration/config-txt/boot.md):
  64-bit kernels load at `0x80000`; the
  [current one](https://github.com/raspberrypi/documentation/blob/master/documentation/asciidoc/computers/legacy_config_txt/boot.adoc)
  says `0x200000`, and
  [raspberrypi/firmware issue #1415](https://github.com/raspberrypi/firmware/issues/1415)
  gives the rule behind both: the header's `text_offset`, 2 MB higher
  when it would overlap the stub; images without the header still load
  at `0x80000`.
- Raspberry Pi documentation, `config.txt`:
  [the file and include](https://github.com/raspberrypi/documentation/blob/master/documentation/asciidoc/computers/config_txt/what_is_config_txt.adoc),
  [conditional filters](https://github.com/raspberrypi/documentation/blob/master/documentation/asciidoc/computers/config_txt/conditional.adoc),
  [boot options](https://github.com/raspberrypi/documentation/blob/master/documentation/asciidoc/computers/config_txt/boot.adoc)
  (`kernel`, `arm_64bit`, `initramfs`, `ramfsfile`, `auto_initramfs`,
  `os_prefix`, `enable_uart`, `start_file`) and their
  [legacy options](https://github.com/raspberrypi/documentation/blob/master/documentation/asciidoc/computers/legacy_config_txt/boot.adoc)
  (`start_x`, `start_debug`, `kernel_address`);
  [the boot sequence of the BCM2837](https://github.com/raspberrypi/documentation/blob/master/documentation/asciidoc/computers/raspberry-pi/bootflow-legacy.adoc);
  and, in the 2024 page on device trees, `device_tree`,
  `device_tree_address` and `device_tree_end`, and that the firmware
  loads `bcm2710-rpi-3-b.dtb` or an upstream one.
- [dt-blob.dts](https://github.com/raspberrypi/firmware/blob/master/extra/dt-blob.dts),
  `pins_3b2`: the pins as the firmware sets them on the Pi 3 Model B
  rev 1.2.
- The Raspberry Pi device trees,
  [bcm270x.dtsi](https://github.com/raspberrypi/linux/blob/rpi-6.6.y/arch/arm/boot/dts/broadcom/bcm270x.dtsi),
  [bcm2708-rpi.dtsi](https://github.com/raspberrypi/linux/blob/rpi-6.6.y/arch/arm/boot/dts/broadcom/bcm2708-rpi.dtsi)
  and
  [bcm283x-rpi-smsc9514.dtsi](https://github.com/raspberrypi/linux/blob/rpi-6.6.y/arch/arm/boot/dts/broadcom/bcm283x-rpi-smsc9514.dtsi):
  the memory node that the firmware fills in, the command line it adds
  to, and the Ethernet node that `ethernet0` names.
- Linux,
  [Documentation/arch/arm64/booting.rst](https://github.com/torvalds/linux/blob/master/Documentation/arch/arm64/booting.rst):
  the header of an arm64 image.
- The U-Boot mailing list,
  [May 2018](https://lists.denx.de/pipermail/u-boot/2018-May/329386.html):
  the device tree of a real Pi 3 at `0x2effb300`.
- [SD Physical Layer Simplified Specification, version 9.10](https://www.sdcard.org/downloads/pls/),
  SD Association: card identification (4.2), data transfer (4.3), the
  switch function (4.3.10), CRC7 and its examples (4.5), the commands
  (4.7.4) and the state transitions (4.8), the responses (4.9), the card
  status and the SD status (4.10), the timings Ncr and Nac (4.12), and
  the registers OCR, CID, CSD and SCR (chapter 5).
- The SDHOST drivers:
  [bcm2835-sdhost.c](https://github.com/raspberrypi/linux/blob/rpi-6.6.y/drivers/mmc/host/bcm2835-sdhost.c)
  of Raspberry Pi's Linux (registers, bits, the clock divider and its
  comment, `SET_SDHOST_CLOCK`),
  [bcm2835.c](https://github.com/torvalds/linux/blob/master/drivers/mmc/host/bcm2835.c)
  of Linux and
  [bcm2835_sdhost.c](https://github.com/u-boot/u-boot/blob/master/drivers/mmc/bcm2835_sdhost.c)
  of U-Boot (the polling, the long responses in `SDRSP0` to `SDRSP3`);
  the device tree
  [bcm283x.dtsi](https://github.com/raspberrypi/linux/blob/rpi-6.6.y/arch/arm/boot/dts/broadcom/bcm283x.dtsi):
  SDHOST at `0x7e202000`, GPU interrupt 56.
- [Devicetree Specification v0.4](https://github.com/devicetree-org/devicetree-specification/releases/tag/v0.4),
  chapter 5: the blob that smirror reads and writes.
- [ECMA-107](https://ecma-international.org/publications-and-standards/standards/ecma-107/),
  2nd edition: FAT12 and FAT16; Wikipedia,
  [File Allocation Table](https://en.wikipedia.org/wiki/File_Allocation_Table):
  FAT32, the type by the number of clusters, and long file names;
  Wikipedia, [Master boot record](https://en.wikipedia.org/wiki/Master_boot_record):
  the partition table.
- Raspberry Pi documentation, `arm_freq` in
  [config.txt, Overclocking options](https://www.raspberrypi.com/documentation/computers/config_txt.html#overclocking-options):
  the Raspberry Pi 3 runs its cores at 1200 MHz.
- Raspberry Pi documentation,
  [Configure UARTs](https://www.raspberrypi.com/documentation/computers/configuration.html#configure-uarts):
  which UART is on GPIO 14 and 15.
