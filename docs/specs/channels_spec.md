# 规范: 通道与软件源配置 (`/etc/sage/channels.toml` v1)

- **文件路径**: `/etc/sage/channels.toml`
- **Schema 版本**: `1`
- **设计目标**: 定义软件源层级拓扑、**默认带版本的子通道 (Versioned Sub-channels)** 与多版本物理共存模型。

---

## 1. 核心命名规范：子通道默认自带包版本

为了在架构上天然支持多版本共存，**子通道名称默认携带对应主语言或工具链的主版本号（Version/Slot）**：
- Python 子通道规范：`python3.12`、`python3.13`、`python3.14` ...
- GCC 工具链规范：`gcc14`、`gcc15` ...
- LLVM 工具链规范：`llvm19`、`llvm20` ...
- Rust 工具链规范：`rust1.97`、`rust1.98` ...

各版本子通道拥有完全独立的物理基准目录 `/opt/channels/<subchannel>/`，使得不同大版本的 Python 运行环境或编译器套件在系统中**天然并存、互不污染**。

---

## 2. 规范示例

```toml
schema_version = 1

# 系统主软件源 (Root Channel)
[channels.main]
url = "https://mirror.example.org/sage"
priority = 100
signing_key = "/etc/sage/keys/sage-main.pub"
enabled = true

# 系统主通道 (安装至 /)
[channels.main.subchannels.system]
scope = "system"
target_root = "/"
enabled = true

# Python 3.12 独立子通道
[channels.main.subchannels.python312]
alias = "python3.12"
type = "python"
scope = "runtime"
target_root = "/opt/channels/python3.12/site-packages"
enabled = true

# Python 3.13 独立子通道 (与 3.12 完全共存)
[channels.main.subchannels.python313]
alias = "python3.13"
type = "python"
scope = "runtime"
target_root = "/opt/channels/python3.13/site-packages"
enabled = true

# Python 3.14 independent subchannel
[channels.main.subchannels.python314]
alias = "python3.14"
type = "python"
scope = "runtime"
target_root = "/opt/channels/python3.14/site-packages"
enabled = true

# GCC 14 工具链子通道
[channels.main.subchannels.gcc14]
type = "toolchain"
scope = "toolchain"
target_root = "/opt/channels/gcc14"
enabled = true

# GCC 15 工具链子通道 (与 14 完全共存)
[channels.main.subchannels.gcc15]
type = "toolchain"
scope = "toolchain"
target_root = "/opt/channels/gcc15"
enabled = true

# GCC 16 toolchain subchannel
[channels.main.subchannels.gcc16]
type = "toolchain"
scope = "toolchain"
target_root = "/opt/channels/gcc16"
enabled = true

# LLVM 22 toolchain subchannel
[channels.main.subchannels.llvm22]
type = "toolchain"
scope = "toolchain"
target_root = "/opt/channels/llvm22"
enabled = true

# Rust 1.98 toolchain subchannel
[channels.main.subchannels.rust198]
alias = "rust1.98"
type = "toolchain"
scope = "toolchain"
target_root = "/opt/channels/rust1.98"
enabled = true
```

Compiler, linker, Rust, and Python packages belong to their versioned
subchannels. A recipe dependency may use a short alias such as
`gcc16/gcc:16` or `python3.14/python:3.14`; after the repository root is
known, Sage canonicalizes it to `main/gcc16` or `main/python3.14`.

When a dependency names a provider symbol rather than a concrete package, Sage
keeps the same channel scope and resolves the declared provider releases. A
concrete package key wins when both the key and a provider symbol are present.

---

## 3. 多版本共存安装与 CLI 交互

用户可指定精确的版本化子通道进行依赖安装或调和：

```bash
# 仅为 Python 3.12 环境安装 numpy
sage install --channel python3.12 python-numpy

# 仅为 Python 3.13 环境安装 numpy
sage install --channel python3.13 python-numpy

# 同时为多个 Python 版本安装不同版本的 requests
sage install --channel python3.12 python-requests
sage install --channel python3.13 python-requests
```

---

## 4. 物理隔离与数据库多版本模型

- **物理存储路径**:
  - `python3.12` 的模块：`/opt/channels/python3.12/site-packages/...`
  - `python3.13` 的模块：`/opt/channels/python3.13/site-packages/...`
  - `gcc14` 的编译器：`/opt/channels/gcc14/bin/gcc`
  - `gcc15` 的编译器：`/opt/channels/gcc15/bin/gcc`
- **数据库唯一键**:
  - `(main/python3.12, numpy, 0)` 与 `(main/python3.13, numpy, 0)` 为两个完全正交的数据库实体，分别跟踪其独立的文件所有权与版本生命周期。
