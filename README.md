# siliconmirror

[![CI](https://github.com/huyluongme/siliconmirror/actions/workflows/ci.yml/badge.svg)](https://github.com/huyluongme/siliconmirror/actions/workflows/ci.yml)
[![License: GPL-2.0-or-later](https://img.shields.io/badge/license-GPL--2.0--or--later-blue.svg)](LICENSE)

Generic full-system emulator for multiple CPU architectures and boards,
booting unmodified bootloaders and kernels.

> **Status:** early development. On the first board, the Raspberry Pi 3
> Model B, U-Boot and Linux boot unmodified: Linux runs a shell on the
> serial console and uses the SD card, USB and the Ethernet controller,
> whose cable reaches the host through a TAP device or a capture file;
> a user-mode network, which needs no root, is still to come.

## Goals

- **Full-system emulation.** CPU, memory, interrupt controllers, timers
  and peripherals are modelled, so guest software sees a complete board.
- **Unmodified guest software.** Bootloaders and kernels built for the
  real board (for example U-Boot `rpi_3_defconfig` or the Raspberry Pi
  kernel `bcm2711_defconfig`) run without patches or special builds.
- **Boots like the real board.** On Raspberry Pi boards the emulator
  reads `config.txt`, the kernel and the device tree from the SD card
  image, and applies the device tree overlays and parameters that
  `config.txt` names, the same way the board firmware does.
- **Generic core.** CPU architectures, devices and boards are separate
  layers, so new ones can be added without changing the core.
- **Debuggable.** Built-in GDB stub and instruction tracing.

## Supported boards

| Board                  | SoC     | CPU                     | Status      |
|------------------------|---------|-------------------------|-------------|
| Raspberry Pi 3 Model B | BCM2837 | 4x Cortex-A53 (AArch64) | Linux shell |

## Limitations

- On Raspberry Pi boards the VideoCore GPU firmware (`bootcode.bin`,
  `start.elf`) is not executed. The emulator reimplements the work it
  does at boot. Everything that runs on the ARM cores (U-Boot, Linux,
  the root file system) is unmodified.
- No 3D GPU emulation.
- Not cycle-accurate.

## Building

Requirements: a C11 compiler (GCC or Clang), GNU Make, CMake 3.25 or
later and Ninja. The bare-metal tests also need the GNU binutils for
AArch64 (`binutils-aarch64-linux-gnu` on Debian and Ubuntu); without
them, those tests are skipped. `make stub` needs them too; it is only
used after changing the AArch64 code of a board's boot stub. The tests
that boot from an SD card image make the image and its device tree as
real cards are made, with `sfdisk`, `mkfs.fat`, mtools and `dtc` (the
`fdisk`, `dosfstools`, `mtools` and `device-tree-compiler` packages);
without them, those tests are skipped too. `-audio alsa`, the host's
sound card, needs ALSA's library (`libasound2-dev`); without it,
smirror is built without that option.

One more test boots U-Boot on the `raspi3b` and uses its console, as a
user would: the SD card, USB, Ethernet, a reset. It needs a card that
`scripts/uboot-card.sh` makes in `~/.cache/siliconmirror-uboot`: it
builds U-Boot 2026.07 (with `git`, an `aarch64-linux-gnu-` GCC, `bison`
and `flex`) and fetches the Raspberry Pi firmware files. The test takes
a few minutes, so only the `release` and `ci-release` builds run it
(`make PRESET=release test`); without the card it is skipped.

`scripts/linux-card.sh` makes a card for Linux in
`~/.cache/siliconmirror-linux`: the firmware files and the `kernel8.img`
of the same release, and an initramfs of BusyBox 1.38.0 (built with an
`aarch64-linux-gnu-` GCC), whose `/init` starts a shell on the serial
console. smirror boots it to that shell. A test uses it as a user
would: commands in the shell, the SD card, Ethernet, a reboot and a
power off; like the U-Boot test, it takes minutes, only the `release`
and `ci-release` builds run it, and without the card it is skipped.
Another boots the same card with `-net pcap:`, brings the link up,
pings, and checks the frames Linux sent in the capture; a third, with
`-net user`, gets an address by DHCP, pings the router, looks up a
name through the host, and connects with `nc` to its own forwarded
port through the host's loopback. The Raspberry Pi OS session has
`-net user` too: it gets its address by DHCP and answers `ssh`.

`scripts/uboot-linux-card.sh` puts both together, as a card that boots
with U-Boot would: the firmware starts U-Boot, whose `boot.scr` loads
that kernel and initramfs and starts Linux. `scripts/boot-log.sh` boots
it and keeps what the console shows in
[docs/boot-logs/raspi3b-uboot-linux.log](docs/boot-logs/raspi3b-uboot-linux.log),
which each change updates: its history shows how far the boot gets.

`scripts/raspios-card.sh` makes the card of an unmodified Raspberry Pi
OS Lite (64-bit) image, as Raspberry Pi Imager writes it: a cloud-init
`user-data` makes the user `pi` (password `raspberry`) and turns the
serial console on. smirror boots it as the real board does: a first
boot, silent on the serial console, that sets the system up and
reboots, then a second to the login prompt, in about ten minutes. A test
logs in and powers the board off (`release` builds only, skipped
without the card), and `scripts/boot-log.sh` keeps that boot too, in
[docs/boot-logs/raspi3b-raspios.log](docs/boot-logs/raspi3b-raspios.log).

`scripts/fuzz.sh [SECONDS]` fuzzes, with clang's libFuzzer, what
smirror reads from an SD card: the MBR, FAT, `config.txt`, device
trees and gzip kernels, each for SECONDS (60 by default).

`scripts/bench.sh` measures the speed of a `release` build: a loop of
200 million instructions, and U-Boot from power on to its prompt (with
the card above), each the best of 3 runs.

```sh
make test                  # build and run the tests
./build/debug/smirror -version
```

| Command               | What it does                            |
|-----------------------|-----------------------------------------|
| `make`                | Build                                   |
| `make test`           | Build and run the tests                 |
| `make run ARGS="..."` | Build and run `smirror` with arguments  |
| `make stub`           | Update the raspi3b boot stub's bytes    |
| `make format`         | Format all C sources with clang-format  |
| `make check-format`   | Check formatting without changing files |
| `make clean`          | Remove all build directories            |

Variables:

| Variable        | Values                                              |
|-----------------|-----------------------------------------------------|
| `PRESET=<name>` | A preset from the table below; `debug` by default   |
| `CC=<compiler>` | Build with another compiler, for example `CC=clang` |

| Preset       | Purpose                                     |
|--------------|---------------------------------------------|
| `debug`      | Debug build                                 |
| `release`    | Optimized build                             |
| `asan`       | Debug build with AddressSanitizer and UBSan |
| `ci`         | `asan` with warnings treated as errors      |
| `ci-release` | `release` with warnings treated as errors   |

The Makefile is a thin wrapper around the CMake presets in
[CMakePresets.json](CMakePresets.json); `cmake --workflow --preset debug`
works as well. Builds go to `build/<preset>/`, or
`build/<preset>-<compiler>/` when `CC` is given. Each build directory
contains `compile_commands.json` for editors and language servers.

## Running

```sh
./build/debug/smirror -M raspi3b -sd card.img
./build/debug/smirror -M raspi3b -kernel hello.bin
```

| Option         | What it does                                                                |
|----------------|-----------------------------------------------------------------------------|
| `-M NAME`      | Board to emulate; `-M help` lists them                                      |
| `-sd FILE`     | SD card image; without `-kernel` the board boots from it, as the real one   |
| `-kernel FILE` | Image to boot, loaded as the board's firmware would                         |
| `-serial DEV`  | Where the serial console goes: `stdio` (the default), `null` or `file:PATH` |
| `-audio DEV`   | Where the board's sound goes: nothing plugged in (`none`, the default), `wav:PATH` (WAV files, each stream in its own format) or `alsa`, `alsa:DEVICE` (the host's sound card) |
| `-net DEV`     | The other end of the board's Ethernet cable: no cable (`none`, the default), `tap:IFNAME` (the host's TAP device IFNAME), `pcap:PATH` (a capture of the frames the guest sends, nothing comes back) or `user[,hostfwd=[tcp]:[hostaddr]:hostport-[guestaddr]:guestport]...` (a network inside smirror, out through the host's sockets, no root; each `hostfwd` brings the host's TCP connections to hostport into the board) |
| `-clock CLOCK` | `virtual` (the default): time counts instructions, the same on every run; `host`: time follows the host's clock, as on the board, and U-Boot boots far faster |
| `-jit on\|off` | `on`: run the guest's code through the JIT, on Linux x86-64 hosts, with the same results as the interpreter; `off` (the default): the interpreter alone. The JIT is being built: it is not faster yet, and it does not yet see code that the guest rewrites after running it (Linux after U-Boot) |

The SD card image is used as it is, as a real card would be: what the
guest writes goes to the file, and two smirror processes cannot use the
same image at once. On the `raspi3b`, the board boots from the card as
its firmware does: `bootcode.bin` and `start.elf` must be there (smirror
does not run them), `config.txt` needs `arm_64bit=1` for a 64-bit
kernel, and `enable_uart=1` for a console; see
[docs/boards/raspi3b.md](docs/boards/raspi3b.md), "Boot from the SD
card". smirror prints a warning for each setting it does not apply.

With `-serial stdio`, the board's serial console is connected to the
terminal: what the guest sends shows up there, and what you type goes
to the guest. On the `raspi3b`, that is a serial cable on GPIO 14 and
15 of the header, which reaches the UART that the pins are set to, the
mini UART as the firmware leaves them (see
[docs/boards/raspi3b.md](docs/boards/raspi3b.md)). The terminal is in raw mode meanwhile, so Ctrl-C
goes to the guest too; press **Ctrl-A x** to quit smirror, and Ctrl-A
Ctrl-A to send a Ctrl-A. When standard input is a file or a pipe, its
bytes go to the guest as they are, until its end. `file:PATH` and `null`
give the guest no input.

On the `raspi3b`, `-audio` is what is plugged into the headphone jack:
what the guest plays there (on Raspberry Pi OS, ALSA's `Headphones`
card) goes to it. `-audio wav:PATH` writes it to PATH, a new file
(`PATH-1.wav`...) for each change of format; `-audio alsa` plays it on
the host. To hear it at the guest's pace, add `-clock host`: with the
virtual clock, a machine that runs ahead of the host waits for the
sound card, and one that falls behind leaves gaps.

On the `raspi3b`, `-net` is what the RJ45 jack's cable is plugged
into; with it, the link comes up at 100 Mb/s. `-net tap:tap0` joins the
board to the host's TAP interface `tap0`, which root makes once and
gives to the user, so that smirror runs without root:

    sudo ip tuntap add dev tap0 mode tap user $USER
    sudo ip link set tap0 up

The host then routes, bridges or serves DHCP on `tap0` as on any
interface. `-net pcap:PATH` writes what the guest sends to PATH, for
Wireshark or tcpdump, each frame at its virtual time.

`-net user` needs no root: the cable goes into a small router inside
smirror, after the idea of QEMU's user networking and with its
addresses. Its DHCP server gives the board 10.0.2.15, with the router
10.0.2.2 and the name server 10.0.2.3; the router answers ping, and
UDP leaves through the host's own sockets: to 10.0.2.3 port 53 it
reaches the host's name server (`/etc/resolv.conf`), to 10.0.2.2 the
host's loopback, 127.0.0.1, elsewhere the address itself. TCP goes the
same ways: the router ends the board's connections and the host's
sockets carry the bytes on. `hostfwd=tcp::2222-:22` is the other way,
as in QEMU: a connection to the host's port 2222 comes to the board's
port 22, so `ssh -p 2222 pi@localhost` reaches a Raspberry Pi OS
board. Without hostaddr only the host itself can connect (127.0.0.1);
`0.0.0.0` lets the network in. The router drops IPv6.

smirror stops when every CPU waits and nothing can wake them, for
example when the program ends in a `WFI` loop, and when the guest does
something that is not emulated yet. While the guest listens on its UART
and more input can come, input can wake it, so smirror waits for it. It then prints why on standard
error, and exits with a failure status in the second case. Exceptions,
such as an undefined instruction or a misaligned access, are taken as
the real CPU takes them: a program that has not set up its vector table
jumps to whatever is there, and may run forever, as on the real board. The board
pages in [docs/boards/](docs/boards/) describe how each board boots.

## Documentation

- [Architecture](docs/architecture.md): source tree, layers and naming.
- [Raspberry Pi 3 Model B](docs/boards/raspi3b.md): memory map, Generic
  Timer, boot and differences from the real board.

## Roadmap

1. Core (bus, interrupt lines, virtual clock), AArch64 interpreter
   (integer subset), PL011 UART: bare-metal "hello world". Done.
2. Exceptions, system registers, generic timer, interrupt controllers.
   Done.
3. MMU, SD host, mailbox, firmware emulation: U-Boot prompt. Done.
4. SMP (spin table): Linux with an initramfs to a serial shell. Done.
5. Root file system on SD: Raspberry Pi OS Lite login. Done.
6. USB, networking (Ethernet, and the WiFi chip on SDIO), framebuffer.

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

Command-line options, and the Ctrl-A keys of the terminal, follow the
conventions of [QEMU](https://www.qemu.org/), so QEMU users can feel at
home.

## Trademarks

Raspberry Pi is a trademark of Raspberry Pi Ltd. siliconmirror is an
independent project and is not affiliated with or endorsed by
Raspberry Pi Ltd.
