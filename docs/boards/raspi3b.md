# Raspberry Pi 3 Model B (`raspi3b`)

```sh
smirror -M raspi3b -kernel kernel8.img
```

## Hardware

| Part | Real board              | Modelled                      |
|------|-------------------------|-------------------------------|
| SoC  | Broadcom BCM2837        | Partly, see the memory map    |
| CPU  | 4 Cortex-A53, Armv8.0-A | AArch64 only, with FEAT_CRC32 |
| RAM  | 1 GB                    | Everything below 0x3f000000   |

The Cortex-A53 of the Raspberry Pi 3 has the CRC32 instructions but not
the Cryptographic Extension; the model follows it.

## Memory map

Addresses as the Arm cores see them. The peripherals sit at bus address
`0x7e000000` and up, which the Arm cores see at `0x3f000000`.

| Address      | Size    | Contents                                                    | Modelled                   |
|--------------|---------|-------------------------------------------------------------|----------------------------|
| `0x00000000` | 1008 MB | RAM                                                         | Yes                        |
| `0x3f000000` | 16 MB   | Peripherals                                                 | Partly                     |
| `0x3f201000` | 4 KB    | UART0, a PL011 with 16-entry FIFOs                          | Yes, without its interrupt |
| `0x40000000` | 4 KB    | Arm local peripherals: interrupt routing, timers, mailboxes | Not yet                    |

Accessing an address that is not modelled stops the machine with a data
abort message.

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

smirror cannot run the stub yet: it needs exception returns and System
registers. Instead, the board starts each core directly in the state
that the stub leaves it in. It also writes a waiting loop of its own at
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
- The firmware initializes the UART that it uses for its console. With
  `-kernel`, the program must set up UART0 itself. In particular, the
  PL011 resets to 5-bit characters: set UARTLCR_H to 8 bits first.

## Sources

- Linux device trees
  [bcm2837.dtsi](https://github.com/torvalds/linux/blob/master/arch/arm/boot/dts/broadcom/bcm2837.dtsi)
  and
  [bcm283x.dtsi](https://github.com/torvalds/linux/blob/master/arch/arm/boot/dts/broadcom/bcm283x.dtsi):
  peripheral addresses, cores and their spin table entries, UART0.
- [BCM2835 ARM Peripherals](https://datasheets.raspberrypi.com/bcm2835/bcm2835-peripherals.pdf),
  section 13: the PL011 and its FIFOs.
- [Trusted Firmware-A, Raspberry Pi 3](https://github.com/ARM-software/arm-trusted-firmware/blob/master/docs/plat/rpi3.rst):
  RAM size, and the default stub that starts `kernel8.img` at EL2.
- The default stub,
  [armstub8.S](https://github.com/raspberrypi/tools/blob/master/armstubs/armstub8.S),
  read for its behavior only.
- Raspberry Pi documentation, `kernel_address` in the
  [2021 version of the config.txt boot options](https://github.com/raspberrypi/documentation/blob/16480247dcac12d1f828c0f2556a3bc430de3c90/configuration/config-txt/boot.md):
  64-bit kernels load at `0x80000`.
- Raspberry Pi documentation,
  [Configure UARTs](https://www.raspberrypi.com/documentation/computers/configuration.html#configure-uarts):
  which UART is on GPIO 14 and 15.
