# siliconmirror

[![CI](https://github.com/huyluongme/siliconmirror/actions/workflows/ci.yml/badge.svg)](https://github.com/huyluongme/siliconmirror/actions/workflows/ci.yml)
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

## Building

Requirements: a C11 compiler (GCC or Clang), GNU Make, CMake 3.25 or
later and Ninja.

```sh
make test                  # build and run the tests
./build/debug/smirror -version
```

| Command               | What it does                            |
|-----------------------|-----------------------------------------|
| `make`                | Build                                   |
| `make test`           | Build and run the tests                 |
| `make run ARGS="..."` | Build and run `smirror` with arguments  |
| `make format`         | Format all C sources with clang-format  |
| `make check-format`   | Check formatting without changing files |
| `make clean`          | Remove all build directories            |

Variables:

| Variable        | Values                                                |
|-----------------|-------------------------------------------------------|
| `PRESET=<name>` | `debug` (default), `release`, `asan`, `ci`            |
| `CC=<compiler>` | Build with another compiler, for example `CC=clang`   |

| Preset    | Purpose                                     |
|-----------|---------------------------------------------|
| `debug`   | Debug build                                 |
| `release` | Optimized build                             |
| `asan`    | Debug build with AddressSanitizer and UBSan |
| `ci`      | `asan` with warnings treated as errors      |

The Makefile is a thin wrapper around the CMake presets in
[CMakePresets.json](CMakePresets.json); `cmake --workflow --preset debug`
works as well. Builds go to `build/<preset>/`, or
`build/<preset>-<compiler>/` when `CC` is given. Each build directory
contains `compile_commands.json` for editors and language servers.

## Documentation

- [Architecture](docs/architecture.md): source tree, layers and naming.

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

## Acknowledgements

Command-line options follow the conventions of
[QEMU](https://www.qemu.org/), so QEMU users can feel at home.

## Trademarks

Raspberry Pi is a trademark of Raspberry Pi Ltd. siliconmirror is an
independent project and is not affiliated with or endorsed by
Raspberry Pi Ltd.
