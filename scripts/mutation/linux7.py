# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Huy Luong Duc

# The registers of the DWC2's dedicated FIFOs and DCFG that the Raspberry
# Pi's dwc_otg driver reads in host mode, in the working tree: the lines
# changed since 0121e05.
SNAPSHOT = None
FILES = [{"path": "src/hw/usb/dwc2.c", "lines": "since:0121e05"}]
WORKERS = 4
JOBS = 3
