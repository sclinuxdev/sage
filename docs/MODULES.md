# Sage C++23 Module Reference & Dependency Topology

**Sage** is architected using **100% C++23 Modules (`.cppm`)**. There are zero legacy header files in business logic.

---

## 1. Module Dependency DAG

```mermaid
graph TD
    subgraph Layer0["Layer 0: Vendor Bridge Modules (src/vendor/)"]
        LMDB["sage.vendor.lmdb<br/>(Wraps liblmdb C API into RAII Env, Txn, Dbi, Cursor)"]
        ZSTD["sage.vendor.zstd<br/>(Wraps libzstd streaming compression/decompression)"]
        TOML["sage.vendor.toml<br/>(Wraps tomlplusplus)"]
        CURL["sage.vendor.curl<br/>(Wraps libcurl RAII session & streaming download)"]
    end

    subgraph Layer1["Layer 1: Utility & Foundation (src/model/)"]
        UTIL["sage.util<br/>(Path normalization, ELF SONAME scanner, SHA256, ANSI styles)"]
    end

    subgraph Layer2["Layer 2: Models & Parsing (src/model/)"]
        CONFIG["sage.config<br/>(system.toml parser & provider configuration)"]
        PKG["sage.package<br/>(Package model, recipe.toml, manifest.toml, triggers)"]
        SVC["sage.service<br/>(Universal service.toml -> Loom/OpenRC/Runit/Systemd/Dinit/s6)"]
    end

    subgraph SysSubgraph["System Subsystems (src/sys/)"]
        CHAN["sage.channel<br/>(Channel scopes, target roots, FHS Profile aggregator)"]
    end

    subgraph Layer3["Layer 3: Storage & Archiving (src/store/)"]
        DB["sage.db<br/>(LMDB zero-copy ACID state & file ownership engine)"]
        ARCH["sage.archive<br/>(Streaming Tar + Zstd engine; partitions :detail/:tape/:extract/:pack)"]
    end

    subgraph Layer4["Layer 4: High-Level Orchestration (src/sys/)"]
        SOLVER["sage.solver<br/>(Native PubGrub / CDCL SAT solver with cause tree diagnostics)"]
        REBUILD["sage.rebuild<br/>(Declarative reconcile engine & atomic swap pipeline)"]
    end

    subgraph Layer5["Layer 5: Primary Aggregator & CLI (src/cli/)"]
        ROOT["sage<br/>(Primary module: export import all sage.*)"]
        CLI["main.cpp + sage.cli.* modules<br/>(CliOptions/parsing + per-group command modules)"]
        TESTS["sage.tests<br/>(tests/ -- standalone integration suite binary)"]
    end

    LMDB --> DB
    ZSTD --> ARCH
    TOML --> CONFIG
    TOML --> PKG
    TOML --> SVC
    CURL --> CHAN
    UTIL --> PKG
    UTIL --> ARCH
    UTIL --> DB
    PKG --> DB
    PKG --> ARCH
    PKG --> SOLVER
    CONFIG --> CHAN
    CONFIG --> REBUILD
    SVC --> REBUILD
    CHAN --> REBUILD
    DB --> SOLVER
    DB --> REBUILD
    ARCH --> REBUILD
    SOLVER --> REBUILD
    REBUILD --> ROOT
    TESTS --> CLI
    ROOT --> CLI
```

---

## 2. Vendor Module Specifications

### `sage.vendor.lmdb`
* **Purpose**: Encapsulates LMDB in strict RAII classes (`Env`, `Txn`, `Dbi`, `Cursor`).
* **Exports**:
  - `class Env`: Manages `MDB_env*`, map size, max dbs.
  - `class Txn`: Manages `MDB_txn*`, automatic abort on destruction unless explicitly committed.
  - `class Dbi`: Manages `MDB_dbi` handle and zero-copy string view queries (`get`, `put`, `del`).
  - `class Cursor`: RAII iterator over key-value pairs.

### `sage.vendor.zstd`
* **Purpose**: Encapsulates Zstandard streaming API in RAII.
* **Exports**:
  - `class StreamDecompressor`: Wraps `ZSTD_DCtx*`, feeds compressed chunks and yields decompressed buffers.
  - `class StreamCompressor`: Wraps `ZSTD_CCtx*`, ingests raw bytes and flushes compressed stream.

### `sage.vendor.curl`
* **Purpose**: Encapsulates `libcurl` in strict RAII for HTTP/HTTPS transfers.
* **Exports**:
  - `class CurlSession`: RAII handle for `CURL*` with automatic cleanup.
  - `download_file(url, dest_path, progress_cb)`: Streams remote file to disk with progress callback.
  - `fetch_string(url)`: Fetches remote string/JSON/TOML metadata directly into memory.

### `sage.vendor.toml`
* **Purpose**: Wraps `tomlplusplus` to expose high-level type-safe table and value lookups.
