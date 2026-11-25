# Status

Update this file at the end of every task, in the same commit as the
work (or the next one), so a new session starts from here.

- **Date:** 2026-11-25
- **master:** d77d2e4 (the profile before the JIT), then the counter's
  fast path.
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
- **Last task:** the counter's fast path, before the JIT: MRS of
  CNTPCT_EL0 and CNTVCT_EL0 goes straight to `arm64_timer_read_count()`
  (timer.c) with their traps, ahead of the System register table search;
  `arm64_timer_trap()` no longer handles them, so the tests see a wrong
  fast path. U-Boot to the prompt 37.8 s -> 33.9 s. Mutation perf3: 53,
  one survivor, equivalent (the `uint16_t` cast drops insn bit 21). And
  a flaky test fixed: three CLI tests shared one card image, which
  smirror locks, so in a parallel ctest one sometimes saw it "in use";
  each has its own files now.

## Next tasks

1. **Next: performance phase 3a.1**, the x86-64 emitter and executable
   memory; then 3a.2 (blocks, dispatcher, `-jit`), 3a.3 (invalidation),
   3a.4 (both modes in tests, bench). The design agreed with the owner
   is in `.claude/knowledge/performance.md` ("Phase 3a"). Specs:
   `~/specs/intel_sdm.txt`, `sysv_amd64_abi.txt`. `/effort high` for
   3a.1, `xhigh` for 3a.2 and 3a.3.
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
