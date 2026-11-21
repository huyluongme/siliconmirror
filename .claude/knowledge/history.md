# How the Raspberry Pi 3B was brought up

The order that worked, with what each stage needed. Commit hashes are on
master; `git log --oneline` has the rest. Use it as the model for a new
board (see the add-board skill).

## Roadmap item 1: bare-metal hello (2026-11-07/08)

core (bus, IRQ lines, virtual clock, events), the AArch64 integer
interpreter, PL011, the raspi3b machine with `-kernel`, bare-metal
tests in `tests/baremetal/` (programs built with the cross binutils,
expect files on the UART output).

## Item 2: exceptions, system registers, timer, interrupts

7a system registers (Cortex-A53 TRM 4.2 reset values), 7b synchronous
exceptions and ERET, 7c the boot stub (cores reset at EL3 into our
armstub8 equivalent, spin table at 0xd8), 7d Generic Timer, 7e IRQ/FIQ
and the BCM2836 local block (routing, mailboxes, local timer), 7f the
BCM2835 interrupt controller and PL011 interrupts, 7g UART receive
(stdio back end, raw tty, Ctrl-A x).

## Item 3: U-Boot to its prompt

8a MMU, 8b GPIO + mini UART (the Pi 3 console is the mini UART on GPIO
14/15 ALT5), group 1 = mailbox/property interface + SD card + SDHOST +
firmware HLE (MBR, FAT, config.txt, DTB fix-ups), group 2 = USB (DWC2,
hub, LAN9514 Ethernet) + the U-Boot session test (`tests/uboot/`,
release build only, local, needs the card from `scripts/uboot-card.sh`).
Then the watchdog (U-Boot `reset`). U-Boot v2026.07 rpi_3_defconfig is
the pinned version (2026.10 writes BSS before relocation and fails on
the real board too).

## Performance phases 0-2 (2026-11-14)

bench.sh; page caches for fetch and data; division-free counter reads;
`-clock host`. U-Boot to its prompt went from 75 s to 34 s (virtual) and
3 s (host clock). The JIT (phase 3) waits: see performance.md.

## Item 4: Linux with an initramfs to a shell (2026-11-14 to 11-21)

Method: build a card with the firmware's own kernel8.img
(`scripts/linux-card.sh`), boot, and turn each stop of the machine into
a work item. Exploration hacks (unmodelled registers read as zero, with
a log) are local only, gated by an environment variable, never
committed (`.claude/tools/linux_explore.sh`).

Work items in order: gzip kernels and initramfs placement; PMU and
debug registers; exclusives, acquire/release, monitors, WFE wake;
FP/SIMD in three steps (registers and loads/stores, integer SIMD, IEEE
floating point on integers, no host FPU); dwc2 device-mode registers;
mini UART byte access; CPRMAN; DMA; framebuffer and clock tags; RNG,
thermal; MPHI (dwc_otg FIQ); VCHIQ; the AXI bridges (ASB); SDHOST
BLOCK_IRPT; LAN9514 checksum offload; PHY registers; the boot-to-shell
test (`tests/linux/`, step G). Then the firmware's device tree
details (model, serial, KASLR and RNG seeds, command line), and
overlays/dtparam (af104d4).

## What is next (owner decides)

Item 5: an unmodified Raspberry Pi OS Lite image to its login prompt
(root file system on the SD card). Then networking to the host, display
and USB keyboard, I2C/SPI/PWM, Bluetooth, WiFi, the JIT. A second board
uses the add-board skill.
