# Mutation testing

"xài mutation test đầy đủ đi, đừng làm nhanh, tôi không muốn có bất kì sai
sót nào": every task gets a full mutation run on the lines it changed.

## How

1. Write `scripts/mutation/<name>.py` (next number after the latest
   `linuxNN.py`):
   ```python
   SNAPSHOT = None            # the working tree
   FILES = [
       {"path": "src/hw/net/lan95xx.c", "lines": "since:<last commit>"},
       {"path": "src/boot/new_file.c", "lines": "all"},
   ]
   WORKERS = 4
   JOBS = 3
   ```
2. `python3 scripts/mutation/mutate.py scripts/mutation/<name>.py ~/.cache/siliconmirror-mutation/<name>.log`
   in the background (`--count` first to see how many). It works on
   copies in `~/.cache/siliconmirror-mutation/w*`, never on the tree;
   the same command resumes after a stop. About 1-1.5 hours per 1500.
3. Never build or run the tests while it runs (same CPU, time-outs),
   and never run two at once (same work directories). Editing the tree
   is harmless: the copies were taken at the start. Log line numbers are
   those of that snapshot.
4. Classify each survivor: a real gap (write the test), equivalent (say
   why), or code that can go (remove it). Then run the survivors again
   with a new log: `mutate.py CONFIG NEW.log OLD.log`. After big code
   changes, a fresh full run is better (new lines are not in OLD.log).
   A fresh run needs a new LOG name: renaming the old LOG leaves its
   `LOG.results`, and the run under the old name resumes from it,
   mixing both versions of the code (seen in linux28).
5. Commit the config as its own commit, `scripts/mutation: <topic>`.
   Report only totals and the classes of survivors.

## Tests that stay bounded

A mutant can make a loop endless. A test must then end, not flood: a
fake that CHECKs each call printed without end and ctest kept 13 GB of
output in RAM (the OOM killer took VS Code); a WAV test's back end made
2.6 million files. Fakes print once and `abort()` on a call out of
contract; tests that make files or memory grow are bounded (`alarm()`,
capped sizes). After an OOM, clean `w*/build` and run again from the
start: under memory pressure tests fail for other reasons, and mutants
pass for killed. 3 workers on the owner's 14 GB machine.

## Equivalent classes seen

Buffer sizes and limit constants off by one; out-of-memory paths;
`va_end` and `PRINTF_FORMAT` mutants; `>=` against `>` where equality
cannot happen or gives the same result; initial values always
overwritten; `break` at the end of the last case; `free` deletions
(leaks only); sanity checks that callers make impossible; values after
a stop; uninitialised stack reads that happen to be zero (a one-off
MSan build can confirm). Host I/O: `EAGAIN` and `EWOULDBLOCK` are the
same value on Linux; a regular file writes partly only at its limit,
after which the next write fails, so the pointer's advance is never
seen; `write()` returns 0 only for 0 bytes; code under `#else` of
another host. A board's leaks in its failure paths: `main()` still holds
the pointer in `BoardOptions`, so LeakSanitizer sees it reachable.
What only root can reach (a real TAP device's flags, a read-only open)
is checked by hand, not in CI.

## Gaps it found that tests had missed

A test that skips when the code under test fails in a way that
looks like the host's (net_tap test_tun took a mutant's success for
"allowed to make devices"): decide a skip with the test's own probe,
never with the code under test. Descriptors 0 (`close(0)` first) and
fd leaks (`fcntl(fd, F_GETFD)` after destroy). Error paths of malformed
input (broken overrides, fixups); boundaries
(offset 0 vs non-zero, single-character names, the first word of a
list); messages of warnings never checked; a dtparam dropped after a
failed overlay. Mutation testing does not prove a model right: a
misread datasheet passes it. Real software (U-Boot, Linux) is the
strongest check.

## Stopping a run

SIGINT does not stop it. `kill -9` the python3 process, then
`pkill -9 -f siliconmirror-mutation` as the last command of a Bash call
(it also kills that shell); check in a new call with
`ps aux | grep -E "mutate.py|siliconmirror-mutation/w[0-9]" | grep -v grep`.
