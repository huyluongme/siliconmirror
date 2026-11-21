---
name: finish-task
description: Finish a siliconmirror task the way the owner requires - all checks, a full mutation run on the changed lines, the boot log, the status file, a short Vietnamese report, and only after the owner's explicit approval the split commits, push and CI. Use when the code of a task is written and its tests pass.
---

# Finish a task

Follow the steps in order. Keep tool output small. Steps 1-6 happen
before the owner is asked anything; step 7 waits for an explicit yes.

## 1. Checks

`sh .claude/tools/check.sh` (format, layering, the five presets, gcc 13).
Everything must pass. Fix and re-run until it does. If gcc 13 warns, fix
the warning (CI fails on it), do not silence it.

## 2. Mutation run (`.claude/knowledge/mutation.md`)

- Write `scripts/mutation/<next name>.py` with the files of the task:
  new files `"all"`, changed files `"since:<HEAD before the task>"`.
- Run it in the background; do not build while it runs. Read the
  survivors when it ends.
- Classify: real gaps get tests, dead code goes, equivalents get a
  reason. Re-run (survivors, or a fresh run after big changes) until
  only equivalents are left. Re-run step 1 after the new tests.

## 3. Docs

- `docs/boards/<board>.md`: the behaviour, the choices where the docs
  are silent, a bullet in "Differences from the real board" for what is
  not modelled, and the sources in "Sources" (with commit or tag).
- `docs/architecture.md` when a module or a test kind is new;
  `README.md` when the roadmap or a user-visible option changes.

## 4. Boot log

`sh scripts/boot-log.sh` on the plain build. Compare with the previous
log (`git diff --stat docs/boot-logs/`): how far it gets, what changed.

## 5. Status

Update `.claude/status.md`: HEAD, the task done, the next tasks. Add a
line to `.claude/knowledge/history.md` if a stage ended, and new verified
facts to the knowledge file they belong to (only facts that are not in
the code or docs).

## 6. Report (Vietnamese, short, plain words)

What works now, how it was checked (presets, sessions, mutation totals
and the classes of the survivors), the choices made where docs were
silent, what could not be done and why, then the proposed commits (one
line each: subject and files), and "Nguồn tham khảo" (sources actually
read). Then stop and wait.

## 7. After the owner says yes ("đồng ý", "commit và push đi")

1. Stage each commit's files explicitly (never `git add .`); a file
   shared by two commits gets each intermediate version written out by
   hand. Check `git diff --cached --stat`.
2. `git commit -s` with subject `area: summary` (<= 72 chars), a body
   that says what and why (wrapped at 72), and last line
   `Co-Authored-By: Claude <model in use> <noreply@anthropic.com>`.
   The mutation config is its own commit: `scripts/mutation: <topic>`.
3. Check that every intermediate commit builds and passes:
   ```sh
   W=<scratchpad>/wt; git worktree add -q --detach $W <first commit>
   for c in <commits but the last>; do git -C $W checkout -q $c
     rm -rf $W/build; cmake -S $W -B $W/build -G Ninja \
       -DCMAKE_BUILD_TYPE=Debug -DSMIRROR_SANITIZE=ON >/dev/null
     cmake --build $W/build >/dev/null && ctest --test-dir $W/build -j6 | grep 'tests passed'
   done; git worktree remove --force $W
   ```
4. `git push origin master`, then in the background
   `python3 .claude/tools/ci_wait.py <sha>`. When it ends, report the
   result. If a job fails, find out why (gcc13_check.py for gcc jobs;
   an apt-get failure of the runner is not ours) and propose a fix.
5. Give the list of next tasks (from `.claude/status.md`).
