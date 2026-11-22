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
