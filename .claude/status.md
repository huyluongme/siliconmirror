# Status

Update this file at the end of every task, in the same commit as the
work (or the next one), so a new session starts from here.

- **Date:** 2026-11-23
- **master:** ff40122 (task 1 closed, networking next), then this
  task's commits.
- **Board:** raspi3b. Roadmap items 1-5 done: from power on, the
  firmware HLE, U-Boot, Linux to the BusyBox shell
  (`docs/boot-logs/raspi3b-uboot-linux.log`), and an unmodified
  Raspberry Pi OS Lite to its login prompt
  (`docs/boot-logs/raspi3b-raspios.log`, `tests/raspios/`).
- **Last task:** networking to the host, part 1 of 2: the network link
  interface (core/net.h, frames without FCS; net_to_wire/net_from_wire),
  the cable (machine/net_cable: the partner's MAC, frames paced at the
  link's speed), `-net tap:IFNAME` and `-net pcap:PATH`
  (backend/net). Two fixes that a cable brought out: the DWC2 now
  raises `SOF` every microframe (Linux's dwc2 waits for it after an
  interrupt endpoint's NAK, so the LAN9514's link interrupt never came),
  and the LAN9514's lite reset leaves the PHY as it is (Linux 6.18 sets
  the PHY's interrupt mask at bind, lite-resets at open; our reading,
  docs/boards/raspi3b.md). Session `linux.net`: link up at 100 Mb/s
  full, ping, `net_check` finds the three ARP requests in the capture.
  SOF costs about 10% on the sessions (Pi OS session 21.6 -> 24.2 min,
  the latter with other sessions running). TAP itself is checked over a
  socket pair in unit tests; a real TAP needs root once
  (`ip tuntap add ... user $USER`), not in CI. Mutation linux27: 398
  mutants, 84 survivors at first; after the new tests 44, all
  equivalent (sizes, OOM, the board's leaks in failure paths,
  `EAGAIN == EWOULDBLOCK`, partial writes of regular files, non-Linux
  code, what only root reaches with a real TAP; the board's
  `lan95xx_plug` is checked by `linux.net`).

## Next tasks

1. **Next: networking part 2, `-net user`**: a user-mode network in
   smirror, no root (the idea of QEMU's `-netdev user`: credit the idea,
   never read its code): ARP and a gateway at 10.0.2.2, DHCP giving
   10.0.2.15, DNS forwarded to the host's resolver, ICMP echo to the
   gateway, UDP and TCP NAT through host sockets, and `hostfwd=tcp:
   HOSTPORT-:GUESTPORT` so that `ssh -p 2222 localhost` reaches Pi OS.
   Its own back end in `src/backend/net/user*.c` behind core/net.h. A
   Pi OS session that gets an address by DHCP and answers ssh would
   check it end to end (CI can run it: no root).
2. Performance phase 3, the JIT (`.claude/knowledge/performance.md`):
   the Pi OS session takes about 24 minutes.
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
`sudo ethtool eth0` with a cable, and `cat /sys/class/net/eth0/carrier`
right after `ip link set eth0 down; ip link set eth0 up` (whether the
lite reset at open drops the link: ours keeps it).
`amixer -c Headphones contents` after boot (the controls' starting
values; ours follow Linux: switch off, 0 dB, auto).
On Raspberry Pi OS with no display: the HVS's `DISPCTRL` and `DISPSTAT`
as the firmware leaves them (`sudo busybox devmem 0x3f400000` with vc4
blacklisted; ours read 0), and which VCHIQ services the firmware has
(`sudo vcgencmd version` works, so `GCMD` at least).
