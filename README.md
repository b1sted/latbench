<h3 align="center">latbench</h3>

<p align="center">
    <strong>
    A minimal C99 microbenchmark suite for UNIX systems.
    </strong>
</p>
<p align="center">
    <a href="#license">
        <img src="https://img.shields.io/badge/License-GPL--3.0-blue?style=flat-square" alt="License" />
    </a>
    <img src="https://img.shields.io/badge/Status-WIP-orange?style=flat-square" alt="Work in Progress" />
</p>
<p align="center">
    <a href="#build">Build</a> •
    <a href="#license">License</a>
</p>
<hr>

> **Work in progress.** Early initial setup — architecture, interfaces, and benchmarks are not finalized yet.

`latbench` is a dependency-free latency and microarchitecture benchmarking tool written in strict ISO C99 for Linux, macOS, and Android/Termux.

## Build

Requires a C99 compiler and GNU `make`.

### Linux

```bash
make            # -O2 release binary in build/bin/latbench
make dev        # strict -Werror debug build
make test       # unit tests + -O2 selftest
make leaks      # AddressSanitizer/UBSan test run
make lint       # style, format, clang-tidy, and cppcheck (when installed)
```

Add `STATIC=1` to link a self-contained binary (`make STATIC=1`). Static
linking is supported on Linux only.

### macOS Intel

Apple clang is sufficient. The build selects `src/platform/pal_macos.c`
automatically:

```bash
make clean && make && make dev && make test && make leaks
```

`make lint` additionally needs `clang-format` (and optionally `clang-tidy`
and `cppcheck`) from Homebrew or the LLVM tools.

### Android / Termux

Install the compiler and make with `pkg install clang make`, then build in the
Termux home directory:

```bash
make clean && make && make dev && make test && make leaks
```

The Android/Bionic platform layer avoids `-static` and `pthread_cancel`.
If the local Bionic ASan runtime reports leak-check failures that come from
system libraries, rerun the leak target with `ASAN_OPTIONS=detect_leaks=0`.

### Luckfox Pico (ARMv7 cross-compilation)

The Luckfox target uses the Rockchip uClibc toolchain. Cross-compile on the
host, then copy the binary to the board:

```bash
rm -rf build/obj/
make CC=/home/basted/src/SDKs/luckfox-pico/bin/arm-rockchip830-linux-uclibcgnueabihf-gcc \
     BIN=/home/basted/Projects/C/latbench/build/bin/latbench-luckfox

scp build/bin/latbench-luckfox root@172.32.0.93:/root/
ssh root@172.32.0.93 /root/latbench-luckfox selftest
```

`make test` cannot run on the x86_64 host because the ARM binaries fail with
`Exec format error`. Cross-build the unit-test binaries and run them on the
board instead:

```bash
make CC=/home/basted/src/SDKs/luckfox-pico/bin/arm-rockchip830-linux-uclibcgnueabihf-gcc \
     build/bin/tests/rel/core/test_stats \
     build/bin/tests/rel/core/test_tty \
     build/bin/tests/rel/core/test_util \
     build/bin/tests/rel/core/test_auto \
     build/bin/tests/rel/platform/test_pal
scp build/bin/tests/rel/core/test_* build/bin/tests/rel/platform/test_pal root@172.32.0.93:/root/
```

On the single-core board, `topo pingpong` and the multi-thread part of
`os shootdown` skip gracefully with exit status 0.

See [STYLEGUIDE.md](./STYLEGUIDE.md) for architecture rules, coding conventions, and development targets.

## License

Distributed under the **GNU General Public License v3.0**.

See [LICENSE](./LICENSE) for the full text.
