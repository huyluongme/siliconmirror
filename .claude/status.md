# Status

Update this file at the end of every task, in the same commit as the
work (or the next one), so a new session starts from here.

- **Date:** 2026-11-22
- **master:** 81e1e94 (status after roadmap 5), then this task's commits.
- **Board:** raspi3b. Roadmap items 1-5 done: from power on, the
  firmware HLE, U-Boot, Linux to the BusyBox shell
  (`docs/boot-logs/raspi3b-uboot-linux.log`), and an unmodified
  Raspberry Pi OS Lite to its login prompt
  (`docs/boot-logs/raspi3b-raspios.log`, `tests/raspios/`).
- **Last task:** task 1, part 1, the VideoCore's services: VCHIQ's
  service layer (OPENACK, DATA both ways, CLOSE, messages waiting for
  slots), `SMEM` (machine/raspi3b_smem: CLIENT_VERSION, IMPORT, FREE,
  RELEASED), and vcgencmd's tag `GET_GENCMD_RESULT` (0x00030080; Pi OS's
  vcgencmd uses it, not VCHIQ `GCMD`); the Pi OS session logs in and
  runs vcgencmd. Mutation linux24: 44 survivors of 845, all equivalent.

## Next tasks (ask the owner which)

1. Task 1, part 2: `mmal`, to the point where bcm2835-codec, -isp and
   -camera probe as on a board with no camera (`/effort xhigh`; see
   `.claude/knowledge/boot-chain.md` for what each probe sends). Then
   `AUDS` (audio, with a host back end) as its own task.
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
`vcgencmd version`, `measure_temp`, `get_mem arm`, `measure_volts`,
`get_throttled`, and `for c in arm core v3d h264 isp uart emmc; do
vcgencmd measure_clock $c; done` (the shapes and clock numbers ours
follow), and `vcgencmd foo` (what an unknown command answers).
On Raspberry Pi OS with no display: the HVS's `DISPCTRL` and `DISPSTAT`
as the firmware leaves them (`sudo busybox devmem 0x3f400000` with vc4
blacklisted; ours read 0), and which VCHIQ services the firmware has
(`sudo vcgencmd version` works, so `GCMD` at least).
