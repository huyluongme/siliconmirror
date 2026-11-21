# Decisions and conventions

The owner's decisions, with the reason where one was given. Most are not
written anywhere else in the repo.

## What the project is

- A **full-boot** emulator: power on, the board's own boot chain
  (firmware or boot ROM stage, then the bootloader, then the kernel),
  exactly as on the real board. The owner works on network switch
  devices, which always boot that way, and wants an emulator for that.
  Booting a kernel directly (`-kernel`) exists for development and
  tests only.
- Written **from scratch in C11**, not a QEMU fork; open source, mainly
  for personal use. **Generic like QEMU**: core / cpu / hw / boot /
  machine / frontend layers, so more CPUs and boards can be added.
- First board: Raspberry Pi 3 Model B, AArch64 only. The VideoCore
  firmware cannot run (it is GPU code), so its boot is done in C (high
  level emulation): it reads the SD image (MBR, FAT, `config.txt`,
  kernel, device tree, overlays) and must boot unmodified U-Boot and
  kernels, and the owner's own builds.
- Names in **English only** (the owner rejected Vietnamese names).
  Executable `smirror`; options in **QEMU style** (`-M raspi3b -sd card.img
  -serial stdio`), credited to QEMU in the README. Never copy QEMU's
  option code or help text; never use QEMU's name or logo as ours.

## Originality

- "tôi hy vọng là những cái đang làm là idea tự nghĩ ra chứ ko phải lấy
  source của qemu": write every line from specs, datasheets and our own
  design. Never read QEMU's source.
- When a design, interface or name follows another project, say so when
  presenting the step and credit it in the docs. Ask before porting any
  outside code. Where we match another project instruction for
  instruction (the boot stub follows armstub8, BSD-3-Clause), credit it
  and do not call it "written from scratch".
- Reading Linux or U-Boot drivers to learn **what the guest does** is
  allowed; say so in the docs and commit message.

## Behaviour of the models

- "cứ làm sao giống board thật nhất là được": as close to the real board
  as possible, including its quirks (an 8-byte UART FIFO loses pasted
  bytes, as on the real board).
- Behaviour that is not modelled **stops the machine** with a message
  that names the device, offset and access. No silent RAZ/WI.
- Where documentation is silent: search the web first; otherwise choose,
  write the choice in the code comment and in `docs/boards/<board>.md`.
- Time is virtual by default (1 instruction = 1 ns, deterministic, what
  the tests need); `-clock host` follows host time.

## Build and code

- CMake + Ninja + CTest behind a thin **Makefile**, which is the owner's
  interface (`make`, `make test`, `make run ARGS=`, `make format`,
  `make check-format`, `PRESET=`, `CC=`). The owner knows Make, not
  CMake: explain every CMake line you add.
- License GPL-2.0-or-later: LICENSE stays the verbatim GPLv2 text; every
  source file starts with `SPDX-License-Identifier: GPL-2.0-or-later`
  and the copyright line of the other files.
- Indent with **tabs, width 4** (.editorconfig; YAML and Markdown use
  spaces). clang-format 21 decides the rest.
- CI: GitHub Actions on Ubuntu 24.04, gcc 13 and clang, `ci-release`
  at -O3 -Werror. The local gcc is newer and misses some gcc 13
  warnings, so run `.claude/tools/gcc13_check.py` before pushing.

## Commits

- "luôn luôn đợi tôi kiểm tra, khi nào tôi đồng ý commit và push thì mới
  được làm": stop after each task, report, and wait. Commit and push only
  after an explicit yes ("đồng ý", "commit và push đi"). A plain "ok" did
  not satisfy the auto-mode classifier once; ask again if unsure.
- `git commit -s` (DCO, see CONTRIBUTING.md) and the last trailer line
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>` (use the
  model actually in use). The owner keeps it so AI-written code can be
  traced.
- Subject `area: summary` (area = path such as `hw/net`,
  `machine/raspi3b_firmware`), at most 72 characters; body wrapped at
  72. Split a task into commits that each build and pass the tests
  (check with a worktree, see the finish-task skill).
- A file shared by two commits: write each intermediate version of it
  explicitly and check `git diff --cached`. Never stage hunks with
  `git apply --cached --unidiff-zero` + stash: that once pushed a
  duplicated docs bullet.
- Every task ends with a boot log taken and committed
  (`sh scripts/boot-log.sh` writes `docs/boot-logs/`), so its history
  shows how far the boot chain gets.
- `gh` is not installed: CI is checked through the public API with
  `.claude/tools/ci_wait.py SHA`.
