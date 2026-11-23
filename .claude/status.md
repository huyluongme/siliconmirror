# Status

Update this file at the end of every task, in the same commit as the
work (or the next one), so a new session starts from here.

- **Date:** 2026-11-23
- **master:** d1f18d2 (networking part 1 closed), then this task's
  commits.
- **Board:** raspi3b. Roadmap items 1-5 done: from power on, the
  firmware HLE, U-Boot, Linux to the BusyBox shell
  (`docs/boot-logs/raspi3b-uboot-linux.log`), and an unmodified
  Raspberry Pi OS Lite to its login prompt
  (`docs/boot-logs/raspi3b-raspios.log`, `tests/raspios/`).
- **Last task:** networking to the host, part 2a: `-net user`, a router
  inside smirror (backend/net/user.c: Ethernet, ARP, IPv4, ICMP echo and
  time exceeded, fragments out; user_dhcp.c: 10.0.2.15 for any client;
  user_udp.c: NAT through connected host sockets, 10.0.2.3:53 to the
  host's resolv.conf server, 10.0.2.2 to the host's 127.0.0.1, 5 min
  idle, 64 mappings LRU). `receive()` of core/net.h now takes the
  virtual time. A DWC2 fix it brought out: a NAKed bulk or control IN
  no longer halts the channel, the core tries again each microframe
  (Linux's dwc2_hc_nak_intr: "The core will continue transferring
  data"); before, no frame from the cable ever reached Linux (part 1's
  session only checked what Linux sent). Session `linux.user`: udhcpc
  gets the lease, ping 10.0.2.2, nslookup localhost through 10.0.2.3.
  Found, not fixed: typing on the console while the kernel prints a
  line hangs the guest (with or without a cable); user.txt waits for
  the line's end. Mutation linux28: 1348 mutants, 213
  survivors at first; after new tests and simpler code, a fresh run of
  1295: 86 survivors, all equivalent (parameter array sizes, buffer
  sizes, fd 0 is stdin, copies overwritten after, ties in the LRU,
  delays only: the queue's threshold, ECONNREFUSED's retry, poll's
  timeout; fd leaks a test cannot see, OOM, the default port 53 that
  `linux.user` checks, connect to 0.0.0.0 being the host on Linux, an
  IP header under 20 bytes making the destination the data).

## Next tasks

1. **Next: the rest of networking, 2b and 2c together, as one task in
   one session** (the owner's call, 2026-11-24: not step by step, to
   reach the JIT sooner, as each step's tests take long at today's
   speed; one finish-task run, one mutation run, one report):
   - 2b, TCP NAT out in user_tcp.c: a small TCP state machine at the
     router's side (handshake, window, retransmission in virtual time,
     close) joined to a host TCP socket; unit tests against a TCP
     server on the host's loopback. `/effort xhigh` for its design.
   - 2c, `hostfwd=tcp:HOSTPORT-:GUESTPORT` (the option parser in
     net_user_open) and a Pi OS session that gets its address by DHCP
     and answers ssh.
2. **Then at once: performance phase 3, the JIT**
   (`.claude/knowledge/performance.md`): the Pi OS session takes about
   25 minutes.
3. The console hang: typing while the kernel prints a line (see
   tests/linux/user.txt); find which side (PL011 or the guest) loops.
4. Display (HVS channels, pixelvalves, HDMI) and USB keyboard/mouse.
5. I2C, SPI, PWM devices; Bluetooth; WiFi (SDIO chip).
6. A second board: the add-board skill.
7. Last, nice to have: MMAL streaming (VCHIQ bulk transfers, port
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
