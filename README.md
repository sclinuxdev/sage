# 🌿 Sage Package Manager

**Sage** is a high-performance, modular, multi-layer universal Linux package manager written from scratch in **Modern C++23**.

Designed for absolute user control, radical simplicity, and microsecond-level execution speed, Sage introduces a unified **Channel-based architecture**, full **FHS compliance**, **declarative system reconciliation**, and a **pure C++23 module-first design**.

---

## ✨ Key Features

* **⚡ Ultra-High Performance & Zero-Copy**:
  - Powered by **LMDB** (Memory-Mapped B+ Tree) for nanosecond package queries and Copy-on-Write ACID transactional safety.
  - Native **C++23 streaming archive engine** over **libzstd**, eliminating heavy legacy tar libraries.
* **🌐 Universal Multi-Layer Channel System**:
  - Manages multiple OS layers seamlessly: System root (`/`), shared runtimes (`/usr/lib/runtimes`), toolchains (`/opt/channels`), and user-level apps (`~/.local`).
  - Adheres strictly to **FHS (Filesystem Hierarchy Standard)** via declarative profile symlinks and environment hooks.
* **🎛️ Absolute System Sovereignty & Minimal Virtual Providers**:
  - Core system structural components (`virtual/init`, `virtual/udev`, `virtual/libc`) are fully swappable.
  - Natural coexisting components (Linux Kernels, Shells, Awk, Core utilities) are managed as pure, independent packages.
* **🔄 Declarative System Reconcile (`sage rebuild`)**:
  - Automatically compares `/etc/sage/system.toml` against active LMDB state.
  - Performs atomic package swaps (e.g., swapping `systemd` with `openrc` + `eudev`).
  - Automatically re-generates native service configurations for all installed daemons.
* **🔌 Universal Service Specification (`service.toml`)**:
  - Package daemons are declared with a single init-agnostic spec, auto-compiled into native **Loom**, **OpenRC**, **Runit**, **Systemd**, **Dinit**, or **s6** definitions.
* **🧩 Native C++23 PubGrub / CDCL SAT Dependency Solver**:
  - Zero external solver dependencies.
  - Generates clear, human-readable conflict diagnostic cause trees.
* **🛡️ 100% C++23 Modules & RAII Memory Safety**:
  - Fully modular `.cppm` architecture with zero header pollution (`import std;`).
  - Dynamic linking against system shared libraries (`liblmdb`, `libzstd`, `libtomlplusplus`, `libcurl`).

---

## 🏗️ Architecture at a Glance

```
sage
 ├── 状态引擎: LMDB (零拷贝 mmap B+ 树，/var/lib/sage/data.mdb，微秒级读写)
 ├── 归档引擎: libzstd + 原生 C++23 流式 Tar 解包与打包器 (无 libarchive 依赖)
 ├── 求解引擎: 自研 C++23 PubGrub / CDCL SAT 依赖求解器 (顶级因果树诊断)
 ├── 服务体系: 通用 service.toml -> Loom / OpenRC / Runit / Systemd / Dinit / s6 自动生成
 ├── 抽象收敛: 精简虚拟提供者 virtual/init, virtual/udev, virtual/libc
 └── 系统重构: sage rebuild 自动基于 /etc/sage/system.toml 执行原子大件迁移与服务重构
```

---

## 🚀 Building & Running

### Requirements
* **xmake** (>= 2.8.0)
* **GCC** (>= 14.0 / 15.0 with C++23 modules) or **Clang** (>= 18.0)
* Dynamic system libraries: `liblmdb`, `libzstd`, `libtomlplusplus`, `libcurl`

### Build with xmake
```bash
# Build sage in release mode
xmake f -m release
xmake

# Run the compiled binary
xmake run sage --help

# Run internal engine test suite
xmake run sage test-suite
```

---

## 📖 Documentation
* [Architecture Specification](docs/ARCHITECTURE.md)
* [Next Steps: Multi-Toolchain Coexistence & Sub-Channels Design](docs/NEXTSTEP.md)
* [Module Reference & Dependency Topology](docs/MODULES.md)
* [CLI Command Specification](docs/CLI_SPEC.md)
* [Developer & AI Contributor Guide](AGENTS.md)
