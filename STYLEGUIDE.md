# latbench C99 Engineering Style Guide

This document defines the coding, architecture, and safety rules for the
`latbench` codebase. Every contribution must follow these rules. Automated
checks (`make format-check`, `make lint`, `make test`, and `make leaks`) enforce
the mechanical parts of the guide; reviewers enforce the rest.

## 1. Language Standard and Build Rules

- The project targets **strict ISO C99** (`-std=c99`).
- Do **not** define `-D_POSIX_C_SOURCE=200809L` globally in `CFLAGS`: doing so
  breaks Apple SDK headers on macOS. Feature-test macros must be scoped
  strictly to platform sources:
  - `src/platform/{posix,linux-common,linux,android}/`: `#define _POSIX_C_SOURCE 200809L`
  - `src/platform/macos/`: `#define _DARWIN_C_SOURCE 1`
  - `src/core/` and `src/bench/` must compile under strict `-std=c99` with no
    OS feature-test macros.
- Do **not** use C11 or later features. In particular, the following are
  explicitly banned:
  - `_Generic`
  - anonymous structs and anonymous unions
  - `<threads.h>`
  - `<stdatomic.h>`
  - `_Static_assert`
  - variable length arrays (VLAs)
- Any array whose size depends on runtime data must be allocated on the heap (or
  via `pal_mem_alloc()`) or bounded by a compile-time constant. Prefer
  fixed-size buffers from `include/defs.h` for CLI/UI/metadata and always pass
  their capacity explicitly.
- **Compiler extensions:** GNU statement expressions, nested functions, and
  `typeof` are banned everywhere. Compiler-specific primitives required by
  microbenchmarks (`__atomic_*` / `__sync_*`, `noinline` attributes, and
  compiler barriers such as `__asm__ volatile`) are banned in `src/core/` and
  `src/bench/`, and may appear **only** behind portable wrappers in
  `include/pal/compiler.h` and `src/platform/`.
- **Optimization level:** Benchmark release builds (`make`) must compile with
  `-O2` (never `-O0` or sanitizers). Avoid `-flto` by default unless verified by
  `latbench selftest`, as cross-unit LTO may fold away synthetic benchmark loops.
- Every source and header file starts with the SPDX identifier:
  `/* SPDX-License-Identifier: GPL-3.0 */`.

## 2. Repository Architecture

The code is split into a portable core, benchmark modules, and a thin platform
abstraction layer (PAL):

```text
include/            Public headers shared across core, bench, and PAL
  pal/              Platform abstraction and compiler primitive interfaces
src/core/           Portable engine: CLI, stats, step detector, TTY plots, main
src/bench/          Benchmark suites (sys, mem, tlb, core, branch, dram, topo, os)
src/platform/       Platform implementations
  posix/            POSIX.1-2008 fallbacks
  linux-common/     Shared Linux/Android implementation (incl. Luckfox)
  linux/            Linux-specific implementation (affinity, madvise/THP)
  android/          Android/Termux-specific adjustments (Bionic)
  macos/            macOS-specific implementation (mach_absolute_time, QoS)
tests/              Unit, integration, and optimizer self-tests
```

Rules for the layering:

- `src/core/` and `src/bench/` must not include OS-specific headers or call
  platform-specific APIs directly. They interact only with interfaces declared
  in `include/` and `include/pal/`.
- Platform implementations provide the PAL symbols and may include their own
  OS headers.
- Shared helpers live in `src/core/` and are exported through `include/`.
- Headers must be self-contained: each header includes everything it needs and
  is protected by an include guard.

## 3. Naming and Namespacing

- All identifiers use strict `snake_case`.
- Every non-static function, type, and global carries a module prefix. The
  prefix identifies the owning module, not the file name alone:

  | Prefix    | Module                                              |
  |-----------|-----------------------------------------------------|
  | `lb_`     | Top-level orchestration, CLI config, `auto`/`selftest` |
  | `pal_`    | Platform abstraction (timer, memory, threads, atomics) |
  | `stats_`  | Percentiles, step detection, frequency-drift canary |
  | `tty_`    | Terminal rendering, ASCII/Unicode plots, CSV output |
  | `sys_`    | System calibration and throttling benchmarks        |
  | `mem_`    | Cache size, line, associativity, prefetcher tests   |
  | `tlb_`    | TLB and page-fault benchmarks                       |
  | `core_`   | Pipeline ILP, MLP, alignment, STLF benchmarks       |
  | `br_`     | Branch predictor and RSB/BTB benchmarks             |
  | `dram_`   | Memory bandwidth and DRAM row-buffer benchmarks     |
  | `topo_`   | Core-to-core ping-pong, false sharing, atomics      |
  | `os_`     | Syscall, context switch, shootdown, fork, signal    |
  | `util_`   | Generic portable helpers                            |

- Static functions may omit the prefix when the file is small and the name is
  unambiguous, but a module prefix is preferred.
- Macros and compile-time constants use `SCREAMING_SNAKE_CASE`.
- Type names use `snake_case` with a `_t` suffix (for example `lb_cfg_t`).
- Enum members use `SCREAMING_SNAKE_CASE` with a module-related prefix.
- `main` is the only function exempt from the prefix rule.

## 4. Formatting

Formatting is enforced by `clang-format` using the repository `.clang-format`.

- Indent with **4 spaces**. Never use tab characters.
- Maximum line length is **100 columns**.
- Braces follow a K&R style:
  - Function definitions place the opening brace on its own line.
  - Control statements (`if`, `for`, `while`, `switch`, `do`) place the
    opening brace on the same line.
  - `struct`, `enum`, and `union` bodies place the opening brace on the same
    line.
  - `else` stays on the same line as the closing brace.
- **Braces are mandatory for every body**, including single-statement
  `if`/`else`, loops, and `do`/`while`. `clang-format` inserts them
  automatically through `InsertBraces: true`.
- Pointer placement attaches the asterisk to the identifier:
  `char *buf`, not `char* buf`; `const char *name`, not `const char* name`.
- Do not align consecutive assignments or declarations.
- One statement per line. Do not use the comma operator to pack statements.
- `/* clang-format off */` guards are permitted only around generated lookup/jump
  tables or manual unroll blocks in `src/bench/`.
- Comments are part of the source of truth. Automated formatting may re-align
  comments but must never change their text.

Run the formatter before committing:

```bash
make format
make format-check
```

## 5. Types

- Include `<stdint.h>` and use fixed-width types for all data that crosses a
  module, file, syscall, or hardware boundary: `uint8_t`, `uint16_t`,
  `uint32_t`, `uint64_t`, `int8_t`, `int16_t`, `int32_t`, `int64_t`, and
  `uintptr_t` for pointer-chasing arithmetic.
- Use `size_t` for object sizes, buffer capacities, indices, and counts.
- Use `bool` from `<stdbool.h>` for boolean values.
- Plain `int` is restricted to:
  - function exit codes and status values,
  - `argc` and `argv` processing,
  - trivial loop counters over small, local, compile-time-bounded ranges.
- Raw `long`, `unsigned long`, `short`, and `unsigned short` must not be used
  for stored data. When a libc or POSIX API requires them (for example
  `strtol`, `sysconf`, or `ftell`), convert at the call site and immediately
  narrow to a fixed-width type with an explicit range check.
- Always cast explicitly when narrowing. Never rely on implicit truncation.
- Use `PRIu64`, `PRId64`, and the other `<inttypes.h>` macros when formatting
  fixed-width types.
- **Floating-point types (`float`, `double`):** must not be used for
  identifiers, pointers, array indices, bitmasks, or loop control. They are
  permitted in statistical analysis/display (`stats_`, `tty_`) and in FPU
  pipeline microbenchmarks (`core_ilp`).

## 6. Const Correctness

- Mark pointer parameters `const` whenever the callee does not modify the
  pointee: `const char *path`, `const uint8_t *data`.
- Mark pointer-to-pointer arrays `const` at the appropriate level, for example
  `const char *const items[]`.
- Apply `const` only when it is safe. Do not add `const` when the address is
  passed to an API that expects a mutable pointer and would emit a
  `discarded-qualifiers` warning. Typical examples are `execv`, `open`,
  `strtol`, `strtoul`, and legacy platform interfaces.
- Prefer `const` on locals that are assigned once and never modified.
- Never cast away `const` to silence a warning. Fix the interface instead.

## 7. Initialization, Ownership, and Memory Safety

- Initialize every variable at declaration. Pointers default to `NULL`,
  scalars to `0`, and structures to `{0}`.
- When initializing a structure with designated initializers, initialize every
  field that matters to avoid `-Wmissing-field-initializers`. Prefer `{0}`
  followed by explicit assignments for partially populated structures.
- **Resource handles vs. POD structs:** Use opaque pointers (`pal_mem_t *`,
  `pal_thread_t *`, `stats_series_t *`) for platform-dependent or
  resource-owning objects. Flat configuration and sample POD structs
  (`lb_cfg_t`, `lb_point_t`) may be exposed in `include/` and allocated on the
  stack. Hot timed loops must operate directly on raw pointers extracted before
  entering the timed region.
- **Benchmark buffer initialization exception:** Do **not** eagerly zero or
  `memset()` large arenas allocated via `pal_mem_alloc()` unless a test
  explicitly requires pre-faulted pages. In particular, `tlb_fault` relies on
  untouched virtual pages to measure first-touch OS page-fault latency, whereas
  `mem_*` and `tlb_size` populate pointer chains prior to starting the timer.
- Check every allocation, every syscall result, and every pointer returned by a
  library function before use.
- `free()` accepts `NULL`; set pointers to `NULL` after freeing when the
  variable remains in scope.
- Never index an array without proving the index is within bounds.
- Use `snprintf` rather than `sprintf`, and always pass the destination
  capacity.
- Do not ignore return values from `read`, `write`, `open`, `close`, `mprotect`,
  `mmap`, or allocation functions in production paths.

### Single exit point and `goto cleanup`

Functions that acquire more than one resource (file descriptors, heap
allocations, threads, platform handles) use a single exit point and unwind in
LIFO order:

```c
bool example(void)
{
    int fd = -1;
    char *buf = NULL;
    bool ok = false;

    fd = open("/path", O_RDONLY);
    if (fd < 0) {
        goto cleanup;
    }

    buf = malloc(BUFFER_SIZE);
    if (buf == NULL) {
        goto cleanup;
    }

    /* ... use fd and buf ... */
    ok = true;

cleanup:
    free(buf);
    if (fd >= 0) {
        close(fd);
    }
    return ok;
}
```

- The `cleanup` label is the last statement before the return.
- Release resources in the reverse order of acquisition.
- Guard cleanup with checks so it is safe on partially initialized paths.
- Do not use `goto` for ordinary control flow; it is reserved for cleanup.

## 8. Header Ordering

Within every `.c` file, include headers in this order, separated by blank
lines:

1. The module's own header (for example `#include "bench_mem.h"`).
2. C99 standard headers (`<stdbool.h>`, `<stdint.h>`, `<stdio.h>`, ...).
3. POSIX / OS headers (`<fcntl.h>`, `<unistd.h>`, `<sys/...>`, ...) — **allowed
   only inside `src/platform/`**.
4. Project headers (`"pal/pal.h"`, `"stats.h"`, `"defs.h"`, ...).

Headers use include guards rather than `#pragma once`. Do not include a header
that is not needed by the translation unit.

## 9. Functions and Control Flow

- Keep functions focused and short enough to review. Extract helpers when a
  function grows beyond a single responsibility.
- Validate all parameters at the top of public functions and return early on
  invalid input.
- Prefer early returns for error checks. Use `goto cleanup` only for resource
  unwinding as described above.
- Every `switch` handles all enum values, either with explicit cases or a
  `default` that fails safely.
- Do not use assignments inside conditions. Assign first, then test.
- Avoid macros that hide control flow (`return`, `break`, `goto`). Prefer
  `static inline` functions for small helpers. Simple statement-repetition
  macros (such as `LB_UNROLL_8(stmt)`) are permitted inside `src/bench/` hot
  loops to reduce branch overhead, provided they contain no hidden control flow.

## 10. Diagnostics, Output, and Measurement Integrity

- User-facing output goes through the `tty_` module so formatting, plots, ANSI
  heatmaps, and CSV rendering remain consistent.
- **Stream separation:** In `--csv` mode, `stdout` must contain **only** valid
  CSV records. All progress output, `V_PRINTF` verbose diagnostics, `[WARN]`
  notices (including DVFS frequency-drift warnings), and `[ERROR]` messages must
  go to `stderr`.
- Never perform I/O (`printf`, `fprintf`, `fflush`) inside timed measurement
  regions.
- Output must remain stable across formatting-only changes.

## 11. Testing and Validation

Every change must pass:

```bash
make                # -O2 release build (never ships sanitizers or -Werror by default)
make format-check   # formatting and brace insertion are clean
make dev            # strict debug build (-Werror -Wall -Wextra -Wconversion ...)
make test           # unit tests + `./build/bin/latbench selftest` compiled with -O2
make leaks          # AddressSanitizer/UBSan run with small bounded test buffers
make lint           # clang-tidy, cppcheck, and format-check pass
```

- Tests live in `tests/` and are split into `core/` and `platform/`.
- `make test` must execute `./build/bin/latbench selftest` on the **`-O2`** binary
  to verify that compiler optimizations did not eliminate benchmark loops or
  break Sattolo single-cycle integrity.
- `make leaks` must never be treated as a latency benchmark and must bound test
  allocations so it remains runnable on memory-constrained targets.
- Deliberate low-level hardware probes (for example, unaligned loads in
  `core_align` or partial-overlap loads in `core_stlf`) that trigger static
  analyzer false positives must use targeted, commented suppressions rather than
  disabling global lint checks.
- Platform-specific tests (such as `topo_pingpong` on single-core Luckfox or
  pinned-core tests on macOS) must skip gracefully with a clear notice rather
  than failing.

## 12. Commits

- Use Conventional Commits: `feat:`, `fix:`, `refactor:`, `style:`, `docs:`,
  `test:`, `build:`, or `chore:`.
- Keep commits focused. Formatting-only changes belong in a `style:` commit.
- All commits and merge commits must be GPG-signed (`git commit -S`,
  `git merge -S`).
- Write commit messages in English and describe the intent, not just the files
  touched.

