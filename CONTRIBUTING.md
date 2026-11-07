# Contributing to siliconmirror

Thanks for your interest. siliconmirror is in early development, so
please open an issue to discuss larger changes before starting work.

## Developer Certificate of Origin

Every commit must carry a `Signed-off-by:` line. By adding it you
certify the [Developer Certificate of Origin 1.1](https://developercertificate.org/):
you wrote the change, or otherwise have the right to submit it under
the project's license.

Add the line automatically with:

```sh
git commit -s
```

Use your real name and an email address you can be reached at.

## Licensing of new files

All code is licensed under GPL-2.0-or-later. Start every new source
file with:

```c
/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) <year> <name> */
```

Code taken from another project must:

- keep its original copyright and license notices;
- be under a license compatible with GPL-2.0-or-later;
- be named in the commit message (project, file and version).

Importing GPL-2.0-only code makes the combined work GPL-2.0-only. Say
so in the commit message so the decision is visible.

Do not commit firmware blobs, operating system images, device tree
blobs or other third-party binaries.

## Commit messages

- Subject line: `area: short summary`, in the imperative mood, with no
  trailing period and at most 72 characters. Examples:
  `cpu/arm64: add LDXR and STXR`, `hw/char/pl011: implement FIFO`.
- Then a blank line and a body that explains why the change is needed,
  wrapped at 72 columns.
- One logical change per commit. Every commit should build.

## Coding style

- C11.
- Indent with tabs, one tab per level, with a tab width of 4. Do not
  indent with spaces; spaces are only for aligning text after code.
- Lines at most 80 columns, counting a tab as 4 columns.
- Whitespace rules are in [.editorconfig](.editorconfig); most editors
  apply them automatically.
