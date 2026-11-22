# Status

Update this file at the end of every task, in the same commit as the
work (or the next one), so a new session starts from here.

- **Date:** 2026-11-22
- **master:** d3e76d9 (status after task 1, part 1), then this task's
  commits.
- **Board:** raspi3b. Roadmap items 1-5 done: from power on, the
  firmware HLE, U-Boot, Linux to the BusyBox shell
  (`docs/boot-logs/raspi3b-uboot-linux.log`), and an unmodified
  Raspberry Pi OS Lite to its login prompt
  (`docs/boot-logs/raspi3b-raspios.log`, `tests/raspios/`).
- **Last task:** task 1, part 2, the VideoCore's `mmal` at probe
  (machine/raspi3b_mmal): COMPONENT_CREATE, PORT_INFO_GET,
  PORT_PARAMETER_GET (SUPPORTED_ENCODINGS, CAMERA_INFO), COMPONENT_DESTROY,
  for `ril.video_decode`, `ril.video_encode`, `ril.isp`, `ril.image_fx`,
  `ril.image_encode` and `camera_info` (no camera). Pi OS makes
  /dev/video10-16, 18, 20-23, 31 as a real board with no camera; the
  session reads every queue's formats with VIDIOC_ENUM_FMT. Encodings
  from real boards' V4L2 lists where found, the rest smirror's choice
  (docs/boards/raspi3b.md, mmal). Streaming stops the machine.
  Mutation linux25: 13 survivors of 503, all equivalent.

## Next tasks (ask the owner which)

1. Task 1, part 3: `AUDS` (audio, with a host back end), its own task;
   then MMAL streaming (formats, enable, buffers, bulk transfers; the
   codecs' work on the host), `/effort xhigh`.
2. Networking to the host (a back end behind the LAN9514): Pi OS
   would then get an address and run ssh.
3. Performance phase 3, the JIT (`.claude/knowledge/performance.md`):
   a Pi OS boot takes about 10 minutes.
4. Display (HVS channels, pixelvalves, HDMI) and USB keyboard/mouse.
5. I2C, SPI, PWM devices; Bluetooth; WiFi (SDIO chip).
6. A second board: the add-board skill.

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
On Raspberry Pi OS with no display: the HVS's `DISPCTRL` and `DISPSTAT`
as the firmware leaves them (`sudo busybox devmem 0x3f400000` with vc4
blacklisted; ours read 0), and which VCHIQ services the firmware has
(`sudo vcgencmd version` works, so `GCMD` at least).
