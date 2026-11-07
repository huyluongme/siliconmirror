# siliconmirror

[![License: GPL-2.0-or-later](https://img.shields.io/badge/license-GPL--2.0--or--later-blue.svg)](LICENSE)

Generic full-system emulator for multiple CPU architectures and boards,
booting unmodified bootloaders and kernels.

> **Status:** early development. Nothing runs yet. The first target
> board is the Raspberry Pi 3 Model B.

## Goals

- **Full-system emulation.** CPU, memory, interrupt controllers, timers
  and peripherals are modelled, so guest software sees a complete board.
- **Unmodified guest software.** Bootloaders and kernels built for the
  real board (for example U-Boot `rpi_3_defconfig` or the Raspberry Pi
  kernel `bcm2711_defconfig`) run without patches or special builds.
- **Boots like the real board.** On Raspberry Pi boards the emulator
  reads `config.txt`, the kernel and the device tree from the SD card
  image, the same way the board firmware does.
- **Generic core.** CPU architectures, devices and boards are separate
  layers, so new ones can be added without changing the core.
- **Debuggable.** Built-in GDB stub and instruction tracing.

## Supported boards

| Board                  | SoC     | CPU                     | Status  |
|------------------------|---------|-------------------------|---------|
| Raspberry Pi 3 Model B | BCM2837 | 4x Cortex-A53 (AArch64) | Planned |

## Limitations

- On Raspberry Pi boards the VideoCore GPU firmware (`bootcode.bin`,
  `start.elf`) is not executed. The emulator reimplements the work it
  does at boot. Everything that runs on the ARM cores (U-Boot, Linux,
  the root file system) is unmodified.
- No 3D GPU emulation.
- Not cycle-accurate.

## Roadmap

1. Core (bus, interrupt lines, virtual clock), AArch64 interpreter
   (integer subset), PL011 UART: bare-metal "hello world".
2. Exceptions, system registers, generic timer, interrupt controllers.
3. MMU, SD host, mailbox, firmware emulation: U-Boot prompt.
4. SMP (spin table): Linux with an initramfs to a serial shell.
5. Root file system on SD, FP/SIMD: Raspberry Pi OS Lite login.
6. USB, networking, framebuffer.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md).

## License

siliconmirror is licensed under the GNU General Public License,
version 2 or (at your option) any later version. See [LICENSE](LICENSE)
for the full text.

Each source file starts with an SPDX license identifier:

```
SPDX-License-Identifier: GPL-2.0-or-later
```

## Trademarks

Raspberry Pi is a trademark of Raspberry Pi Ltd. siliconmirror is an
independent project and is not affiliated with or endorsed by
Raspberry Pi Ltd.
