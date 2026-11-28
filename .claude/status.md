# Status

Update this file at the end of every task, in the same commit as the
work (or the next one), so a new session starts from here.

- **Date:** 2026-11-28
- **master:** b8ef0ae (phase 3a.3 and its notes; 3a.4 on top once
  committed).
- **Phase 3.0 done:** the profile before the JIT, in
  `.claude/knowledge/performance.md` ("Phase 3.0"): 92 % of Pi OS's host
  time is the interpreter; the guest code is flat integer, load/store
  and branch code; U-Boot's waits are most of the Linux session.
- **Board:** raspi3b. Roadmap items 1-5 done: from power on, the
  firmware HLE, U-Boot, Linux to the BusyBox shell
  (`docs/boot-logs/raspi3b-uboot-linux.log`), and an unmodified
  Raspberry Pi OS Lite to its login prompt
  (`docs/boot-logs/raspi3b-raspios.log`, `tests/raspios/`).
  Networking to the host is done: `-net tap`, `-net pcap`, `-net user`
  with TCP and `hostfwd`.
- **Last task:** performance phase 3a.4: both modes in the tests.
  Each bare-metal program runs again as `<name>-jit` with `-jit on`
  (`add_guest_test()`), each U-Boot, Linux and Pi OS session has a
  `-jit` twin, and `scripts/boot-log.sh` boots each chain in both modes
  at once and fails unless the logs are byte-identical: they are, Pi OS
  included (its first run with the JIT). `scripts/bench.sh` measures
  both modes (loop 2.27 -> 2.48 s, U-Boot 37.6 -> 47.1 s with `-jit
  on`: 3b and 3e bring the speed). The single-instruction unit tests
  stay interpreter-only (owner's choice); 3b adds a differential test
  of each translated instruction. No C changed: two hand-made JIT
  faults instead of a mutation run (`performance.md`, "3a.4").

## Next tasks

1. **Next: performance phase 3b**: integer ALU and branches in native
   code, with a differential test in test_arm64_jit.c of each
   translated instruction against the interpreter on many states. The
   design agreed with the owner is in `.claude/knowledge/performance.md`
   ("Phase 3a", "Phase 3"); `/effort xhigh` for the design of 3b.
2. The console hang: typing while the kernel prints a line (see
   tests/linux/user.txt); find which side (PL011 or the guest) loops.
3. Display (HVS channels, pixelvalves, HDMI) and USB keyboard/mouse.
4. I2C, SPI, PWM devices; Bluetooth; WiFi (SDIO chip).
5. A second board: the add-board skill.
6. Last, nice to have: MMAL streaming (VCHIQ bulk transfers, port
   formats, enable, buffers; ISP and image_fx in C; video decode and
   encode would need a host codec library, the owner's call),
   `/effort xhigh`. Not needed by Pi OS Lite or by switch-like boards.
   Audio extras (mixing, bulk, IEC958) only if a user needs them.
   UDP forwards (`hostfwd=udp:`) if a user needs them.

## Open questions only a real board can answer

`lsusb -v -d 0424:ec00 | grep -E "bcdDevice|bDeviceProtocol"`,
`lsusb -v -d 0424:9514 | grep bmAttributes` (ours 0xC0; a LAN9512 shows
0xE0), `sudo ethtool -d eth0 | head -5` (ID_REV revision; ours 0).
`v4l2-ctl -d 10 --list-formats-out` (ours: MPG4 H264 H263 MJPG; the
order after MPG4 and H263 itself are guesses), `-d 18 --list-formats-out`
and `--list-formats` (ours: YU12 NC12), `-d 31 --list-formats-out`.
`vcgencmd version`, `measure_temp`, `get_mem arm`, `measure_volts`,
`get_throttled`, and `for c in arm core v3d h264 isp uart emmc; do
vcgencmd measure_clock $c; done` (the shapes and clock numbers ours
follow), and `vcgencmd foo` (what an unknown command answers).
`sudo ethtool eth0` with a cable, and `cat /sys/class/net/eth0/carrier`
right after `ip link set eth0 down; ip link set eth0 up` (whether the
lite reset at open drops the link: ours keeps it).
`amixer -c Headphones contents` after boot (the controls' starting
values; ours follow Linux: switch off, 0 dB, auto).
On Raspberry Pi OS with no display: the HVS's `DISPCTRL` and `DISPSTAT`
as the firmware leaves them (`sudo busybox devmem 0x3f400000` with vc4
blacklisted; ours read 0), and which VCHIQ services the firmware has
(`sudo vcgencmd version` works, so `GCMD` at least).
