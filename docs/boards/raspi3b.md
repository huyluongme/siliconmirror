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
  behaves as if there were none.
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
implement yet, such as `CNTFRQ_EL0` of the Generic Timer, stops the
machine with the register's name. Encodings that the core does not have
are UNDEFINED.

## Memory map

Addresses as the Arm cores see them. The peripherals sit at bus address
`0x7e000000` and up, which the Arm cores see at `0x3f000000`.

| Address      | Size    | Contents                                                    | Modelled                   |
|--------------|---------|-------------------------------------------------------------|----------------------------|
| `0x00000000` | 1008 MB | RAM                                                         | Yes                        |
| `0x3f000000` | 16 MB   | Peripherals                                                 | Partly                     |
| `0x3f201000` | 4 KB    | UART0, a PL011 with 16-entry FIFOs                          | Yes, without its interrupt |
| `0x40000000` | 4 KB    | Arm local peripherals: interrupt routing, timers, mailboxes | Not yet                    |

Accessing an address where nothing is modelled stops the machine with
an error that names the access: what the real board does there is not
known.

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

At EL3, every core writes these System registers:

| Register       | Value              | Meaning                                                      |
|----------------|--------------------|--------------------------------------------------------------|
| `L2CTLR_EL1`   | bits 5 and 1 set   | asks for slower L2 RAM; the Cortex-A53 ignores it            |
| `CPTR_EL3`     | `0`                | no traps to EL3, of FP and SIMD or anything else             |
| `SCR_EL3`      | `0x5b1`            | EL2 and EL1 are Non-secure and AArch64; `HVC` on, `SMC` off  |
| `ACTLR_EL3`    | `0x73`             | EL2 may write the Cortex-A53 configuration registers         |
| `CPUECTLR_EL1` | `0x40`             | SMPEN: the core takes part in cache coherency                |
| `SCTLR_EL2`    | `0x30c50830`       | MMU, caches and alignment checks off, stack alignment too    |
| `SPSR_EL3`     | `0x3c9`            | for the `ERET` to EL2: EL2h, with D, A, I and F masked       |

armstub8 also sets the Generic Timer's frequency, `CNTFRQ_EL0`, to
19.2 MHz and `CNTVOFF_EL2` to 0, and sets up the timer of the ARM local
peripherals: 0 at `0x40000000` (control) and `0x80000000` at
`0x40000008` (prescaler). smirror's stub does not yet: it will when the
Generic Timer and the ARM local peripherals are modelled.

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

## Differences from the real board

- No device tree is passed yet: X0 is 0.
- The boot stub does not set up the Generic Timer and the ARM local
  peripherals yet, which are not modelled yet.
- The GPU's share of the RAM, a firmware setting, is not modelled: all
  of the RAM below the peripherals is free.
- GPIO pin functions are not modelled, and UART0 always goes to
  `-serial`, as in QEMU. On a real Raspberry Pi 3, UART0 drives the
  Bluetooth module by default, and the serial console on GPIO 14 and 15
  is UART1, the mini UART, which is not modelled yet.
- The Cortex-A53 manual says which `ACTLR_EL3` and `ACTLR_EL2` bits
  allow a lower Exception level to write the configuration registers,
  but not what happens when the bit is 0. smirror traps the write to
  the Exception level whose `ACTLR` forbids it.
- The firmware initializes the UART that it uses for its console. With
  `-kernel`, the program must set up UART0 itself. In particular, the
  PL011 resets to 5-bit characters: set UARTLCR_H to 8 bits first.

## Sources

- Linux device trees
  [bcm2837.dtsi](https://github.com/torvalds/linux/blob/master/arch/arm/boot/dts/broadcom/bcm2837.dtsi)
  and
  [bcm283x.dtsi](https://github.com/torvalds/linux/blob/master/arch/arm/boot/dts/broadcom/bcm283x.dtsi):
  peripheral addresses, cores and their spin table entries, UART0.
- [Arm Cortex-A53 MPCore Processor Technical Reference Manual](https://developer.arm.com/documentation/ddi0500/j/),
  ARM DDI 0500J, chapter 4: identification and reset values, the
  configuration registers and the `ACTLR` bits that control them
  (`ACTLR_EL3` 4.3.33, `L2CTLR_EL1` 4.3.64, `CPUECTLR_EL1` 4.3.79).
- `/proc/cpuinfo` of a Raspberry Pi 3, for example in
  [this gist](https://gist.github.com/fawick/d142633de52b31d0dade):
  CPU part `0xd03`, revision 4, so a Cortex-A53 r0p4.
- [BCM2835 ARM Peripherals](https://datasheets.raspberrypi.com/bcm2835/bcm2835-peripherals.pdf),
  section 13: the PL011 and its FIFOs.
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
  `SPSR_EL3` (C5.2.23) and the Event Register (D1.7.1.1).
- Raspberry Pi documentation, `kernel_address` in the
  [2021 version of the config.txt boot options](https://github.com/raspberrypi/documentation/blob/16480247dcac12d1f828c0f2556a3bc430de3c90/configuration/config-txt/boot.md):
  64-bit kernels load at `0x80000`.
- Raspberry Pi documentation,
  [Configure UARTs](https://www.raspberrypi.com/documentation/computers/configuration.html#configure-uarts):
  which UART is on GPIO 14 and 15.
