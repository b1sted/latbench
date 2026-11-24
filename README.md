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

Requires a C99 compiler and GNU `make`:

```bash
make
```

See [STYLEGUIDE.md](./STYLEGUIDE.md) for architecture rules, coding conventions, and development targets (`make dev`, `make test`, `make lint`).

## License

Distributed under the **GNU General Public License v3.0**.

See [LICENSE](./LICENSE) for the full text.
