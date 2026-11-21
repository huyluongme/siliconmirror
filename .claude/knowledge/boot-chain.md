# The Raspberry Pi 3B boot chain in smirror

Facts that were checked, and where the pinned software comes from.

- Firmware files: raspberrypi/firmware tag **1.20260915** (sha256 pinned
  in the card scripts). Its `boot/kernel8.img` is Linux at raspberrypi/linux
  **cff533aec2fa601846766b32ff57204e0a61bed7** (`extra/git_hash`), with
  `extra/System8.map` for symbols. Linux sources of the drivers studied
  are cached in `~/.cache/siliconmirror-linux/src`.
- U-Boot **v2026.07** `rpi_3_defconfig`, built from source by
  `scripts/uboot-card.sh` (cache `~/.cache/siliconmirror-uboot`).
- Initramfs: BusyBox **1.38.0** static, `/init` mounts proc/sys/devtmpfs
  and runs a shell (`scripts/linux-card.sh`).
- The chain of the boot log: firmware HLE (`machine/raspi3b_firmware`:
  config.txt, overlays, DTB fix-ups, kernel placement) -> U-Boot
  (`kernel=u-boot.bin`) -> U-Boot loads kernel8.img + initramfs ->
  Linux to the BusyBox shell. `tests/uboot/` and `tests/linux/` are
  expect sessions (release build only, local; they skip with exit 77
  without their card).
- Where the Pi 3 differs from what one expects: the console is the mini
  UART (serial0 = uart1, GPIO 14/15 ALT5; the PL011 goes to Bluetooth);
  the SD card is on SDHOST (0x7E202000) while the Arasan EMMC goes to
  the WiFi chip (not modelled: SDIO commands time out); arm_64bit=0 and
  enable_uart=0 by default; the DTB goes just below 0x2f000000.
- Exploration of a new stop: `sh .claude/tools/linux_explore.sh
  ~/.cache/siliconmirror-linux/cmdline-explore.txt
  ~/.cache/siliconmirror-linux/runNN` (cmdline-nosmp.txt for one CPU).
  The patch makes unmodelled registers read as zero with a log line, so
  one run lists several stops. About 6 minutes a run.
- Kernel time to /init: about 19 s with `-clock host` on one core; the
  Linux session test takes about 110 s.
