# Status

Update this file at the end of every task, in the same commit as the
work (or the next one), so a new session starts from here.

- **Date:** 2026-11-22
- **master:** 3f49a7b (the session guide), then this task's commits.
- **Board:** raspi3b. Roadmap items 1-5 done: from power on, the
  firmware HLE, U-Boot, Linux to the BusyBox shell
  (`docs/boot-logs/raspi3b-uboot-linux.log`), and an unmodified
  Raspberry Pi OS Lite to its login prompt
  (`docs/boot-logs/raspi3b-raspios.log`, `tests/raspios/`).
- **Last task:** roadmap 5, Raspberry Pi OS (scripts/raspios-card.sh;
  firmware KMS and display settings; hw/i2c BSC; hw/display HVS;
  VCHIQ messages; NOTIFY_DISPLAY_DONE; mutation linux23: 56 survivors
  of 1006, all equivalent).

## Next tasks (ask the owner which)

1. The VideoCore's VCHIQ services that Pi OS asks for (`SMEM`, `mmal`;
   then `GCMD` for vcgencmd, `AUDS` for audio): now refused.
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
On Raspberry Pi OS with no display: the HVS's `DISPCTRL` and `DISPSTAT`
as the firmware leaves them (`sudo busybox devmem 0x3f400000` with vc4
blacklisted; ours read 0), and which VCHIQ services the firmware has
(`sudo vcgencmd version` works, so `GCMD` at least).
