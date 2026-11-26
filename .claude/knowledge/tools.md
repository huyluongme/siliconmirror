# Tools

Run them from the repo root. Scripts in `scripts/` are part of the
project; those in `.claude/tools/` are helpers for the work.

| Command | What it does |
|---|---|
| `sh .claude/tools/check.sh [PRESET...]` | Format, layering, build + test each preset, gcc 13 check; one line each, failures in full. Default: all five presets. |
| `python3 .claude/tools/gcc13_check.py [BUILD] [SUBSTR...]` | Compiles with gcc 13's cc1 (CI's compiler) and the flags of BUILD/compile_commands.json (default build/ci-release-gcc). Needs `sh .claude/tools/setup-gcc13.sh` once. |
| `python3 .claude/tools/ci_wait.py SHA` | Waits for the GitHub Actions run of SHA (public API, no auth; polls once a minute, sleeps on rate limits) and prints each job. Run it in the background after a push. Job logs need auth: if a gcc job fails, use gcc13_check.py. |
| `python3 .claude/tools/x64_bytes_check.py [FILE]` | Assembles the Intel-syntax comment after each `CASE(...)` of tests/unit/test_jit_x64.c with GNU as and compares the bytes with the test's. Run it after adding an encoding to the JIT's emitter. |
| CI's headers | CI runs Ubuntu 24.04: glibc 2.39, kernel headers 6.8, older than this machine's (glibc 2.43 includes `<linux/sockios.h>` from `<sys/ioctl.h>`; 2.39 does not). With a new system include, check it: `dpkg-deb -x` the noble `libc6-dev` and `linux-libc-dev` debs (archive.ubuntu.com/ubuntu/pool/main/g/glibc, .../l/linux) into the scratchpad, then `gcc -std=c11 -fsyntax-only -Werror -nostdinc -isystem $(gcc -print-file-name=include) -isystem ROOT/usr/include/x86_64-linux-gnu -isystem ROOT/usr/include -Isrc -Itests/unit FILE`. |
| `sh scripts/boot-log.sh` | Boots firmware HLE, U-Boot, then Linux from the card and writes `docs/boot-logs/raspi3b-uboot-linux.log`; then Raspberry Pi OS to its login prompt, `docs/boot-logs/raspi3b-raspios.log` (about 27 min in all). Deterministic. |
| `sh scripts/uboot-card.sh`, `sh scripts/linux-card.sh`, `sh scripts/uboot-linux-card.sh`, `sh scripts/raspios-card.sh` | Build the U-Boot card, the Linux card, the U-Boot-loads-Linux card, and the Raspberry Pi OS card (pinned versions and sha256, cached in `~/.cache/siliconmirror-uboot`, `~/.cache/siliconmirror-linux`, `~/.cache/siliconmirror-raspios`). |
| `sh scripts/bench.sh [BUILD]` | Speed: a 200M-instruction loop, and U-Boot to its prompt with both clocks. |
| `sh scripts/fuzz.sh` | libFuzzer + ASan + UBSan on the parsers of what an SD card holds (clang). |
| `python3 scripts/mutation/mutate.py CONFIG LOG [OLD_LOG]` | Mutation testing (see mutation.md). |
| `python3 .claude/tools/sym.py System8.map ADDR...` | Kernel symbol + offset for a VA. The firmware 1.20260915 kernel8.img loads at PA 0x200000, `_text` = 0xffffffc080000000. |
| `sh .claude/tools/raspios_explore.sh OUT_PREFIX` | The same hacks on a debug copy of the Raspberry Pi OS card (enable_uart=1, console on serial only), virtual clock; `T=` limits the run. |
| `sh .claude/tools/linux_explore.sh CMDLINE_FILE OUT_PREFIX` | Applies the local exploration patch (`~/.cache/siliconmirror-linux/explore.patch`), builds, boots the Linux card, reverts. Never commit the patch. `SMIRROR_EXPLORE_LIMIT=N` stops after N instructions and dumps every CPU. |
| `python3 .claude/tools/acc.py 'REG:MRS' 'REG:MSR'` | The access pseudocode of a System register from the Arm ARM text. |
| `python3 .claude/tools/insn.py C7.2.230` | An instruction's description from the Arm ARM text. |
| `python3 .claude/tools/enc.py 'insn' ...` | A64 encodings from GNU as, as C lines. `a64dis.py 0xWORD...` disassembles words. |
| `python3 .claude/tools/simd_ref.py CASES.py` | Independent Python model of integer Advanced SIMD, prints test table rows. |
| `python3 .claude/tools/fp_ref.py ...`, `fp_sweep.py` | Exact rational reference for floating point; sweep of FP encodings classified by binutils. |

How to run things by hand:

- U-Boot: `./build/release/smirror -M raspi3b -sd ~/.cache/siliconmirror-uboot/card.img`
  (press a key during the countdown; type slowly, the mini UART's FIFO
  holds 8 bytes, as on the real board).
- Tests read stdin from `/dev/null`; a ctest run from a tool whose stdin
  is an open pipe used to hang.

Waiting for a background job: wait on something that cannot match the
waiter itself (a log's `DONE` line, `tail --pid=PID -f /dev/null`);
`pgrep -f 'mutate.py ...'` matches its own shell and never ends. Notice
the end and carry on without being asked.
