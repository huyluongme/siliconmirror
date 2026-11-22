# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Huy Luong Duc

# Networking to the host, part 1, in the working tree: the frames
# between the host's form and the cable's, the TAP and pcap links, the
# cable, and the lines changed since ff40122 of the DWC2 (SOF), the
# LAN9514 (the lite reset, the link's speed), the board and the front
# end (-net).
SNAPSHOT = None
FILES = [
    {"path": "src/core/net.c", "lines": "all"},
    {"path": "src/backend/net/tap.c", "lines": "all"},
    {"path": "src/backend/net/pcap.c", "lines": "all"},
    {"path": "src/machine/net_cable.c", "lines": "all"},
    {"path": "src/hw/usb/dwc2.c", "lines": "since:ff40122"},
    {"path": "src/hw/net/lan95xx.c", "lines": "since:ff40122"},
    {"path": "src/machine/raspi3b.c", "lines": "since:ff40122"},
    {"path": "src/frontend/main.c", "lines": "since:ff40122"},
]
WORKERS = 3
JOBS = 3
