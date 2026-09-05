# Sage Project Developer & Agent Guidelines

欢迎来到 Sage（现代 Linux 声明式包管理器与系统调和体系）开发规范与文档导航。
本项目重写版本起步为 **`0.4.0`**，所有配置文件与元数据 Schema 版本统一起步为 **`1`**。

---

## 1. 核心设计原则 (Core Principles)

1. **拒绝重复造轮子 (Leverage Existing Ecosystem)**：
   - 依赖求解基于成熟高性能的 `pubgrub` crate。
   - **全链路统一 LMDB 数据库**：本地状态存储与软件源远端索引全量采用 `heed` (LMDB Rust 绑定)，实现全链路 `mmap` 零拷贝极速读取与 ACID 事务。
   - 压缩归档采用 `tar` + `zstd-rs` 流式处理。
   - CLI 前端完全基于 `clap` (derive) 与 `indicatif`，**当前阶段不引入 TUI**。
   - 沙箱隔离直接调用系统的 `bwrap` (Bubblewrap) 与 `fakeroot`。
2. **沿用明确的数据边界**：
   - 新增行为只实现当前需求；仅在确实需要用户配置或已有多个使用场景时扩展配置，不为“完全可扩展”新增配置或模板层。
   - 触发器（ldconfig, ca-certificates 等）全量由 `triggers/*.toml` 声明。
   - Init 服务转换由 `rclass/init-*.toml` 模板引擎驱动。
   - 编译器/工具链由 `rclass/*.toml` 驱动。
   - 软件源镜像 URL 完全由配置文件提供，代码中零预设 URL。
3. **原生支持多版本共存 (Native Multi-Version Slots)**：
   - 领域标识基于 `(Channel, PackageName, Slot)`，不同 Slot 与不同 Channel 的包在求解器与数据库中天然共存。
4. **极致性能与精简代码 (KISS)**：
   - 坚持 KISS 原则，以直接、清晰、易维护的实现为先，避免不必要的抽象与重复代码。
   - 关键路径优先采用无锁零拷贝 (`mmap`) 与符号 Interning 整数化比对。

---

## 2. Workspace Crate 拓扑导航

| Crate | 路径 | 核心职责 | 对应模块文档 |
| :--- | :--- | :--- | :--- |
| **`sage`** | `crates/sage` | 极速 CLI 命令行前端 (Clap) | [frontend.md](file:///home/ir/sage/docs/modules/frontend.md) |
| **`sage-core`** | `crates/sage-core` | 版本代数 (Epoch-Ver-Rel)、Slot 模型、Schema 定义、共享锁 | [core.md](file:///home/ir/sage/docs/modules/core.md) |
| **`sage-db`** | `crates/sage-db` | LMDB 状态存储、Slot 所有权追踪、事务崩溃恢复日志 | [db.md](file:///home/ir/sage/docs/modules/db.md) |
| **`sage-archive`** | `crates/sage-archive` | 流式 `tar.zst` 读写、`openat` 安全解包、`reflink` 零拷贝写出 | [archive.md](file:///home/ir/sage/docs/modules/archive.md) |
| **`sage-solver`** | `crates/sage-solver` | PubGrub 依赖求解器适配、LMDB 索引零拷贝点查、因果诊断 | [solver.md](file:///home/ir/sage/docs/modules/solver.md) |
| **`sage-sys`** | `crates/sage-sys` | Channel 聚合 (含 Python Channel)、系统调和 (Rebuild)、Init 服务 | [sys.md](file:///home/ir/sage/docs/modules/sys.md) |
| **`sage-build`** | `crates/sage-build` | `bwrap` 密闭沙箱、`rclass` 阶段执行、工具链审计、ELF 扫描 | [build.md](file:///home/ir/sage/docs/modules/build.md) |
| **`sage-repo`** | `crates/sage-repo` | 软件源 LMDB 索引同步、Ed25519 签名验证、分块下载 | [repo.md](file:///home/ir/sage/docs/modules/repo.md) |

架构设计总览：[ARCHITECTURE.md](file:///home/ir/sage/docs/ARCHITECTURE.md)

---

## 3. 规范文件索引 (Specifications, Schema v1)

| 规范对象 | 对应文件 / 路径 | 规范文档 |
| :--- | :--- | :--- |
| **声明式系统配置** | `/etc/sage/system.toml` | [system_spec.md](file:///home/ir/sage/docs/specs/system_spec.md) |
| **通道源配置** | `/etc/sage/channels.toml` | [channels_spec.md](file:///home/ir/sage/docs/specs/channels_spec.md) |
| **构建全局策略** | `/etc/sage/build.toml` | [build_config_spec.md](file:///home/ir/sage/docs/specs/build_config_spec.md) |
| **全源码构建与自举** | `bootstrap.toml` / recipe tree | [bootstrap_spec.md](file:///home/ir/sage/docs/specs/bootstrap_spec.md) |
| **包配方格式与单配方多包** | `recipes/.../recipe.toml` | [recipe_spec.md](file:///home/ir/sage/docs/specs/recipe_spec.md) |
| **配方目录树与分类体系** | `recipes/<category>/<pkg>/...` | [recipe_tree_spec.md](file:///home/ir/sage/docs/specs/recipe_tree_spec.md) |
| **构建类继承规范** | `rclass/*.toml` (如 `cmake.toml`, `cargo.toml`) | [rclass_spec.md](file:///home/ir/sage/docs/specs/rclass_spec.md) |
| **通用守护进程服务** | `.METADATA/service.toml` | [service_spec.md](file:///home/ir/sage/docs/specs/service_spec.md) |
| **触发器与替代项** | `.METADATA/triggers.toml` / sysusers / alternatives | [triggers_spec.md](file:///home/ir/sage/docs/specs/triggers_spec.md) |
| **二进制包归档格式** | `*.pkg.tar.zst` (`manifest.toml`, `files.idx`) | [package_archive_spec.md](file:///home/ir/sage/docs/specs/package_archive_spec.md) |
| **软件源远端 LMDB 索引** | `index.mdb.zst` & `index.mdb.sig` | [repo_index_spec.md](file:///home/ir/sage/docs/specs/repo_index_spec.md) |

---

## 4. Git 提交与代码注释规范 (Commit & Comment Rules)

### 4.1 语言强制要求 (English Only)
- **提交信息 (Commit Messages)**：必须 **全英文书写**。
- **代码注释与文档 (Code Comments & Docstrings)**：必须 **全英文书写** (`///`, `//!`, `//`)。

### 4.2 注释详尽度要求 (Write Plentiful & Detailed Comments)
- **注释要多写、详尽写**：在复杂算法、关键路径和底层逻辑处必须提供详尽的英文注释。
- **重点注释场景**：
  - `vercmp` 状态机与段切分规则；
  - `PubGrub` 候选打分、依赖转换与因果树推导；
  - `heed` LMDB 事务生命周期、文件多所有权移交与崩溃恢复状态机；
  - `openat` / `dirfd` 防路径穿越与安全解包边界；
  - `rclass` 变量展开、阶段覆盖与 `bwrap` 沙箱参数装配；
  - 关键结构体和公共函数必须附带 `///` 文档注释（说明功能、参数、返回值及可能的 Panic/Error 情况）。

### 4.3 提交信息格式 (Conventional Commits)
所有提交必须严格遵循 **Conventional Commits** 规范（英文书写）：
```text
<type>(<scope>): <short description in English>

[optional body in English]

[optional footer]
```

### 4.4 允许的 Type
- `feat`: 新增功能特性
- `fix`: 修复 Bug 或缺陷
- `refactor`: 代码重构（不增加新功能也不修复 Bug）
- `perf`: 性能提升改动
- `test`: 增加或修改测试用例
- `docs`: 文档或规范更新
- `chore`: 构建配置、依赖项调整或辅助工具变动

### 4.5 强制 Scope 范围
提交必须限定在以下合法作用域之一：
- `core`, `db`, `archive`, `solver`, `sys`, `build`, `rclass`, `repo`, `cli`
- 若涉及全局配置或多 Crate 联合变动，可使用 `workspace` 或 `deps`。

### 4.6 提交规则与禁忌
1. **原子性提交**：一个 Commit 只做一件事。
2. **动词现在时**：使用现在时动词开头（如 `feat(solver): implement slot-aware candidate ranking`，严禁 `fix bug` 或非规范短语）。
3. **CI 门禁要求**：
   - 必须通过 `cargo fmt --check`。
   - 必须通过 `cargo clippy --all-targets -- -D warnings`（零警告）。
   - 单元测试全部绿灯通过 (`cargo test`)。
