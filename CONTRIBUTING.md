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

## Project layout

Read [docs/architecture.md](docs/architecture.md) before adding code. It
describes where each kind of code goes and which modules may depend on
which. `make test` fails if a module includes headers from a module it
must not depend on.

## Building and testing

Build and run the tests before sending a change:

```sh
make test
```

CI builds with both GCC and Clang, using two presets: `ci`
(AddressSanitizer, UBSan, warnings as errors) and `ci-release`
(optimized, warnings as errors). Run the same checks locally with:

```sh
make PRESET=ci test
make PRESET=ci CC=clang test
make PRESET=ci-release test
make PRESET=ci-release CC=clang test
make check-format
```

The bare-metal tests in `tests/baremetal/` run small AArch64 programs
through the `smirror` command line. They need the GNU binutils for
AArch64; CMake skips them, with a message, when it cannot find them,
except on GitHub Actions, where CI must run them and a missing tool is
an error.

The raspi3b board starts its cores in a boot stub written in AArch64
assembly, `src/machine/raspi3b_stub.s`. smirror is built from its bytes
kept in `src/machine/raspi3b_stub.inc`, so that building it needs no
AArch64 tools. After changing the stub, run `make stub` to update that
file and commit both; the test `baremetal.raspi3b-stub-up-to-date`
fails until they match.

## Coding style

- C11.
- Naming:
  - types are CamelCase typedefs: `Bus`, `BusMmioOps`;
  - functions and variables are snake_case: `bus_read()`;
  - macros and enum constants are UPPER_CASE: `BUS_OK`;
  - names a module exports start with the module or device name
    (`bus_`, `Bus`, `BUS_`); everything else in a `.c` file is `static`.
- Header guards are named after the path from `src/`: `core/bus.h` uses
  `CORE_BUS_H`.
- Indent with tabs, one tab per level, with a tab width of 4. Do not
  indent with spaces; spaces are only for aligning text after code.
- Lines at most 80 columns, counting a tab as 4 columns.
- Whitespace rules are in [.editorconfig](.editorconfig); most editors
  apply them automatically.
- Format C code with clang-format 21, using the project's
  [.clang-format](.clang-format):

  ```sh
  make format
  ```

  CI fails if any `.c` or `.h` file is not formatted.
