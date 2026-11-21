# siliconmirror: instructions for Claude

siliconmirror (`smirror`) is a from-scratch emulator in C11 whose goal is
the **full boot** of a real board: its firmware or boot ROM stage, the
bootloader, then the kernel, as on the hardware. The owner works on
network switch devices, which always boot that way. The first board is
the Raspberry Pi 3 Model B (AArch64). Read `README.md` and
`docs/architecture.md` for the design, and `docs/boards/<board>.md` for
a board.

Where the work stands, and what comes next:

@.claude/status.md

## The owner's standing rules

- "luôn luôn đợi tôi kiểm tra, khi nào tôi đồng ý commit và push thì mới
  được làm": after each task, report and wait; commit and push only on
  an explicit yes.
- "tôi hy vọng là những cái đang làm là idea tự nghĩ ra chứ ko phải lấy
  source của qemu": never read or copy QEMU's source; write from specs;
  credit any design that follows another project.
- "cứ làm sao giống board thật nhất là được": as close to the real board
  as possible. Unmodelled behaviour stops the machine with a message.
  Where the docs are silent, search the web first, then choose and
  document the choice (code comment and the board page).
- "sau mỗi lần xong task và trước khi commit, hãy lấy boot log của toàn
  bộ từ uboot tới kernel xuất ra một file log để tracking":
  `sh scripts/boot-log.sh`, committed with the task.
- Full mutation testing for every task (`.claude/knowledge/mutation.md`).

## Talking to the owner

- Reply in **Vietnamese**, short and plain: the owner wants to understand
  without being an expert. End reports with a **"Nguồn tham khảo"**
  section listing the sources actually read.
- Check facts against specs, datasheets and official docs; say plainly
  which claims come from general knowledge.
- The owner watches the usage limit: keep tool output small (narrow
  greps, `head`, failing lines only), never dump whole files or long spec
  passages, keep reports short, put details in commit messages and docs.
  Suggest a new session at a task boundary: this file and `.claude/`
  carry the context.
- Keep the owner's email address and other personal details out of
  files, commits and requests to outside services.
- When a background job (mutation, CI) ends, carry on with the next step
  without being asked. After each push, give the list of next tasks.

## How a task goes

1. Agree the task and its split with the owner; read only the spec
   sections it needs (`.claude/knowledge/specs.md`).
2. Write the model with its unit tests (`tests/unit/test_<module>.c`),
   bare-metal or session tests where real software exercises it, and the
   board page. Keep the layering of `docs/architecture.md`
   (`scripts/check-layering.sh`).
3. Finish with the **finish-task** skill (`/finish-task`): checks,
   mutation run, boot log, status update, report, then, after the
   owner's yes, split commits, push and CI.

For a new board, use the **add-board** skill (`/add-board <name>`).

## Code and commits (details in `.claude/knowledge/decisions.md`)

- C11, tabs of width 4, clang-format; every file starts with the SPDX
  line `GPL-2.0-or-later` and the copyright line.
- `git commit -s`, last line `Co-Authored-By: Claude <model> <noreply@anthropic.com>`
  with the model in use; subject `area: summary` at most 72 characters.
- Presets: debug, asan, ci-clang, ci-release-gcc (CI's -O3 -Werror),
  release (runs the U-Boot and Linux sessions). CI uses gcc 13: run
  `.claude/tools/gcc13_check.py`. All of it: `sh .claude/tools/check.sh`.

## Where to look

| File | Read it when |
|---|---|
| `.claude/knowledge/decisions.md` | before changing build, CLI, licence or commit habits |
| `.claude/knowledge/history.md` | planning a board or a stage: what the Pi 3B needed, in order |
| `.claude/knowledge/boot-chain.md` | working on firmware, U-Boot or Linux on the Pi 3B |
| `.claude/knowledge/specs.md` | looking up a register or instruction (specs in `~/specs`) |
| `.claude/knowledge/tools.md` | running checks, sessions, CI, exploration |
| `.claude/knowledge/mutation.md` | the mutation run of a task |
| `.claude/knowledge/performance.md` | speed work, the JIT |

## Model and effort

The project's default is in `.claude/settings.json`: Opus at effort
`high`. It is low-level, spec-exact work where a wrong bit costs hours
of debugging, so a strong model at high effort pays off; `xhigh` and
`max` cost more for little gain on most steps.

- `/effort xhigh` for the hard parts only: a boot that hangs without a
  clear cause, the design of a new CPU or a JIT.
- `/effort medium` for mechanical work: classifying mutation survivors,
  boot logs, docs, commits.
- For a session of purely mechanical work, starting it with
  `claude --model sonnet` costs about half; do not switch model in the
  middle of a session (the prompt cache is per model).
