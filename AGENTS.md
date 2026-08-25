# AGENTS.md - Developer & AI Contributor Guide for Sage

Welcome to the **Sage** project! This document establishes the engineering rules, architectural principles, code conventions, and workflows for all human and AI agent contributors.

---

## 1. Project Overview & Philosophy

**Sage** is a modern, blazingly fast, modular, multi-layer universal Linux package manager written from scratch in **Modern C++23**.

### Key Architectural Pillars:
* **Universal Multi-Layer Channel System**: Manages system root (`/`), shared runtimes (`/usr/lib/runtimes`), toolchains (`/opt/channels`), and user-level packages (`~/.local`) with strict FHS compliance via profile symlink aggregation.
* **Minimal Core Virtual Providers**: Strictly scopes virtual interfaces to fundamental, mutually exclusive system components: `virtual/init`, `virtual/udev`, `virtual/libc`, `virtual/coreutils`. Kernels, shells, awks, and non-core utilities are pure, independent, coexisting packages.
* **Declarative Reconcile Engine (`sage rebuild`)**: Reads `/etc/sage/system.toml`, calculates state diffs, commits guarded provider/package state transitions through LMDB transactions, and auto-regenerates native service scripts for the active init system. Filesystem payloads are staged on the target filesystem, journalled, and committed together with a durable pending-operation record; publication replays the journal idempotently, so an interrupted operation is detected and rolled forward by the next write command rather than silently forked. Concurrent same-privilege mutation of the target root is outside the supported operation model.
* **Universal Service Specification (`service.toml`)**: Decouples services from any single init daemon, auto-compiling into OpenRC, Runit, Systemd, Dinit, and s6 configurations.
* **Zero-Copy ACID State Storage (LMDB)**: Ultra-fast memory-mapped B+ tree database with nanosecond reads and Copy-on-Write transaction safety.
* **libarchive-Backed Streaming Archive Engine**: Reproducible `tar.zst` package archives via a thin RAII bridge (`sage.vendor.libarchive`); extraction is anchored to the target root and never follows symlinks outside it (see `docs/adr/0009-libarchive-migration.md`).
* **Native C++23 PubGrub / CDCL SAT Solver**: Mathematically complete dependency resolution with world-class human-readable conflict diagnostic cause trees.
* **Dynamic Linking by Default**: Dynamically links against system shared libraries (`liblmdb`, `libzstd`, `libtomlplusplus`, `libc`), with automated ELF `DT_NEEDED` and `DT_SONAME` scanning.

---

## 2. The 5 Invariable Engineering Rules

All code contributions MUST strictly adhere to these 5 rules:

### Rule 1: Strict Memory Safety & 100% RAII
* **Zero naked owning pointers**: Manual `new` and `delete` are strictly forbidden.
* **Automatic Resource Cleanup**: All LMDB environments (`MDB_env*`), transactions (`MDB_txn*`), cursors (`MDB_cursor*`), file descriptors, and Zstandard contexts (`ZSTD_DCtx*`, `ZSTD_CCtx*`) must be wrapped in RAII structures that automatically clean up upon scope exit or exceptions.
* **Boundary-safe Views**: Prefer `std::string_view` and `std::span` for zero-copy read operations. Always respect lifetime bounds.

### Rule 2: Clean, Small & Beautiful, High Performance
* **Total Project LOC < 10,000**: Aim for maximum conciseness and code density (~2,500 - 3,500 lines total).
* **Data-Oriented & Value Semantics**: Avoid deep inheritance hierarchies, abstract factory bloat, or excessive enterprise patterns. Keep structs simple, transparent, and moveable.
* **Modern Standard Library Features**: Use `std::expected<T, Error>` for zero-cost monadic error handling, `std::format` / `std::print` for type-safe fast formatting, and `std::ranges` for functional operations.

### Rule 3: DRY (Don't Repeat Yourself) with Zero-Cost Abstractions
* Common utilities (path normalization, binary serialization, ELF SONAME extraction, string manipulation, formatting) must be written once in `sage.util` and reused everywhere.
* Use `constexpr`, `inline`, and templates to ensure code reuse introduces zero runtime overhead.

### Rule 4: 100% C++23 Module System (Zero Headers in Business Logic)
* All application code, domain models, and engines must be implemented as `.cppm` C++23 module interface files.
* Third-party C/C++ library headers (`lmdb.h`, `zstd.h`, `toml++/toml.hpp`) are isolated exclusively within `src/vendor/` module partitions using Global Module Fragments (`module; #include <...>; export module ...;`).
* Application code and the CLI entry point must NEVER `#include` third-party headers; use `import sage.*;` or `import sage;`.

### Rule 5: Orthogonal Modular Architecture
* Maintain strict unidirectional, acyclic module dependencies:
  `vendor` -> `util` -> `package` -> `store/sys` -> `svc` -> `cli`. Engines
  compose through dedicated seams (`sage.repo` for channel/archive resolution,
  `sage.triggers` for post-transaction hooks) instead of growing monoliths.

---

## 3. Directory & Module Structure

```
sage/
├── xmake.lua                     # xmake build script
├── AGENTS.md                     # Agent & Developer instructions (this file)
├── README.md                     # Project overview and quick start
├── docs/
│   ├── ARCHITECTURE.md           # Full architectural specification
│   ├── MODULES.md                # Module reference and dependency DAG
│   ├── CLI_SPEC.md               # Complete CLI command-line reference
│   └── adr/                      # Architecture decision records
├── src/
│   ├── main.cpp                  # Entry point: global option dispatch only
│   ├── sage.cppm                 # Root module aggregating and re-exporting the engine
│   ├── vendor/                   # Layer 0: 3rd-party library RAII bridges
│   │   ├── lmdb.cppm             # sage.vendor.lmdb -- zero-copy B+ tree bridge
│   │   ├── zstd.cppm             # sage.vendor.zstd -- streaming Zstandard bridge
│   │   ├── toml.cppm             # sage.vendor.toml -- tomlplusplus bridge
│   │   ├── curl.cppm             # sage.vendor.curl -- libcurl session & downloads
│   │   └── archive.cppm          # sage.vendor.libarchive -- libarchive reader/writer bridge
│   ├── util/                     # Layer 1: foundation utilities (sage.util partitions)
│   │   ├── util.cppm             # primary interface, re-exports all partitions
│   │   ├── log.cppm              # :log -- ANSI colors & leveled console output
│   │   ├── str.cppm              # :str -- string views, glob matching, human sizes
│   │   ├── fs.cppm               # :fs -- path normalization, metadata snapshots, env
│   │   ├── lock.cppm             # :lock -- process-wide operation lock (flock)
│   │   ├── elf.cppm              # :elf -- DT_NEEDED / DT_SONAME scanner
│   │   └── hash.cppm             # :hash -- SHA-256 (OpenSSL EVP)
│   ├── package/                  # Layer 2: package domain (sage.package partitions)
│   │   ├── package.cppm          # primary interface, re-exports all partitions
│   │   ├── version.cppm          # :version -- epoch-ver-rel algebra, arch validation
│   │   ├── deps.cppm             # :deps -- dependency constraints & satisfaction
│   │   ├── trigger.cppm          # :trigger -- capability hooks & transaction triggers
│   │   ├── manifest.cppm         # :manifest -- installed-package manifest & identity
│   │   └── recipe.cppm           # :recipe -- recipe.toml build配方
│   ├── store/                    # Layer 3: persistent state & archives
│   │   ├── db.cppm               # sage.db -- LMDB registry, file ownership & transactions
│   │   └── archive.cppm          # sage.archive facade over store/archive/ partitions:
│   │       ├── core.cppm         #   :core    shared constants & result structs
│   │       ├── idx.cppm          #   :idx     files.idx integrity index I/O
│   │       ├── detail.cppm       #   :detail  anchored path safety & removal
│   │       ├── inspect.cppm      #   :inspect constant-cost .METADATA inspection
│   │       ├── extract.cppm      #   :extract parallel anchored extraction
│   │       └── pack.cppm         #   :pack    reproducible packing & repo index
│   ├── sys/                      # Layer 4: system engines
│   │   ├── config.cppm           # sage.config -- system.toml & provider configuration
│   │   ├── channel.cppm          # sage.channel -- multi-layer channels & FHS profile
│   │   ├── solver.cppm           # sage.solver -- PubGrub / CDCL dependency solver
│   │   ├── repo.cppm             # sage.repo -- channel pool & archive resolution
│   │   ├── triggers.cppm         # sage.triggers -- post-transaction trigger engine
│   │   └── rebuild.cppm          # sage.rebuild -- declarative reconcile orchestration
│   ├── svc/
│   │   └── service.cppm          # sage.service -- universal init script generator
│   └── cli/                      # Layer 5: command layer, one module per group
│       ├── cli.cppm              # sage.cli -- CliOptions, help text, argument parsing
│       ├── install.cppm          # sage.cli.install -- cmd_install (+ self-test hooks)
│       ├── remove.cppm           # sage.cli.remove -- cmd_remove
│       ├── rebuild.cppm          # sage.cli.rebuild -- cmd_rebuild
│       ├── build.cppm            # sage.cli.build -- build / repo index
│       ├── query.cppm            # sage.cli.query -- query / list / count / verify / status
│       └── toolchain.cppm        # sage.cli.toolchain -- channel / toolchains / shell / service
└── tests/                        # Standalone integration suite (sage-tests binary)
    ├── suite.cppm                # sage.tests -- end-to-end regression
    └── main.cpp                  # Suite entry point
```

**Layout rules.** A directory corresponds to one functional domain; a file
corresponds to exactly one module or partition. File names drop the redundant
`sage.*` prefix — the module name is found in the first line of the file.
Oversized modules are split into **partitions** behind a thin facade that
re-exports them, so import sites keep working unchanged. Hard cap: no source
file exceeds **1000 lines**; aim for well under that.


---

## 4. Build & Development Workflow with xmake

```bash
# Configure debug / release build
xmake f -m debug
xmake f -m release

# Build sage package manager
xmake

# Run the compiled binary
xmake run sage --help

# Build & run the integrated architecture and regression suite
xmake build sage-tests && xmake run sage-tests
```

---

## 5. Summary Checklist Before Any Contribution

- [ ] Does the code use C++23 modules (`.cppm`) without legacy `#include` leaking into business logic?
- [ ] Is all memory and OS resource management 100% RAII-compliant with zero manual memory management?
- [ ] Are third-party dependencies limited exclusively to dynamic `liblmdb`, `libzstd`, and `libtomlplusplus`?
- [ ] Is error handling monadic using `std::expected` / `std::optional` where applicable?
- [ ] Does the change keep the overall codebase clean, readable, and well under the 10,000 LOC limit?
