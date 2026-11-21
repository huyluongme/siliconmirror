# Status

Update this file at the end of every task, in the same commit as the
work (or the next one), so a new session starts from here.

- **Date:** 2026-11-21
- **master:** af104d4 (overlays, CI green), then the commit that added
  `CLAUDE.md` and `.claude/`.
- **Board:** raspi3b. Roadmap items 1-4 done: from power on, the
  firmware HLE, U-Boot, then Linux to the BusyBox shell
  (`docs/boot-logs/raspi3b-uboot-linux.log`).
- **Last task:** device tree overlays and `dtparam` (boot/rpi_overlay,
  firmware applies them; cards carry overlays/; mutation linux22: 101
  survivors of 1447, all equivalent).

## Next tasks (ask the owner which)

1. Roadmap 5: an unmodified Raspberry Pi OS Lite image boots to its login
   prompt (root file system on the SD card). Suggested as the next goal.
2. Performance phase 3, the JIT (`.claude/knowledge/performance.md`).
3. Networking to the host (a back end behind the LAN9514).
4. Display and USB keyboard/mouse.
5. I2C, SPI, PWM; Bluetooth; WiFi (SDIO chip).
6. A second board: the add-board skill.

## Open questions only a real board can answer

`lsusb -v -d 0424:ec00 | grep -E "bcdDevice|bDeviceProtocol"`,
`lsusb -v -d 0424:9514 | grep bmAttributes` (ours 0xC0; a LAN9512 shows
0xE0), `sudo ethtool -d eth0 | head -5` (ID_REV revision; ours 0).
