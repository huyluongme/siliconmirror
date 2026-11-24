# Status

Update this file at the end of every task, in the same commit as the
work (or the next one), so a new session starts from here.

- **Date:** 2026-11-24
- **master:** 332ec07 (networking part 2a closed), then this task's
  commits.
- **Board:** raspi3b. Roadmap items 1-5 done: from power on, the
  firmware HLE, U-Boot, Linux to the BusyBox shell
  (`docs/boot-logs/raspi3b-uboot-linux.log`), and an unmodified
  Raspberry Pi OS Lite to its login prompt
  (`docs/boot-logs/raspi3b-raspios.log`, `tests/raspios/`).
  Networking to the host is done: `-net tap`, `-net pcap`, `-net user`
  with TCP and `hostfwd`.
- **Last task:** networking to the host, parts 2b and 2c: TCP in the
  user network (backend/net/user_tcp.c): the router ends the guest's
  TCP itself (RFC 9293) and joins each connection to a nonblocking host
  socket; the guest's SYN waits for the host's connect() (then SYN-ACK,
  RST when refused, ICMP host unreachable otherwise); MSS 1460, no
  window scaling, SACK or timestamps; silly window avoidance; RFC 5961
  challenge ACKs; RTO 1 s doubled to 60 s, started again at each new
  ACK, go back N, zero-window probes; the end after 8 silent timeouts;
  TIME-WAIT 60 s; 64 connections, a full table evicts the TIME-WAIT
  that ends first; every RST with no data and a window of 0.
  `hostfwd=tcp:[hostaddr]:hostport-[guestaddr]:guestport` (QEMU's
  documented syntax; an empty hostaddr is 127.0.0.1, our choice):
  connections from 10.0.2.2, ports from 49152, an ARP request first
  when the guest has not spoken. A board fix it brought out:
  `MACHINE_MAX_DEVICES` 32 was one short for `-audio` with `-net`
  (now 64, test_every_option). Sessions: `linux.user` adds nc both
  ways through two forwards (2 MB from the guest); `raspios.session`
  gets 10.0.2.15 by DHCP and logs in to its own sshd through
  `hostfwd=tcp::10022-:22`. Mutation linux29: 1344 mutants that build,
  205 survivors at first; after new tests and simpler code, a fresh run of
  1304: 67 survivors; 61 after more tests, all equivalent (parameter
  array sizes, fd -2 for -1, `>=` where equality gives the same, loops
  one slot past the array into the zeroed datagram buffer, Linux's
  nonblocking connect() never returning 0, delays only (poll's
  timeout), initial values written again, the 53 that `linux.user`
  checks, a SYN's length in a synchronized state (both ways an ACK),
  option bytes read past the list that the length check stops, the
  ARP request's target address already zero).

## Next tasks

1. **Next: performance phase 3, the JIT**
   (`.claude/knowledge/performance.md`): the Pi OS session takes about
   25 minutes. `/effort xhigh` for its design.
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
