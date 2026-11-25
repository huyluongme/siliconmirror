# Specifications

Kept outside the repo (copyright, size): `~/specs/` (override with
`SMIRROR_SPECS`), each as the PDF and as text from `pdftotext -layout`.
**Grep the .txt files**; read only the lines the code needs and cite the
section number in the code comment. Download again only if a file is
missing, into that folder. Never commit them.

| File (~/specs) | What | Where to get it |
|---|---|---|
| armarm | Arm ARM for A-profile, DDI 0487 M.d | JSON at documentation-service.arm.com/documentation/ddi0487/latest, `_links.resources` has the PDF (developer.arm.com pages need JavaScript) |
| a53_trm | Cortex-A53 MPCore TRM r0p4, DDI 0500J (4.2: every system register with its reset value) | documentation-service.arm.com/documentation/ddi0500/latest |
| a53_fp_trm | Cortex-A53 FP and NEON TRM, DDI 0502G | Arm documentation service |
| pl011_trm | PL011 UART TRM r1p5, DDI 0183G | documentation-service.arm.com/static/5e8e36c2fd977155116a90b5 |
| bcm2835_peripherals | BCM2835 ARM Peripherals | datasheets.raspberrypi.com/bcm2835/bcm2835-peripherals.pdf |
| bcm2711_peripherals | BCM2711 ARM Peripherals (revised text; fixes the mini UART IER/IIR headings) | datasheets.raspberrypi.com/bcm2711/bcm2711-peripherals.pdf |
| bcm2836_local | BCM2836 ARM-local peripherals (0x40000000 block) | Raspberry Pi documentation |
| devicetree_v0.4 | Devicetree Specification v0.4 (ch. 5 = the FDT format) | devicetree.org |
| ecma-107, fat_wikipedia.wiki, mbr_wikipedia.wiki | FAT and MBR (Microsoft fatgen103 was NOT used: it needs an agreement) | ecma-international.org, Wikipedia raw wikitext |
| sd_physical_v9.10 | SD Physical Layer Simplified Spec 9.10 | sdcard.org (click-through: the owner downloads it) |
| usb20/, usb_20.txt | USB 2.0 (ch. 9 device framework, ch. 11 hubs) | usb.org |
| lan9514 | LAN9514 hub + Ethernet, DS00002306A | microchip.com |
| lan950x_ds.txt | LAN950x DS00001875D: the register map (ch. 7) | microchip.com (403 to scripts: the owner downloads it) |
| lan8700 | LAN8700 PHY, DS00002260A | microchip.com |
| intel_sdm | Intel 64 and IA-32 SDM, combined volumes 1-4 (2: instruction encodings; 3A 11.1.3: self-modifying code), for the JIT | cdrdv2.intel.com/v1/dl/getContent/671200 |
| sysv_amd64_abi | System V AMD64 ABI 1.0 (3.2: the calling convention), for the JIT | gitlab.com/x86-psABIs/x86-64-ABI, the build job's abi.pdf |
| rfc/ | RFC 768, 791, 792, 826, 1122, 2131, 2132, 4787, 5382, 5961, 6298, 9293 (the user network: IP, UDP, DHCP, TCP and NAT) | rfc-editor.org/rfc/rfcN.txt |

Encodings: check instruction encodings with GNU as and objdump
(`.claude/tools/enc.py`, `a64dis.py`), not from memory. The Arm ARM text
helpers are `acc.py` (System register access pseudocode) and `insn.py`
(an instruction's description).

Web material that cannot be fetched by scripts: elinux.org,
web.archive.org, uefi.org (Cloudflare), Microchip (403). Raspberry Pi
documentation pages, firmware wiki pages and downstream device tree
sources are cached in `~/.cache/siliconmirror-research/`.

Board facts and their sources are in `docs/boards/raspi3b.md`
(section "Sources").
