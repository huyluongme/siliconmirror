---
name: add-board
description: Start and carry out the bring-up of a new board in siliconmirror (for example a network switch's SoC), from research to a full boot (boot ROM or firmware stage, bootloader, kernel), the way the Raspberry Pi 3B was done. Use when the owner wants a new board or a new CPU architecture.
argument-hint: <board name, e.g. "a Marvell Armada 3720 switch">
---

# Add a board: $ARGUMENTS

The Pi 3B's bring-up (`.claude/knowledge/history.md`) is the model: small
steps, each approved, each with tests, ending with real software booting
unmodified. The owner's goal is the **full boot chain**, so the boot
stages are the spine of the plan, not an afterthought.

## Stage 0: research, then a plan for the owner

Find, from public sources (say which), and write in a first draft of
`docs/boards/<board>.md`:

- SoC, CPU architecture and cores, RAM, the memory map (the Linux and
  U-Boot device trees of the board are the fastest source).
- **The boot chain**: what the boot ROM does (where it reads the next
  stage: SPI NOR, NAND, eMMC, SD, UART; the image header format, e.g.
  Marvell kwbimage or a vendor's), the stages after it (SPL, TF-A,
  U-Boot), and how the kernel gets its device tree.
- The console UART, storage controllers, Ethernet / switch ASIC,
  interrupt controller, timers: each device with its Linux and U-Boot
  driver names and whether a datasheet is public.
- Which software to boot, pinned: a bootloader release and defconfig,
  a kernel (the vendor's or mainline), a root file system or vendor OS
  (switches often run ONIE, then SONiC or a vendor NOS).

Switch SoC datasheets are often under NDA. Then the drivers of Linux and
U-Boot show what the guest does; say so in the docs, and stop the
machine on everything they do not show. Ask the owner for documents only
they can get (an NDA datasheet, a dump from a real device).

Present the plan to the owner in Vietnamese: the stages below, filled
in for this board, with the open questions (such as: emulate the boot ROM
at a high level or run its image, if a legal copy exists). Wait for
approval.

## Stage 1: the CPU

- An AArch64 core (Cortex-A53/A55/A72...): reuse `src/cpu/arm64`; add the
  core's ID and reset values from its TRM, as `cortex_a53.c` does, and
  stop on features the core lacks.
- Another architecture (ARMv7, MIPS, PowerPC...): a new `src/cpu/<arch>/`
  behind `core/cpu.h`, with its own unit tests and bare-metal tests.
  Large: split it (integer, exceptions, MMU, FP) and agree the split.
- Add the module to the table of `docs/architecture.md` and to
  `scripts/check-layering.sh`.

## Stage 2: the machine and a bare-metal hello

- `src/machine/<board>.c` creates CPUs and RAM, maps devices, wires
  interrupts; list it in `machine/board.c` for `-M`.
- Devices go in `src/hw/<kind>/<chip>.c`, by kind, not by board: reuse
  the existing ones (PL011, the 16550-like mini UART, DWC2, SD cards...)
  when the chip is the same, and check the differences against its
  datasheet.
- A bare-metal program prints on the console UART:
  `tests/baremetal/` with an expect file.

## Stage 3: exceptions, timers, interrupts

The same order as the Pi's item 2: system registers with reset values,
exceptions, the generic timer or the SoC timer, the interrupt
controllers (GIC on most Arm SoCs: write it from the GIC architecture
spec), UART receive. Bare-metal tests for each.

## Stage 4: the boot ROM stage and the bootloader

- The storage the boot ROM reads (SPI NOR, NAND, eMMC, SD), modelled
  from its spec, with a back end on an image file.
- The boot ROM: at a high level in `src/machine/<board>_bootrom.c` (read
  the header, load and check the next stage, set the registers it hands
  over), unless its image can be run. Document every choice.
- A script that builds the flash or card image from pinned sources
  (sha256), like `scripts/uboot-card.sh`; a session test
  (`tests/uboot/`-style expect script, release build, skips without the
  image) that reaches the bootloader prompt and runs a few commands.

## Stage 5: the kernel to a shell

- Boot the pinned kernel with an initramfs; every stop of the machine
  is a work item. A local exploration patch (unmodelled registers read
  as zero, with a log, behind an environment variable, never committed)
  lists many stops in one run.
- Order the work items (the Pi's list in history.md is a guide), one
  task each, approved, finished with `/finish-task`.
- A session test of the boot to the shell, and `scripts/boot-log.sh`
  extended to this board's chain, committed with every task.

## Stage 6: the real system

The vendor OS or root file system from its storage, the switch ASIC as
far as the OS needs it, networking to the host. Agree each step.

## Every step

- Specs first: read only the needed sections, cite them in comments.
- Unmodelled behaviour stops the machine; silent docs mean search, then
  choose and document.
- Unit tests per module; the five presets; the full mutation run; the
  board page; the boot log. `/finish-task` does the end of every step.
- Keep `.claude/status.md` and the board's line in `README.md`'s roadmap
  current.
