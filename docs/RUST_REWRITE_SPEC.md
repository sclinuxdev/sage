# Sage 3.0: Rust 重写技术架构与完整系统设计规范 (Master Specification)

**版本:** 3.0-Draft  
**目标生态位:** `libsage` (底层核心引擎库，类比 `libalpm` / `libpkg`) + `sage` (CLI / TUI 双模前端)  
**语言与工具链:** Rust (Edition 2024 / 2021), Cargo Workspace, Tokio (异步 I/O), Rayon (并行计算), Ratatui (TUI), Clap (CLI)  
**文档状态:** 全特性 / 全概念 / 全算法工程实现规范  

---

# 目录

1. [项目愿景与架构全景](#1-项目愿景与架构全景)
2. [Cargo Workspace 与 Crate 拓扑设计](#2-cargo-workspace-与-crate-拓扑设计)
3. [基础领域模型与代数规范 (`libsage-core`)](#3-基础领域模型与代数规范-libsage-core)
4. [LMDB 零拷贝 ACID 状态存储引擎 (`libsage-db`)](#4-lmdb-零拷贝-acid-状态存储引擎-libsage-db)
5. [事务性流式归档与安全解包引擎 (`libsage-archive`)](#5-事务性流式归档与安全解包引擎-libsage-archive)
6. [PubGrub / CDCL SAT 依赖求解器与因果诊断 (`libsage-solver`)](#6-pubgrub--cdcl-sat-依赖求解器与因果诊断-libsage-solver)
7. [多层 Channel 运行时与 FHS Profile 聚合器 (`libsage-sys::channel`)](#7-多层-channel-运行时与-fhs-profile-聚合器-libsage-syschannel)
8. [通用服务声明与多 Init 生成引擎 (`libsage-sys::service`)](#8-通用服务声明与多-init-生成引擎-libsage-sysservice)
9. [事务后触发器、Sysusers 与 Alternatives 仲裁 (`libsage-sys::triggers`)](#9-事务后触发器sysusers-与-alternatives-仲裁-libsage-systriggers)
10. [声明式系统 Reconcile 与原子状态切换 (`libsage-sys::rebuild`)](#10-声明式系统-reconcile-与原子状态切换-libsage-sysrebuild)
11. [密闭构建沙箱、工具链审计与配方引擎 (`libsage-build`)](#11-密闭构建沙箱工具链审计与配方引擎-libsage-build)
12. [远程仓库同步、Ed25519 签名与分块下载 (`libsage-repo`)](#12-远程仓库同步ed25519-签名与分块下载-libsage-repo)
13. [全局主机锁与 Dry-Run 零写安全保证 (`libsage-core::lock`)](#13-全局主机锁与-dry-run-零写安全保证-libsage-corelock)
14. [前端架构：CLI 命令行与 Ratatui TUI (`sage`)](#14-前端架构cli-命令行与-ratatui-tui-sage)
15. [C-FFI 与动态链接兼容层 (libalpm 生态位)](#15-c-ffi-与动态链接兼容层-libalpm-生态位)
16. [Rust 重写工程规范与落地路线图](#16-rust-重写工程规范与落地路线图)

---

# 1. 项目愿景与架构全景

## 1.1 核心设计理念

Sage 是面向现代 Linux 发行版的模块化、声明式、超高性能通用包管理与系统调和体系。在 Rust 重写版本中，系统被严格解耦为两大部分：

*   **`libsage` (底层核心库 / Libalpm 生态位)**：纯粹的业务与系统状态库，提供无副作用或明确事务边界的 API，支持通过 Rust 原生类型、Trait 与 C-FFI 绑定供外部程序（如系统安装器、初始化脚本、第三方 GUI/TUI 前端、自动化运维代理）嵌入。
*   **`sage` (应用前端)**：面向终端用户的交互层，集成 Clap 构建的 CLI 命令与 Ratatui 构建的现代化 TUI，负责终端渲染、人机交互、信号处理与日志格式化。

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                            Frontend Layer (`sage`)                          │
├──────────────────────────────────────┬──────────────────────────────────────┤
│           CLI Interface (clap)       │           TUI Dashboard (ratatui)    │
│  `sage install / rebuild / ...`      │   Interactive Package & System UI    │
└───────────────────────────────────┬──┴──────────────────────────────────────┘
                                    │ Direct Rust API Call
┌───────────────────────────────────▼─────────────────────────────────────────┐
│                      libsage Unified Facade (Root Crate)                    │
├─────────────────────────────────────────────────────────────────────────────┤
│  • Public Session / Context Engine  • Transaction Coordinator              │
│  • Event & Progress Stream Hooks    • C-FFI Export (`libsage.so`)           │
└──────┬────────────┬─────────────┬─────────────┬─────────────┬───────────────┘
       │            │             │             │             │
┌──────▼─────┐┌─────▼──────┐┌─────▼──────┐┌─────▼──────┐┌─────▼──────┐┌────────▼─────┐
│libsage-core││libsage-db  ││libsage-arch││libsage-solv││libsage-sys ││libsage-build│
│• Primitives││• LMDB ACID ││• tar.zst   ││• PubGrub   ││• Channels  ││• bwrap/ptrace│
│• Epoch-Ver ││• Ownership ││• openat    ││• CDCL SAT  ││• Rebuild   ││• Recipe v1/v2│
│• Manifest  ││• Claims    ││• Journal   ││• CauseTree ││• ServiceGen││• ELF Scanner │
│• Hash/Lock ││• Recovery  ││• SafeExtract││• MSRV     ││• Triggers  ││• Repro Verify│
└────────────┘└────────────┘└────────────┘└────────────┘└────────────┘└──────────────┘
```

## 1.2 核心机制概览

1.  **多层 Channel 与 FHS Profile 聚合**：System (`/`)、Runtime (`/usr/lib/runtimes`)、Toolchain (`/opt/channels`)、User (`~/.local`) 四层正交管理，消除传统包管理器的版本冲突问题。
2.  **极小核心虚拟提供者**：严格限定 `virtual/init`、`virtual/udev`、`virtual/libc`、`virtual/coreutils`、`virtual/depmod` 为互斥系统核心；其余内核、Shell、Awk 等均为独立共存包。
3.  **LMDB 零拷贝 ACID 状态存储**：纳秒级 mmap 读取，强写事务保证，文件多所有权追踪与移交机制（Handover）。
4.  **两阶段事务与日志幂等重放**：磁盘 Payload 暂存 (`/var/lib/sage/transactions/txn-<id>`) + 校验索引 + 事务状态持久化，系统宕机后下次运行自动检测并前向恢复。
5.  **PubGrub / CDCL SAT 依赖求解**：具备原因树（Cause Tree）解释能力的完备依赖求解算法，原生支持版本区间、版本化 Provides 与 MSRV 局部优先决策。
6.  **通用服务定义与多 Init 编译**：统一的 `service.toml` (v2)，零依赖编译至 Loom、OpenRC、Runit、Systemd、Dinit、s6 目标配置。
7.  **密闭构建沙箱与二进制工具链审计**：基于 Bubblewrap、Fakeroot、Ptrace/Seccomp 的深度审计，确保仅有被探测且授权的编译器/链接器穿越 `execve`，生成 100% 确定性、防篡改的制品清单。

---

# 2. Cargo Workspace 与 Crate 拓扑设计

为保证高内聚低耦合，项目采用标准 Cargo Workspace 架构：

```
sage-workspace/
├── Cargo.toml                      # Workspace Root Config
├── crates/
│   ├── libsage-core/               # 基础类型、版本代数、Schema 模型、通用工具
│   ├── libsage-db/                 # LMDB 封装、表结构、所有权追踪、恢复记录
│   ├── libsage-archive/            # 流式 Tar+Zstd 解封包、openat 安全解压、事务日志
│   ├── libsage-solver/             # PubGrub CDCL SAT 依赖求解器与冲突树
│   ├── libsage-sys/                # Channel、Reconcile 调和、Service 生成、Triggers 引擎
│   ├── libsage-build/              # bwrap 沙箱、ptrace 审计、Recipe 解析与构建规划
│   ├── libsage-repo/               # 远端索引、Ed25519 验签、异步并行分块下载
│   ├── libsage/                    # libsage 统一门面 Crate，对外公开 API 与 C-FFI
│   └── sage/                       # CLI 与 TUI 二进制程序
└── tests/                          # 跨 Crate 端的集成测试套件
```

### 依赖 DAG 规则
*   `libsage-core` 不依赖任何内部 Crate。
*   `libsage-db` 依赖 `libsage-core`。
*   `libsage-archive` 依赖 `libsage-core`。
*   `libsage-solver` 依赖 `libsage-core`, `libsage-db`。
*   `libsage-sys` 依赖 `libsage-core`, `libsage-db`, `libsage-archive`, `libsage-solver`。
*   `libsage-build` 依赖 `libsage-core`, `libsage-archive`, `libsage-db`。
*   `libsage-repo` 依赖 `libsage-core`, `libsage-archive`。
*   `libsage` 聚合上述所有 Crates，并导出完整上下文会话。
*   `sage` 仅依赖 `libsage`。

---

# 3. 基础领域模型与代数规范 (`libsage-core`)

## 3.1 明确 Schema 版本控制规范

所有配置文件、配方、清单和索引均强制包含 `schema_version: u32`：

| 配置文件 | 当前版本 | 兼容策略 |
| :--- | :--- | :--- |
| `/etc/sage/system.toml` | `1` | 声明式系统配置，若版本高于支持上限则安全中止 |
| `/etc/sage/channels.toml` | `1` | 通道源与优先级配置 |
| `/etc/sage/build.toml` | `1` | 构建工具链策略与资源上限 |
| `recipe.toml` | `1`, `2` | 包配方格式（v1 兼容历史脚本，v2 结构化声明） |
| `.METADATA/manifest.toml`| `1` | 归档与已安装包状态清单 |
| `.METADATA/service.toml` | `2` | 通用守护进程服务规范 (v2 为命令数组格式) |
| `.METADATA/triggers.toml`| `1` | 文件路径与能力触发器 |
| `index.toml` (远端源索引) | `1` | 软件源索引与 SHA256/大小索引 |
| `.METADATA/files.idx`   | `1` | 逐文件路径、模式、大小、SHA256 完整性清单 |

## 3.2 版本代数与比较规范 (Epoch-Ver-Rel)

包版本遵循 `[epoch:]version[-release]` 格式：

```rust
#[derive(Debug, Clone, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub struct Version {
    pub epoch: u32,
    pub upstream: String,
    pub release: u32,
}
```

### 比较算法 (`vercmp`):
1.  **Epoch 比较**：数值绝对优先比较 (`epoch_a.cmp(&epoch_b)`)。
2.  **Upstream 比较**：
    *   将版本字符串通过正则/状态机划分为连续的 **纯数字段** 与 **纯字母段**，分隔符（`.`、`-`、`_`、`+`、`~`）作为段切分点。
    *   特殊符号 `~` 具有最高前置优先级（`1.0~rc1 < 1.0`）。
    *   数字段按数值比较（忽略前导零，`01 == 1`；`10 > 2`）。
    *   字母段按 ASCII 字典序比较（`alpha < beta`）。
    *   数字段始终大于字母段。
3.  **Release 比较**：若 Upstream 相同，按 `release` 数值比较。

## 3.3 依赖与约束模型 (`Dependency`, `ConstraintOp`)

```rust
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub enum ConstraintOp {
    Any,            // 无约束 (如 "zlib")
    Equal,          // = (如 "virtual/libc = 2.44")
    NotEqual,       // !=
    Greater,        // >
    GreaterOrEqual, // >= (如 "gcc >= 14.2")
    Less,           // <
    LessOrEqual,    // <=
}

#[derive(Debug, Clone, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub struct Dependency {
    pub name: String,
    pub op: ConstraintOp,
    pub version: Option<Version>,
}

impl Dependency {
    pub fn satisfies(&self, target_ver: &Version) -> bool;
    pub fn parse(s: &str) -> Result<Self, ParseError>;
}
```

## 3.4 架构定义 (`Arch`)

*   支持枚举：`Amd64`, `Aarch64`, `Riscv64`, `Armv7`, `Any`。
*   别名兼容：输入 `x86_64` 自动规范化为 `amd64`。
*   `Any` 专用于与架构无关的纯文本、脚本、元数据、字体与文档包。

---

# 4. LMDB 零拷贝 ACID 状态存储引擎 (`libsage-db`)

## 4.1 LMDB 存储布局与表结构

数据库持久化存储于 `/var/lib/sage/data.mdb`，采用 LMDB B+ 树存储：

```
/var/lib/sage/
├── data.mdb                        # 主数据表 (Single-writer, Multi-reader mmap)
└── lock.mdb                        # LMDB 内部事务互斥锁
```

### 专用表 (Named DBI Tables)

| 表名 (DBI) | Key 格式 | Value 格式 | 功能与所有权模型 |
| :--- | :--- | :--- | :--- |
| `packages` | `pkg_name` (e.g. `ripgrep`) | 序列化的 `PackageManifest` (TOML) | 已安装包完整元数据、版本、通道、依赖、触发器 |
| `files` | `rel_path` (e.g. `usr/bin/rg`) | `pkg_name:channel\n...` (多所有权集合) | 快速文件冲突检测与反向文件归属查询 |
| `provides` | `symbol` (e.g. `so:libc.so.6`) | `pkg_name` | 虚拟提供者与 SONAME 动态符号反向索引 |
| `channels` | `channel_name` (e.g. `core`) | `ChannelConfig` | 已注册通道源、优先级、范围、挂载根 |
| `system` | `interface` (e.g. `virtual/init`) | `active_provider` (e.g. `openrc`) | 声明式系统核心提供者锁定 |
| `operations`| `op_id` (e.g. `txn-2026...`) | `FilesystemOperationRecord` | 未完成事务日志，用于系统宕机自动恢复 |

## 4.2 文件所有权集合与交接机制 (Ownership & Handover)

1.  **单所有权规则**：常规文件与符号链接在同一时刻仅允许存在一个所有权记号（`pkg_name:channel_name`）。
2.  **目录声明计数**：显式声明的目录在 `files` 表中按换行符累积多个包的所有权；卸载包时仅释放该包的所有权计数，直至最后一个拥有者卸载时才删除该目录记录。
3.  **事务内文件交接 (In-flight Handover)**：
    *   当单次大事务涉及包拆分（如 `foo` 升级拆分为 `foo` 与 `foo-libs`）时，旧包退还的文件与新包认领的文件在同一事务内发生交接。
    *   `check_file_conflicts` 引入 `released_claims: HashMap<Path, HashSet<PackageName>>`，允许认领在当前事务中被前置步骤释放的路径，消除虚假文件冲突。
4.  **孤儿文件回收 (`prune_orphaned_files`)**：在事务后自动清理拥有者包已不在 `packages` 表中的悬挂文件记录。

## 4.3 崩溃恢复状态机 (`operations` 表)

```
        开始事务
           │
           ▼
   [创建暂存目录并写入 Payload]
   [生成 versioned operations journal]
   [fsync 暂存目录与 journal]
           │
           ▼ (原子 LMDB 写入事务: packages + files + system + operations)
┌───────────────────────────────────────────────────────────┐
│ 状态: `filesystem_pending`                                 │
│ 意义: LMDB 元数据已持久化，文件正在向 Live 根目录发布     │
└─────────────────────────────┬─────────────────────────────┘
                              │
                              ▼ (执行发布: P F / P L / P D / R F / R D)
┌───────────────────────────────────────────────────────────┐
│ 状态: `postprocess_pending`                                │
│ 意义: 文件已完全到位，正在执行 post-transaction 触发器     │
└─────────────────────────────┬─────────────────────────────┘
                              │
                              ▼ (触发器、服务生成、Profile 链接全部成功)
┌───────────────────────────────────────────────────────────┐
│ 状态: (记录删除)                                          │
│ 意义: 事务完整终结，清理暂存目录                           │
└───────────────────────────────────────────────────────────┘
```

**恢复行为**：任何写操作启动时首先扫描 `operations` 表。若发现非空记录，校验 `journal_sha256` 完整性并根据当前 Phase 幂等前向重放，防止文件系统与 LMDB 数据库状态分叉。

---

# 5. 事务性流式归档与安全解包引擎 (`libsage-archive`)

## 5.1 归档包结构规范 (`*.pkg.tar.zst`)

```
pkgname-1.0.0-1-amd64.pkg.tar.zst
├── .METADATA/
│   ├── manifest.toml     # 包名称、版本、架构、依赖、Provides、Conflicts
│   ├── files.idx         # 逐文件相对路径、大小、mode、SHA-256 完整性摘要
│   ├── triggers.toml     # 路径匹配触发器 (ldconfig, ca-certificates 等)
│   └── service.toml      # 通用守护进程定义 (可选)
└── data/                 # 根文件系统载荷 (usr/bin/..., etc/...)
```

## 5.2 恒定代价元数据检视 (`inspect_package`)

流式读取归档前部，仅解压 `.METADATA/` 目录中的 TOML 与索引后立即终止解压，在 $O(1)$ 时间与极低内存开销下获取完整包信息。

## 5.3 基于 `openat` 与 `dirfd` 的防逃逸安全解包模型

为彻底杜绝路径遍历攻击（Path Traversal）与符号链接越权逃逸（Symlink Traversal Attack）：

```
 目标根目录 (sysroot) ──────── fd_root (通过 open(O_DIRECTORY | O_CLOEXEC) 打开)
       │
       ├─► 逐级校验并进入父目录 ─── fd_parent (通过 openat(fd_cur, sub, O_NOFOLLOW))
       │
       └─► 写入临时文件 ───────── fd_tmp (通过 openat(fd_parent, ".sage-tmp.XXX", O_CREAT | O_EXCL))
             │
             └─► 原子重命名 ────── renameat(fd_parent, ".sage-tmp.XXX", fd_parent, "final_name")
```

### 关键安全守则：
1.  **路径预检与规范化**：严禁包含 `..`、绝对路径或 `\0` 字节；严禁以 `data/` 外的非法前缀逃逸。
2.  **禁用跟踪符号链接**：所有目录查找均使用 `O_NOFOLLOW`。若中间路径为指向外部的符号链接，立即以安全错误中止。
3.  **TOCTOU 防御**：替换文件前对比 `(dev, ino, size)` 设备节点与 inode 号，防止竞争攻击。
4.  **Rayon 并行解压与写入**：对于多包全新安装，采用并发 Worker 线程池并行写出非冲突 Payload，在落地后单次调用 `syncfs` 确保物理持久化。

---

# 6. PubGrub / CDCL SAT 依赖求解器与因果诊断 (`libsage-solver`)

`libsage-solver` 实现完备的 PubGrub 依赖求解算法，结合冲突驱动子句学习 (CDCL) 机制：

```
                    ┌────────────────────────┐
                    │ 根请求 (Root Requests) │
                    └───────────┬────────────┘
                                │
                                ▼
                   ┌───────────────────────────┐
                   │  单位传播 (Unit Propagate) │◄─────────────────┐
                   └────────────┬──────────────┘                   │
                                │                                  │
                 ┌──────────────┴──────────────┐                   │
                 ▼                             ▼                   │
            【发现冲突】                  【无冲突，需决策】       │
                 │                             │                   │
                 ▼                             ▼                   │
       ┌───────────────────┐         ┌───────────────────┐         │
       │ 分析 1-UIP 冲突原因│         │ 挑选未决符号与候选│         │
       │ 学习 Incompatibility│        │ (Active优先>版本>名)│        │
       └─────────┬─────────┘         └─────────┬─────────┘         │
                 │                             │                   │
                 ▼                             ▼                   │
       ┌───────────────────┐         ┌───────────────────┐         │
       │ 回跳 (Backjump)    │         │ 决策赋值并推入 Trail │─────────┘
       └─────────┬─────────┘         └───────────────────┘
                 │ (若在 Level 0 冲突)
                 ▼
       ┌───────────────────┐
       │ 生成因果树诊断报告│
       │ (Unsolvable Error)│
       └───────────────────┘
```

## 6.1 核心数据结构

```rust
pub struct Term {
    pub symbol_name: String,
    pub constraint: Dependency,
    pub is_positive: bool, // true: 要求满足; false: 要求不满足
}

pub enum CauseKind {
    Root,
    Dependency,
    Conflict,
    Derived, // CDCL 学习得到的冲突子句
}

pub struct Incompatibility {
    pub terms: Vec<Term>,
    pub cause: CauseKind,
    pub custom_reason: Option<String>,
    pub previous_cause_a: Option<Arc<Incompatibility>>,
    pub previous_cause_b: Option<Arc<Incompatibility>>,
}

pub struct Assignment {
    pub term: Term,
    pub decision_level: usize,
    pub cause: Option<Arc<Incompatibility>>,
    pub decision_package: Option<PackageManifest>,
}
```

## 6.2 候选包打分与排序准则 (Candidate Ranking)
当为一个符号选择提供者时，按以下优先级从高到低排序：
1.  **声明式系统锁定优先**：`/etc/sage/system.toml` 中的 `[providers]`（如 `init = "openrc"`）加权 +1000。
2.  **包名完全匹配优先**：请求 `foo` 时，名为 `foo` 的包优先于声明 `provides = ["foo"]` 的其他包（+100）。
3.  **SONAME 纯净性**：对于 `so:libxxx.so` 符号，非 `-dev` 包优先于 `-dev` 开发包（+10）。
4.  **版本倒序**：高版本优先。
5.  **名称字母序**：确定性决胜。

## 6.3 MSRV (最低工具链版本) 局部优先解析
配方请求 `toolchain/gcc >= 14.2` 时：
1.  首先扫描本地 `/opt/channels/gcc/` 已安装插槽；若存在满足约束的版本（如 `15.3`），优先复用本地插槽，实现**零网络消耗**。
2.  仅当本地无可用插槽时，触发远端通道检索并拉取最小满足版本。

## 6.4 拓扑排序与安装序列化
求解完成后，通过带染色（0=未访问, 1=访问中, 2=已完成）的深度优先搜索 (DFS)，生成**依赖前置**的确定性拓扑安装序列。

---

# 7. 多层 Channel 运行时与 FHS Profile 聚合器 (`libsage-sys::channel`)

## 7.1 多层 Channel 架构与路径映射

```
                                 / (FHS System Root)
                                 ├── usr/
                                 │   ├── bin/
                                 │   ├── lib/
                                 │   └── lib/runtimes/        <── Runtime Scope
                                 │       ├── python/3.12/
                                 │       └── cuda/12.4/
                                 ├── opt/channels/            <── Toolchain Scope
                                 │   ├── llvm/22/
                                 │   └── gcc/15/
                                 ├── etc/sage/profiles/
                                 │   └── default/             <── Active Profile Aggregator
                                 │       ├── bin/ (symlinks)
                                 │       └── lib/ (symlinks)
                                 └── home/user/.local/        <── User Scope
```

### 子通道寻址语法标准:
$$\text{Channel Spec} := \langle\text{Scope}\rangle/\langle\text{Category}\rangle:\langle\text{Slot}\rangle$$

*   `toolchain/llvm:22` $\rightarrow$ 物理路径 `/opt/channels/llvm/22`
*   `toolchain/gcc:15` $\rightarrow$ 物理路径 `/opt/channels/gcc/15`
*   `runtime/python:3.12` $\rightarrow$ 物理路径 `/usr/lib/runtimes/python/3.12`
*   `runtime/cuda:12.4` $\rightarrow$ 物理路径 `/usr/lib/runtimes/cuda/12.4`

## 7.2 Profile 聚合器与原子软链接切换 (`ProfileEngine`)

1.  **活动链接目录**：`/etc/sage/profiles/default/`。
2.  **原子切换**：通过构建临时影子目录并在同文件系统执行 `rename` 实现 $O(1)$ 无缝切换编译器与运行时版本。
3.  **Shell 环境变量桥接 (`/etc/profile.d/sage-channels.sh`)**：
    *   自动将 `/etc/sage/profiles/default/bin` 注入 `PATH` 前端。
    *   自动识别活动 Java 运行时，导出 `JAVA_HOME=/etc/sage/profiles/default/runtimes/java`。

## 7.3 临时沙箱隔离环境 (`sage shell`)

支持开发者即时创建包含任意多工具链/运行时的隔离 Shell 会话，不污染全局环境：

```bash
sage shell --with toolchain/llvm:22 --with runtime/python:3.12 --with runtime/cuda:12.4
```

### 动态环境合成：
*   `PATH`: `/opt/channels/llvm/22/bin:/usr/lib/runtimes/python/3.12/bin:/usr/lib/runtimes/cuda/12.4/bin:$PATH`
*   `LD_LIBRARY_PATH`: 对应各层 `lib/` 路径聚合。
*   `CPATH` / `PKG_CONFIG_PATH`: 头文件与 pkgconfig 元数据路径聚合。
*   `CC=clang`, `CXX=clang++`。

---

# 8. 通用服务声明与多 Init 生成引擎 (`libsage-sys::service`)

## 8.1 Universal `service.toml` (Schema v2) 规范

```toml
schema_version = 2

[service]
name = "sshd"
description = "OpenSSH Server Daemon"
command = ["/usr/sbin/sshd", "-D"]
stop_command = []
reload_command = ["/bin/kill", "-HUP", "$MAINPID"]
user = "root"
group = "root"
working_dir = "/"
pid_file = "/run/sshd.pid"
restart = "always"      # "always", "on-failure", "no"
type = "simple"         # "simple", "forking"
after = ["net", "syslog"]
before = []
runtime = ""            # e.g. "runtime/java:openjdk-21"
```

## 8.2 多 Init 目标自动生成映射表

| 目标 Init 系统 | 生成目标路径 | 生成模式与安全约束 |
| :--- | :--- | :--- |
| **Loom** | `/usr/lib/loom/services/<name>.toml` | 调用 `/usr/lib/loom/loom compile-service` 进行严谨转换；全量生成后调用 `loom validate --root` 进行全局依赖拓扑验证 (Fail-closed)。 |
| **OpenRC** | `/etc/init.d/<name>` | 渲染为 `#!/usr/bin/openrc-run` 脚本，权限 `0755`，支持 `need` / `before` 依赖转换与 `command_user`。 |
| **Runit** | `/etc/sv/<name>/run` & `finish` | 渲染为包含 `chpst -u user:group` 的可执行 Shell 脚本，权限 `0755`。 |
| **Systemd** | `/usr/lib/systemd/system/<name>.service` | 渲染为标准 INI 格式单元文件 (`[Unit]`, `[Service]`, `[Install]`)，权限 `0644`。 |
| **Dinit** | `/etc/dinit.d/<name>` | 渲染为 Dinit 声明式服务配置文件 (`type = process`, `depends-on = ...`)，权限 `0644`。 |
| **s6** | `/etc/s6/services/<name>/run` | 渲染为基于 `s6-setuidgid` 的服务运行脚本，权限 `0755`。 |

## 8.3 运行时服务环境绑定 (Runtime Service Binding)
当服务声明 `runtime = "runtime/java:openjdk-21"` 时，代码生成器自动在服务启动脚本/单元文件中注入局部 `JAVA_HOME` 与 `PATH`，无需依赖宿主全局 Java 版本。

---

# 9. 事务后触发器、Sysusers 与 Alternatives 仲裁 (`libsage-sys::triggers`)

## 9.1 内置与声明式触发器体系

触发器在文件系统写完且 LMDB 事务提交后批量聚合执行：

1.  **内置核心触发器 (基于修改路径模式自动激活)**：
    *   `ldconfig`: 触碰 `usr/lib/*.so*` 时自动执行 `/sbin/ldconfig`。
    *   `ca-certificates`: 触碰 `etc/ssl/certs/*` 或 `usr/share/ca-certificates/*` 时执行。
    *   `mime-database`: 触碰 `usr/share/mime/*` 时执行。
    *   `glib-schemas`: 触碰 `usr/share/glib-2.0/schemas/*` 时执行。
    *   `desktop-database`: 触碰 `usr/share/applications/*` 时执行。
    *   `icon-cache`: 触碰 `usr/share/icons/*` 时执行。
    *   `font-cache`: 触碰 `usr/share/fonts/*` 时执行。
    *   `depmod`: 触碰 `usr/lib/modules/<version>/*` 时，通过 `virtual/depmod` 提供者（如 kmod）执行。
2.  **包声明触发器 (`triggers.toml`)**：项目专属钩子，支持 `on_paths` 匹配与 `exec` 执行。

## 9.2 声明式系统用户与组 (`sysusers`)

包配方中声明 `[[sysusers]]`：
```toml
[[sysusers]]
type = "user"
name = "redis"
id = 75
description = "Redis Database Server"
home = "/var/lib/redis"
shell = "/usr/bin/nologin"
```
*   `libsage` 在安装阶段自动注入 `usr/lib/sysusers.d/<pkg>.conf`，并在事务后安全调用 `systemd-sysusers` 或原生 POSIX `useradd` / `groupadd` 创建对应账户。

## 9.3 软链接替代项优先级仲裁 (`alternatives`)

当多个包提供相同命令（如 `vi`、`cc`、`awk`）时，声明 `[[alternatives]]`：
```toml
[[alternatives]]
link = "usr/bin/vi"
target = "vim"
priority = 50
```
*   `libsage` 维护全局最高优先级者指向，当卸载当前最高优先级包时，自动原子降级链接至次高优先级的已安装提供者。

---

# 10. 声明式系统 Reconcile 与原子状态切换 (`libsage-sys::rebuild`)

`sage rebuild` 是系统状态的终极调和器，其执行流水线如下：

```
                      读取 /etc/sage/system.toml
                                  │
                                  ▼
                     对比 LMDB packages & system 表
                                  │
                                  ▼
               ┌───────────────────────────────────────┐
               │ 规划状态迁移差异 (Plan Diff)          │
               │ • 虚拟提供者替换 (如 sysvinit -> loom)│
               │ • 待卸载旧包集合 & 待安装新包集合     │
               └──────────────────┬────────────────────┘
                                  │
                                  ▼
               ┌───────────────────────────────────────┐
               │ PubGrub SAT 联合依赖求解              │
               │ • 验证状态切换后的全局数学可满足性    │
               └──────────────────┬────────────────────┘
                                  │
                                  ▼
               ┌───────────────────────────────────────┐
               │ 两阶段事务执行与原子文件交接          │
               │ • 暂存与校验新 Payload                │
               │ • LMDB 原子提交新元数据与新所有权     │
               │ • 发布新文件并回收被弃用的旧文件      │
               └──────────────────┬────────────────────┘
                                  │
                                  ▼
               ┌───────────────────────────────────────┐
               │ 服务重新生成 (Service Re-generation)  │
               │ • 清理旧 Init 服务定义                │
               │ • 编译生成新 Init 的全量服务脚本      │
               │ • Loom 下执行 fail-closed 全图验证    │
               └──────────────────┬────────────────────┘
                                  │
                                  ▼
               ┌───────────────────────────────────────┐
               │ 执行事务后触发器 (Triggers Execution) │
               │ • 更新 ld.so 缓存、证书库与内核模块   │
               └───────────────────────────────────────┘
```

---

# 11. 密闭构建沙箱、工具链审计与配方引擎 (`libsage-build`)

## 11.1 工具链策略规范 (`/etc/sage/build.toml`)

```toml
schema_version = 1
fakeroot = "fakeroot"            # 必须存在的 fakeroot 命令路径
sysroot = "/"                    # 构建暴露的只读基础环境
cc = "clang"
cxx = "clang++"
linker = "ld.lld"              # lld | mold | ld
fallback_cc = "gcc"
fallback_cxx = "g++"
fallback_linker = "ld"
rustc = "rustc"
cflags = "-O3 -march=x86-64-v3 -pipe"
cxxflags = ""                 # 为空时自动继承 cflags
cppflags = ""
ldflags = "-Wl,--as-needed -Wl,-O1"
rustflags = "-C target-cpu=x86-64-v3"
source_date_epoch = 1700000000
jobs = 0                         # 0 = 自动匹配在线 CPU 线程数
compile_jobs = 0                 # 单包编译并发度
compiler_cache = "auto"          # none | auto | ccache | sccache
ccache_dir = "/var/cache/sage/ccache"
memory_limit = ""                # Cgroups v2 内存限制 (例如 "8G")
pids_limit = 2048                # Cgroups v2 最大进程数
```

## 11.2 Recipe v2 声明式规范与多后端适配器

```toml
schema_version = 2

[package]
name = "ripgrep"
version = "14.1.0"
release = "1"
description = "Fast line-oriented search tool"
license = "MIT OR Unlicense"
channel = "system"
arch = "amd64"
dependencies = ["virtual/libc", "so:libpcre2-8.so.0"]
build_dependencies = ["cmake", "ninja"]
check_dependencies = ["pkg-config >= 2.0"]

[source]
url = "https://github.com/BurntSushi/ripgrep/archive/14.1.0.tar.gz"
sha256 = "33c616959def5f80a763a51cf1feed8c8ea9db583556862e3c6a84fa42f95499"

[[vendor]]
url = "https://example.com/ripgrep-vendor-14.1.0.tar.zst"
sha256 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
target = "vendor"

[build]
system = "cargo"                 # cmake | meson | xmake | cargo | go | autotools | make | script
payload = "all"                  # all | allowlist | outputs
build_targets = ["--features", "pcre2"]
install_targets = ["--bin", "rg"]
allowed_compilers = ["clang", "gcc"]
allowed_linkers = ["lld", "mold"]
```

### 多后端支持矩阵：
*   **`cmake`**: 自动注入 `-DCMAKE_INSTALL_PREFIX=/usr`, `-DCMAKE_BUILD_TYPE=Release`, Ninja 生成器与编译器标志。
*   **`meson`**: 自动注入 `meson setup --prefix=/usr --libdir=lib` 与编译/安装命令。
*   **`xmake`**: 自动传递 `--cc`, `--cxx`, `--ld` 与标志配置。
*   **`cargo`**: 自动注入 `RUSTFLAGS`, `--locked`, 离线 Crates 替换与 `DESTDIR` 安装。
*   **`go`**: 自动配置 `GOBIN`, `GOTOOLCHAIN=local`, `-mod=vendor` 离线编译。
*   **`autotools`**: 自动注入 `--prefix=/usr --libdir=/usr/lib --sysconfdir=/etc` 与并行 Make。
*   **`make`**: 自动识别 `kernel = true`（Linux Kbuild 系统），智能映射 `LLVM=1`, `KCFLAGS`, `KRUSTFLAGS`。
*   **`script`**: 结构化执行步骤，支持 `tools = true` 声明以接入受控编译器审计。

## 11.3 密闭沙箱与 Ptrace/Seccomp 工具链审计

```
┌─────────────────────────────────────────────────────────────┐
│ Fakeroot + Bubblewrap (bwrap) 密闭沙箱                      │
│ • 只读挂载 sysroot 作为 /                                   │
│ • 读写挂载专有源码目录 (source)、构建目录 (build)、DESTDIR   │
│ • 清空环境变量: LC_ALL=C, TZ=UTC, SOURCE_DATE_EPOCH=固定值   │
│ • 隔离网络命名空间 (--unshare-net, 除非 network = true)     │
└──────────────────────────────┬──────────────────────────────┘
                               │
                               ▼
┌─────────────────────────────────────────────────────────────┐
│ Ptrace + Seccomp 监督进程 (Audit Supervisor)                │
│ • 拦截并追踪子进程的 `execve` 与 `execveat` 系统调用        │
│ • 验证执行的二进制文件是否属于 /etc/sage/build.toml 授权集合 │
│ • 记录工具执行次数与版本号，写入 `[[managed_build_tools]]`  │
│ • 若发现未授权的外部编译器执行，立即阻断并判定构建失败      │
└─────────────────────────────────────────────────────────────┘
```

## 11.4 确定性产物过滤与变换 (Payload Transforms)
*   **`install_copies` / `install_symlinks` / `install_moves` / `install_removes` / `install_generates`**: 声明式重塑产物目录结构。
*   **`payload = "allowlist"`**: 结合 Glob 模式精确控制打包文件列表（彻底防止拆分包内容交叉污染）。
*   **`[build.content]` 策略**: 自动执行 `strip`、Deterministic `man_compress = "gzip"`、`shebangs = "absolute"` 重写与未授权语言包 (`locales`) 剪裁。
*   **自动化 ELF 扫描器**: 零外部依赖扫描所有 ELF 动态二进制文件，提取 `DT_SONAME`（自动填充 `provides = ["so:libxxx.so"]`）与 `DT_NEEDED`（自动填充 `dependencies = ["so:libyyy.so"]`）。
*   **两阶段可再现性验证 (Bit-for-Bit Repro Pass)**：构建完成后在干净环境再次构建并比对 SHA-256 校验和。

---

# 12. 远程仓库同步、Ed25519 签名与分块下载 (`libsage-repo`)

## 12.1 软件源索引规范 (`index.toml` v1)

```toml
schema_version = 1
channel = "core"
architecture = "amd64"
generated_at = 1700000000

[[packages]]
name = "ripgrep"
version = "14.1.0"
release = 1
epoch = 0
arch = "amd64"
installed_size = 5384912
download_size = 1845120
sha256 = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
filename = "ripgrep-14.1.0-1-amd64.pkg.tar.zst"
dependencies = ["virtual/libc", "so:libpcre2-8.so.0"]
provides = ["ripgrep", "rg"]
```

## 12.2 Ed25519 签名验证体系
每个软件源必须提供 `index.toml.sig`。`libsage-repo` 内置 Ed25519 验签器，基于 `/etc/sage/keys/` 中的信任公钥对远端索引进行离线与在线验证，阻断恶意镜像篡改。

## 12.3 异步并行流式下载器 (`DownloadEngine`)
*   基于 `reqwest` / `hyper` 异步运行时。
*   支持 HTTP `Range` 头分块并发下载大包。
*   多镜像源延迟竞速探测与自动故障转移 (Mirror Failover)。
*   边下载边计算 SHA-256 摘要，并在内存中进行流式解压前置验证。

---

# 13. 全局主机锁与 Dry-Run 零写安全保证 (`libsage-core::lock`)

## 13.1 全局互斥锁规范 (`/run/sage/operation.lock`)

1.  **路径与权限**：宿主根目录 `/run/sage/` 目录模式 `0700`，锁文件 `/run/sage/operation.lock` 模式 `0600`，属主必须为 `root:root`。
2.  **锁粒度**：
    *   **只读与 Dry-Run 预览**：获取共享文件锁 (`LOCK_SH`)。
    *   **状态写操作 (Install, Remove, Rebuild)**：获取独占排他文件锁 (`LOCK_EX`)。
3.  **RAII 守卫**：锁持有贯穿 CLI 启动至 LMDB 句柄完全释放的整个生命周期。

## 13.2 Dry-Run 零写保证 (Zero-Write Invariant)
在 `--dry-run` 模式下：
*   LMDB 以 `MDB_RDONLY | MDB_NOLOCK` 模式打开（完全避免创建 `lock.mdb` 写操作）。
*   若目标根目录数据库文件不存在，直接在内存中建立虚拟空状态，禁止在目标磁盘创建任何文件或目录。
*   远端索引仅在内存中解析，禁止写入本地磁盘缓存。

---

# 14. 前端架构：CLI 命令行与 Ratatui TUI (`sage`)

## 14.1 CLI 命令语法设计 (Clap Derive)

```
sage [GLOBAL_OPTIONS] <COMMAND> [ARGS...]

全局选项 (Global Options):
  -v, --verbose           启用详细调试输出与因果树展开
  -q, --quiet             静默模式，仅输出严重错误
      --dry-run           事务演练模式，禁止持久化写入
      --root <PATH>       指定目标根目录 (默认 "/")
      --wait[=SECONDS]    若存在锁竞争，等待指定秒数
  -h, --help              输出帮助信息
  -V, --version           输出版本号

核心子命令 (Commands):
  install [PKGS...]       求解依赖并安装指定包 (支持 --channel)
  remove  [PKGS...]       安全卸载包 (支持级联卸载与反向依赖保护)
  rebuild                 根据 /etc/sage/system.toml 调和系统状态
  channel                 管理软件源与通道 (list/add/remove/sync)
  toolchain               管理隔离工具链 (list/use/install/remove)
  shell                   启动包含指定 toolchains/runtimes 的临时环境
  service                 检查与手动生成 Init 服务配置 (list/status/generate)
  build <RECIPE_DIR>      基于 recipe.toml 构建 *.pkg.tar.zst 包
  query                   LMDB 极速状态查询 (installed/info/files/owner)
  verify                  校验已安装文件的 SHA-256 完整性
  tui                     启动全屏交互式终端管理控制台
```

## 14.2 Ratatui TUI 控制台架构

TUI 模式提供直观、现代化的终端交互管理界面：

```
┌─────────────────────────────────────────────────────────────────────────────┐
│ SAGE PACKAGE MANAGER (Rust Edition)             [Target: /] [Mode: ACID-RO] │
├───────────────────┬─────────────────────────────────────────────────────────┤
│ [F1] Installed    │ Package: ripgrep 14.1.0-1 (amd64)                       │
│ [F2] Available    │ Scope:   system (channel: core)                         │
│ [F3] Channels     │ License: MIT OR Unlicense                               │
│ [F4] Services     │ Size:    5.13 MiB                                       │
│ [F5] Reconcile    │ Description: Fast line-oriented search tool             │
│ [F6] Build Mon    ├─────────────────────────────────────────────────────────┤
│                   │ Dependencies:                                           │
│ ❯ ripgrep         │  • virtual/libc (satisfied by glibc 2.40)                │
│   neovim          │  • so:libpcre2-8.so.0 (satisfied by pcre2 10.42)        │
│   clang-20        ├─────────────────────────────────────────────────────────┤
│   gcc-15          │ Files (4 entries):                                      │
│   python-3.12     │  usr/bin/rg                                             │
│   openrc          │  usr/share/man/man1/rg.1                                │
│   systemd-udev    │  usr/share/doc/ripgrep/README.md                        │
├───────────────────┴─────────────────────────────────────────────────────────┤
│ [Tab] 切换面板  [Enter] 详情  [I] 安装  [D] 卸载  [R] Rebuild  [Q] 退出      │
└─────────────────────────────────────────────────────────────────────────────┘
```

### TUI 模块架构:
*   `tui::app`: 应用程序状态机、路由与焦点管理。
*   `tui::views::packages`: 已安装与远端包浏览、全文模糊搜索（Fuzzy Match）。
*   `tui::views::reconcile`: 声明式系统差异可视化对比器，直观展示提供者切换与待生成功服务。
*   `tui::views::channels`: 多层 Channel 启用、插槽切换与激活控制。
*   `tui::views::builder`: 本地构建实时进度监控，展示 Ptrace 审计事件流与构建日志。

---

# 15. C-FFI 与动态链接兼容层 (libalpm 生态位)

为了使 `libsage` 能够无缝作为底层包管理库嵌入到第三方工具、C/C++ 应用程序及外部安装器中，`libsage` 导出符合 C-ABI 的头文件与动态库：

```rust
// libsage/src/ffi.rs

#[repr(C)]
pub struct sage_handle_t {
    _private: [u8; 0],
}

#[repr(C)]
pub struct sage_pkg_t {
    _private: [u8; 0],
}

#[no_mangle]
pub unsafe extern "C" fn sage_initialize(
    root: *const libc::c_char,
    dbpath: *const libc::c_char,
    error: *mut *mut libc::c_char,
) -> *mut sage_handle_t;

#[no_mangle]
pub unsafe extern "C" fn sage_trans_init(
    handle: *mut sage_handle_t,
    flags: u32,
    error: *mut *mut libc::c_char,
) -> libc::c_int;

#[no_mangle]
pub unsafe extern "C" fn sage_trans_add_pkg(
    handle: *mut sage_handle_t,
    pkg_name: *const libc::c_char,
) -> libc::c_int;

#[no_mangle]
pub unsafe extern "C" fn sage_trans_commit(
    handle: *mut sage_handle_t,
    error: *mut *mut libc::c_char,
) -> libc::c_int;

#[no_mangle]
pub unsafe extern "C" fn sage_trans_release(handle: *mut sage_handle_t) -> libc::c_int;

#[no_mangle]
pub unsafe extern "C" fn sage_cleanup(handle: *mut sage_handle_t);
```

---

# 16. Rust 重写工程规范与落地路线图

## 16.1 Rust 编码安全与工程规范

1.  **零非必要 `unsafe`**：除了底层系统调用（`openat`, `ptrace`, `seccomp`, `bwrap` 交互）与 C-FFI 接口外，业务逻辑层 100% 采用 Safe Rust。
2.  **错误处理规范**：
    *   底层库（`libsage-*`）统一采用 `thiserror` 定义强类型枚举错误。
    *   顶层前端（`sage`）采用 `miette` 或 `anyhow` 输出带有代码行指示与建议的美观诊断信息。
3.  **零死锁与零资源泄漏**：严格遵循 RAII 模式管理文件描述符、LMDB 事务读写锁与沙箱进程管道。
4.  **性能基准**：依赖求解与 LMDB 查询保持微秒级响应；100 个包的批量解压与元数据登记耗时严控在 15 秒以内。

## 16.2 落地演进路线图

```
┌──────────────────────────────────────────────────────────────────────────┐
│ 阶段一 (Core & Storage):                                                 │
│ • 构建 libsage-core (版本代数、Schema 模型、通用工具)                    │
│ • 构建 libsage-db (LMDB 封装、所有权与两阶段恢复状态机)                 │
├──────────────────────────────────────────────────────────────────────────┤
│ 阶段二 (Archive & Solver):                                               │
│ • 构建 libsage-archive (tar.zst 流式读写、openat 安全解包、日志引擎)     │
│ • 构建 libsage-solver (PubGrub CDCL SAT 求解器与因果诊断树)              │
├──────────────────────────────────────────────────────────────────────────┤
│ 阶段三 (System Engine):                                                  │
│ • 构建 libsage-sys (Channel 运行时、Reconcile 调和、Service 多 Init 生成)│
│ • 实现 Triggers 触发器、Sysusers 与 Alternatives 仲裁                    │
├──────────────────────────────────────────────────────────────────────────┤
│ 阶段四 (Build & Sandbox):                                                │
│ • 构建 libsage-build (bwrap 沙箱、ptrace/seccomp 审计、Recipe v2 构建器) │
│ • 自动化 ELF DT_NEEDED / DT_SONAME 扫描器                                │
├──────────────────────────────────────────────────────────────────────────┤
│ 阶段五 (Repo & Network):                                                 │
│ • 构建 libsage-repo (Ed25519 验签、异步并行下载器、镜像竞速)             │
├──────────────────────────────────────────────────────────────────────────┤
│ 阶段六 (Frontend & FFI):                                                 │
│ • 整合 libsage 统一门面与 C-FFI                                          │
│ • 开发 sage CLI (Clap) 与 sage TUI (Ratatui)                             │
│ • 运行全量回归与架构验收测试套件                                         │
└──────────────────────────────────────────────────────────────────────────┘
```

---
*文档编制完成。本规范为 Sage 3.0 Rust 重写的唯一最高技术指导基线。*
