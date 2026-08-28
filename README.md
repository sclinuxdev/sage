# Sage

[![License: BSD-2-Clause](https://img.shields.io/badge/License-BSD--2--Clause-blue.svg)](https://opensource.org/licenses/BSD-2-Clause)
[![Rust: 2021](https://img.shields.io/badge/Rust-2021_Edition-orange.svg)](https://www.rust-lang.org)

**Sage** is an ultra-fast, declarative Linux package manager and system reconciliation engine written in Rust.  
**Sage** 是一个采用 Rust 编写的现代 Linux 极速声明式包管理器与系统状态调和引擎。

> [!NOTE]
> **Legacy Notice / 旧版说明**:  
> The previous C++ implementation prototype has been archived and moved to the [`prototype`](https://github.com/sclinuxdev/sage/tree/prototype) branch. The current `main` branch contains the clean Rust rewrite starting from version `0.4.0`.  
> 历史 C++ 原型实现已归档并移入 [`prototype`](https://github.com/sclinuxdev/sage/tree/prototype) 分支。当前 `main` 分支为自 `0.4.0` 版本起步的全新 Rust 重构版本。

---

## Key Features / 核心特性

- **Unified LMDB Engine / 全链路统一 LMDB 极速存储**:
  Local state (`/var/lib/sage/data.mdb`) and remote indexes (`index.mdb`) use LMDB (`heed`) for zero-copy, microsecond mmap queries and ACID transactions.  
  本地已安装状态与软件源远端索引全量采用 LMDB 数据库，实现零反序列化微秒级 `mmap` 读取与 ACID 事务。

- **Purely Data-Driven / 绝对零硬编码与数据驱动**:
  Triggers, init daemon generation, toolchains, and mirror URLs are purely configured via TOML specs and `rclass` templates. No hardcoded logic in Rust.  
  系统触发器、Init 守护进程生成、编译器与构建阶段完全由 TOML 规范与 `rclass` 模板驱动，Rust 引擎内部零硬编码。

- **Native Multi-Version Slots / 原生多版本共存**:
  Identifies packages by `(Channel, PackageName, Slot)` natively supporting parallel runtimes (e.g. `python3.12`, `python3.13`, `gcc14`, `gcc15`) without collisions.  
  基于 `(Channel, Name, Slot)` 领域实体，天然支持多版本运行时与工具链并行共存、互不污染。

- **Declarative System Reconciliation / 声明式系统调和 (`sage rebuild`)**:
  Computes exact diffs between `/etc/sage/system.toml` and installed state, orchestrating atomic transactions, service rendering, and trigger execution.  
  以 `/etc/sage/system.toml` 为系统唯一真相源，自动化执行差集求解、双阶段文件原子替换、服务配置渲染与触发器执行。

- **Hermetic Sandbox & Carving / 密闭沙箱与单配方多包切分**:
  `bwrap` sandboxed builds with Ptrace toolchain auditing, automatic ELF symbol resolution (`so:libfoo`), and single-build zero-duplicate payload carving (`libs`, `dev`, `doc`).  
  基于 Bubblewrap 的密闭沙箱，支持 Ptrace 工具链审计、ELF 动态符号反查以及单次编译零重复的多包切分。

- **Ultra Compact / 极致精简**:
  Strict engineering budget controlling total workspace Rust code under **9,000 lines of code**.
  严格控制全 Workspace Rust 代码总量在 **9,000 行以内**。

---

## Workspace Structure / 项目架构

```text
sage/
├── rclass/            # Declarative build & init classes (cmake, cargo, init-loom, init-systemd)
├── recipes/           # Per-architecture package recipes
└── crates/
    ├── sage-core/     # Version algebra, Slot model, PackageKey, HostLock
    ├── sage-db/       # LMDB state storage, file ownership & crash recovery log
    ├── sage-archive/  # Streaming tar.zst & openat traversal-safe unpacking
    ├── sage-solver/   # PubGrub SAT solver adapter & causality diagnosis
    ├── sage-sys/      # Declarative triggers, init renderer & reconciler
    ├── sage-build/    # bwrap sandbox, rclass execution & payload carver
    ├── sage-repo/     # Remote index sync, Ed25519 verification & indexer
    └── sage/          # Pure Clap CLI binary
```

---

## Quick Start / 快速开始

### Build & Test / 编译与测试

```bash
# Build release binary
cargo build --release

# Run all test suites & clippy checks
cargo test --all-targets
cargo clippy --all-targets -- -D warnings
```

---

## License / 开源协议

This project is licensed under the **2-Clause BSD License** (`SPDX-License-Identifier: BSD-2-Clause`).  
See the [LICENSE](LICENSE) file for details.
