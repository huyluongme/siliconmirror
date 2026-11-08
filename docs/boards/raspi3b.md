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

1. It loads the image at `0x80000`.
2. All four cores start at address 0, at EL3, in a small stub of Arm
   code. The stub moves each core to EL2, with D, A, I and F masked.
3. Core 0 jumps to the image with X0 holding the address of the device
   tree and X1 to X3 zero.
4. Cores 1 to 3 wait in a loop. Core *n* reads the 64-bit word at
   `0xd8 + 8 * n`, its entry in the spin table. When the operating system
   writes an address there and executes `SEV`, the core jumps to that
   address with X0 to X3 zero.

smirror does not run the stub yet. Instead, the board starts each core
directly in the state that the stub leaves it in, with the System
registers that the stub sets up:

| Register       | Value        | Meaning                                                      |
|----------------|--------------|--------------------------------------------------------------|
| `SCR_EL3`      | `0x5b1`      | EL2 and EL1 are Non-secure and AArch64; `HVC` on, `SMC` off  |
| `ACTLR_EL3`    | `0x73`       | EL2 may write the Cortex-A53 configuration registers         |
| `CPUECTLR_EL1` | `0x40`       | SMPEN: the core takes part in cache coherency                |
| `CPTR_EL3`     | `0`          | no traps to EL3, of FP and SIMD or anything else             |
| `SCTLR_EL2`    | `0x30c50830` | MMU, caches and alignment checks off, stack alignment too    |
| `SPSR_EL3`     | `0x3c9`      | what the `ERET` to EL2 used: EL2h, with D, A, I and F masked |

The stub also sets `CNTFRQ_EL0` and `CNTVOFF_EL2`; they come with the
Generic Timer. The board also writes a waiting loop of its own at
address 0, which follows the same spin-table protocol: X5 holds the
address of the core's spin table entry, and the loop checks the entry
before each `WFE`.

The machine stops when every core waits in `WFI` or `WFE` and nothing can
wake them, so a program that ends with a `WFI` loop makes smirror exit.

## Differences from the real board

- No device tree is passed yet: X0 is 0.
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
  configuration registers and the `ACTLR` bits that control them.
- `/proc/cpuinfo` of a Raspberry Pi 3, for example in
  [this gist](https://gist.github.com/fawick/d142633de52b31d0dade):
  CPU part `0xd03`, revision 4, so a Cortex-A53 r0p4.
- [BCM2835 ARM Peripherals](https://datasheets.raspberrypi.com/bcm2835/bcm2835-peripherals.pdf),
  section 13: the PL011 and its FIFOs.
- [Trusted Firmware-A, Raspberry Pi 3](https://github.com/ARM-software/arm-trusted-firmware/blob/master/docs/plat/rpi3.rst):
  RAM size, and the default stub that starts `kernel8.img` at EL2.
- The default stub,
  [armstub8.S](https://github.com/raspberrypi/tools/blob/master/armstubs/armstub8.S),
  read for its behavior and the register values it writes only.
- Raspberry Pi documentation, `kernel_address` in the
  [2021 version of the config.txt boot options](https://github.com/raspberrypi/documentation/blob/16480247dcac12d1f828c0f2556a3bc430de3c90/configuration/config-txt/boot.md):
  64-bit kernels load at `0x80000`.
- Raspberry Pi documentation,
  [Configure UARTs](https://www.raspberrypi.com/documentation/computers/configuration.html#configure-uarts):
  which UART is on GPIO 14 and 15.
