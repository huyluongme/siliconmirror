# Performance plan

Agreed 2026-11-14; each step is its own commit after approval. Measure
before and after (numbers in the commit message). Never trade exactness
for speed: the interpreter is the specification. Unmodelled behaviour
still stops the machine.

## Done

- Phase 0: `scripts/bench.sh` (200M-instruction loop; U-Boot to the
  prompt).
- Phase 1: page caches for fetch and data accesses
  (`Arm64Page pages[3][64]`, emptied by `arm64_tlb_flush`), division-free
  counter reads (`util/muldiv.h`). U-Boot to the prompt 73.6 s -> 34.4 s.
- Phase 2: `-clock host` (`backend/clock/host`, `machine_follow_host`):
  U-Boot 2.9 s. The default stays virtual (deterministic tests).

Baseline then: about 70-110 M instructions/s; QEMU about 1 G/s on the
same loop.

Session times on the owner's machine (release, 2026-11-23): U-Boot
209 s, Linux 118 s, Pi OS 1296 s alone before the DWC2's `SOF`; with
it, and the sessions run 4 at a time, 233 s, 135 s and 1453 s. `SOF`
interrupts the guest every 125 us of virtual time while Linux keeps a
periodic transfer queued (always, with the hub's and the LAN9514's
interrupt endpoints), as on the board.

## Phase 3.0: the profile (2026-11-25, at e0ac2bb)

Host side: gprof (`build/prof`, `-pg`; perf is blocked by
`perf_event_paranoid` 4; give each process its own file with
`GMON_OUT_PREFIX`, or drive's gmon.out overwrites smirror's). Guest
side: a throwaway patch in `step()` that samples (EL, PC, word) every
1024 instructions (bound the hash probe: Pi OS has over 1 M distinct
sampled PCs), U-Boot's PCs mapped through its System.map (relocation
found by matching words), the kernel's read by disassembly.

- Host time in `cpu/arm64`: 88 % (U-Boot + Linux), 92 % (Pi OS); the
  device models 3 % and 0.5 %. The JIT is the right next step.
- U-Boot + Linux session: about 9.1 G guest instructions. 67 % in U-Boot
  at EL2, nearly all waiting: `get_ticks` + `__udelay` 43 % (the 2 s
  autoboot countdown, delays), `bcm2835_send_cmd` 14 % (SD polling).
  Kernel: the 8250 mini UART's `wait_for_xmitr` (LSR & 0x60) 14 %, a
  byte loop testing `\`, `"`, digits and identifiers 13 % (likely the
  trace events' print_fmt update at boot; an inference). 580 M counter
  reads, each through about ten functions (`exec_sysreg` ->
  `access_sysreg` -> `arm64_timer_trap` ... `machine_now`).
- Pi OS session: about 58 G guest instructions, 67 % at EL0, 33 % at
  EL1; about 45 M instructions/s. Flat: the top 1000 PCs hold 15 % of
  EL0 and 20 % of EL1. Mix at both ELs: branches 20 %, loads 19 %,
  stores 9 %, moves and adds 19 %, compares 6-7 %, SIMD/FP under 2 %.
  The same kernel string loop 5 %.
- What it means for the JIT: integer, load/store and branch translation
  (3b, 3c) covers nearly all of it; SIMD can stay with the interpreter.
  The code is large and flat, so translation must be cheap and the
  cache large, and invalidation when pages are reused (exec, exit) must
  be right and fast; maybe translate a block only after it runs N
  times. `ret`/`br` lookups matter (branches 20 %). The busy waits are
  the board's own behaviour and stay: virtual time must pass; a JIT
  makes them cheaper, and a fast path for `MRS CNTPCT/CNTVCT` (also in
  the interpreter) shortens U-Boot's waits.

## Phase 3: JIT (weeks; the owner decides when)

Own design, x86-64 hosts first; the interpreter stays as the reference
and for other hosts.

- Translate A64 basic blocks into x86-64 code in an mmap'd buffer;
  block cache keyed by (PA, EL, regime); a dispatcher, then chaining.
- System instructions, exceptions and rare cases call the interpreter.
- Invalidate on writes to translated pages (dirty tracking in the bus)
  and on TLB flushes.
- Correctness: a differential test mode runs interpreter and JIT on the
  same code and compares states per block; all unit tests in both modes;
  U-Boot and Linux boot.
- Milestones, measured after each: 3a infrastructure ("call the
  interpreter" translation), 3b integer ALU and branches, 3c loads and
  stores with a TLB fast path, 3d lazy NZCV, 3e chaining. Target
  >= 500 M instructions/s.
- Profile first: in Linux's boot, find what is hot (gprof on the release
  build).
