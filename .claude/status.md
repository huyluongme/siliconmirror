# Status

Update this file at the end of every task, in the same commit as the
work (or the next one), so a new session starts from here.

- **Date:** 2026-11-23
- **master:** af1d80a (status after task 1, part 2), then this task's
  commits.
- **Board:** raspi3b. Roadmap items 1-5 done: from power on, the
  firmware HLE, U-Boot, Linux to the BusyBox shell
  (`docs/boot-logs/raspi3b-uboot-linux.log`), and an unmodified
  Raspberry Pi OS Lite to its login prompt
  (`docs/boot-logs/raspi3b-raspios.log`, `tests/raspios/`).
- **Last task:** task 1, part 3, the VideoCore's audio service `AUDS`
  (machine/raspi3b_auds) and the audio outputs (core/audio.h,
  backend/audio: `-audio wav:PATH`, `-audio alsa[:DEVICE]` through
  libasound, optional at build). snd-bcm2835's streams play in virtual
  time, 10 ms blocks, `COMPLETE` with bit 30 at the end, the volume
  applied; HDMI (no display) goes nowhere. The Pi OS session plays two
  files with `aplay` and `raspios.audio` (tests/raspios/audio_check.c)
  checks the WAV files sample for sample. Mutation linux26 (fresh run
  after two OOM-killed runs, see knowledge/mutation.md): 957 mutants,
  142 survivors, all equivalent (sizes, OOM, values after a stop, leaks,
  the 4 GB WAV limit, ALSA paths only a real card reaches; the board's
  wiring of the output is checked by raspios.audio). Boot logs unchanged:
  nothing opens AUDS at boot.

## Next tasks

Task 1 (the VideoCore's VCHIQ services) is closed (2026-11-23): what
Pi OS uses at boot and to play sound works. The owner chose networking
next.

1. **Next: networking to the host**, a back end behind the LAN9514
   (src/hw/net/lan95xx.c, which has no cable today): Pi OS would get
   an address and run ssh. First question for the owner: how frames
   reach the host (a TAP device, which needs root to set up once, or a
   user-mode network written in smirror, larger, as QEMU's -netdev
   user does: credit the idea, never read its code), and the CLI
   (`-net ...`). A new back end kind: core interface plus
   `src/backend/net/`, as audio did (docs/architecture.md).
2. Performance phase 3, the JIT (`.claude/knowledge/performance.md`):
   a Pi OS boot takes about 10 minutes.
3. Display (HVS channels, pixelvalves, HDMI) and USB keyboard/mouse.
4. I2C, SPI, PWM devices; Bluetooth; WiFi (SDIO chip).
5. A second board: the add-board skill.
6. Last, nice to have: MMAL streaming (VCHIQ bulk transfers, port
   formats, enable, buffers; ISP and image_fx in C; video decode and
   encode would need a host codec library, the owner's call),
   `/effort xhigh`. Not needed by Pi OS Lite or by switch-like boards.
   Audio extras (mixing, bulk, IEC958) only if a user needs them.

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
`amixer -c Headphones contents` after boot (the controls' starting
values; ours follow Linux: switch off, 0 dB, auto).
On Raspberry Pi OS with no display: the HVS's `DISPCTRL` and `DISPSTAT`
as the firmware leaves them (`sudo busybox devmem 0x3f400000` with vc4
blacklisted; ours read 0), and which VCHIQ services the firmware has
(`sudo vcgencmd version` works, so `GCMD` at least).
