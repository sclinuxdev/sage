# 规范: 配方目录层级与分类体系 (`recipes/` v1)

- **根目录路径**: `recipes/`
- **参考蓝本**: `/home/ir/newworld/recipes`
- **核心原则**: **各架构严格独立维护 (Strict Per-Architecture Independence)**。每个支持的架构拥有完全自包含的独立配方目录，严禁跨架构隐式继承与回退。

---

## 1. 架构严格独立维护目录树

所有软件包必须在目标架构目录下拥有专属的配方与补丁，彻底隔离不同硬件架构的打包修订号 (`release`)、编译参数与补丁集合：

```text
recipes/
└── <category>/                               # 一级分类目录 (devel, lib, net, security, system, text, tools, utils)
    └── <pkgname>/                            # 软件包名称 (e.g. gcc, binutils, zlib, libarchive)
        ├── amd64/                            # x86-64 架构独立维护目录
        │   └── <pkgname>-<version>-<release>/# 专属版本与打包修订目录 (e.g. gcc-16.2.0-1)
        │       ├── recipe.toml               # amd64 专属配方 (含专用 flags 与依赖)
        │       └── patches/                  # amd64 专属补丁 (可选)
        │           └── fix-avx512-build.patch
        │
        ├── aarch64/                          # ARM64 架构独立维护目录
        │   └── <pkgname>-<version>-<release>/# 专属版本与修订目录 (e.g. gcc-16.2.0-3)
        │       ├── recipe.toml               # aarch64 专属配方
        │       └── patches/                  # aarch64 专属补丁 (可选)
        │
        ├── riscv64/                          # RISC-V 架构独立维护目录 (按需维护)
        │   └── <pkgname>-<version>-<release>/
        │       └── recipe.toml
        │
        └── any/                              # 架构无关包独立目录 (文档、字体、纯 Python 库)
            └── <pkgname>-<version>-<release>/# e.g. jinja2-3.1.6-2, meson-1.12.0-2
                └── recipe.toml
```

---

## 2. 为什么各架构必须单独维护？

1. **修订号独立演进 (`release`)**:
   - 当 ARM64 架构因特定的内存对齐或汇编修复需要重新发版时，仅需递增 `aarch64` 下的 `release`（如 `gcc-16.2.0-3`），`amd64`（`gcc-16.2.0-1`）不受任何扰动。
2. **构建参数与工具链解耦**:
   - 各架构的编译标志（`CFLAGS`）、内联汇编开关（`--enable-lto`、`--with-arch=...`）以及专属配置选项彼此独立，配方内无需编写复杂的 `if-arch` 条件分支。
3. **补丁隔离**:
   - 某架构专有的硬件修复补丁仅存放于该架构目录下的 `patches/` 中，零污染其他架构。
4. **架构无关包归宿 (`any`)**:
   - 纯 Python 模块（如 `jinja2`）、Autoconf/Automake 脚本、字体等通用无二进制编译的包，统一定义在 `any/<pkgname>-<version>-<release>/` 目录下，所有架构客户端均可直接复用构建。

---

## 3. 标准一级分类体系 (Category Taxonomy)

参考 `newworld` 蓝本，Sage 设立 8 大标准分类：

| 分类 (Category) | 范围与职责 | 典型软件包示例 |
| :--- | :--- | :--- |
| **`devel`** | 编译器、链接器、解释器、构建系统与调试器 | `gcc`, `clang`, `llvm`, `cmake`, `ninja`, `python`, `binutils`, `m4` |
| **`lib`** | 通用基础运行时动态库、压缩库、FFI 库 | `zlib`, `zstd`, `libarchive`, `libffi`, `libcap`, `ncurses`, `gmp` |
| **`net`** | 网络协议栈、客户端、守护进程与网络管理工具 | `openssh`, `curl`, `dhcpcd`, `iproute2`, `wget`, `bind-utils` |
| **`security`** | 加密库、鉴权认证模块、权限管理 | `openssl`, `ca-certificates`, `shadow`, `sudo`, `pam`, `libxcrypt` |
| **`system`** | 操作系统核心基石、Init 系统、C 运行库、设备管理 | `glibc`, `musl`, `openrc`, `systemd`, `eudev`, `kmod`, `coreutils` |
| **`text`** | 文本流处理、行编辑器、语法分析与全文检索 | `gawk`, `sed`, `grep`, `diffutils`, `less`, `vim`, `ripgrep` |
| **`tools`** | 磁盘与文件系统工具、硬件检测、打包压缩工具 | `e2fsprogs`, `btrfs-progs`, `tar`, `xz`, `gzip`, `pciutils` |
| **`utils`** | 进程管理、系统监控、终端工具集 | `procps-ng`, `util-linux`, `findutils`, `which`, `psmisc` |
