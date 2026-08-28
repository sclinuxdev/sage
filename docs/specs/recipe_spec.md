# 规范: 包配方与单配方多包拆分 (`recipe.toml` v1)

- **文件位置**: `recipes/<category>/<pkgname>/<arch>/<pkgname>-<version>-<release>/recipe.toml` (例如 `recipes/devel/gcc/amd64/gcc-16.2.0-1/recipe.toml`)
- **Schema 版本**: `1`
- **核心目标**: 支持单次密闭编译、零重复构建，并依据 Glob 规则声明式**将产物拆分为多个独立子包** (如 `bin`, `libs`, `dev`, `doc`)。

---

## 1. 单配方拆分多包机制 (Single-Recipe Multi-Subpackages)

在传统构建中，拆分主程序、运行库与开发头文件往往需要多次重复编译。Sage 引入声明式 **`[[subpackages]]`** 切分机制：

```text
               ┌──────────────────────────────────────────────┐
               │    源码编译与安装至统一 DESTDIR (单次编译)     │
               └──────────────────────┬───────────────────────┘
                                      │
               ┌──────────────────────┴───────────────────────┐
               │         sage-build Payload 声明式切分         │
               ├──────────────────────┬───────────────────────┤
               │                      │                       │
               ▼                      ▼                       ▼
    [[subpackages]] (libs)  [[subpackages]] (dev)    [package] (主包)
    files = ["usr/lib/*.so.*"]  files = ["usr/include/**",  剩余未切分文件
                            "usr/lib/*.so", "pkgconfig"] (如 usr/bin/...)
               │                      │                       │
               ▼                      ▼                       ▼
     *.pkg.tar.zst          *.pkg.tar.zst          *.pkg.tar.zst
```

### 切分与隔离守则：
1. **优先切分**: `[[subpackages]]` 按声明顺序从 `DESTDIR` 中认领匹配的文件并移入各自的归档暂存区。
2. **零文件冲突与无交叉污染**: 被子包认领的文件自动从主包文件池中剔除，确保生成的多个 `*.pkg.tar.zst` 之间**文件集合严格互斥**。
3. **独立 ELF 符号扫描**: 每个子包独立运行 `ElfScanner`，例如主包工具自动生成对 `libs` 子包导出的 `so:libfoo.so` 的依赖。

---

## 2. 完整实战配方示例 (`libarchive` 多包拆分)

```toml
schema_version = 1

# 主软件包 (包含可执行命令工具: bsdtar, bsdcpio)
[package]
name = "libarchive"
version = "3.8.9"
release = 1
epoch = 0
description = "Multi-format archive and compression library tools"
license = "BSD-2-Clause AND BSD-3-Clause"
channel = "system"
arch = "amd64"

dependencies = [
    "libarchive-libs >= 3.8.9",
    "virtual/libc"
]

[source]
url = "https://github.com/libarchive/libarchive/releases/download/v3.8.9/libarchive-3.8.9.tar.xz"
sha256 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

[build]
inherit = ["cmake"]

[build.args]
cmake_args = "-DENABLE_TEST=OFF -DENABLE_TAR=ON -DENABLE_CPIO=ON"

# 主包保留未被子包领走的二进制文件 (usr/bin/bsdtar, usr/bin/bsdcpio 等)
[build.payload]
default = "remaining"

# -------------------------------------------------------------
# 子包 1: 运行时共享库 (libarchive-libs)
# -------------------------------------------------------------
[[subpackages]]
name = "libarchive-libs"
description = "libarchive runtime shared library (libarchive.so.*)"
license = "BSD-2-Clause"

dependencies = [
    "virtual/libc",
    "zlib-libs >= 1.3.1",
    "zstd-libs >= 1.5.7"
]

[subpackages.payload]
files = [
    "usr/lib/*.so.*"
]

# -------------------------------------------------------------
# 子包 2: 开发头文件与链接符号 (libarchive-dev)
# -------------------------------------------------------------
[[subpackages]]
name = "libarchive-dev"
description = "libarchive headers, pkgconfig and linker symlinks"
license = "BSD-2-Clause"

dependencies = [
    "libarchive-libs >= 3.8.9"
]

[subpackages.payload]
files = [
    "usr/include/**",
    "usr/lib/*.a",
    "usr/lib/*.so",
    "usr/lib/pkgconfig/**",
    "usr/lib/cmake/**",
    "usr/share/man/man3/**"
]
```

---

## 3. 字段语义表

### 3.1 `[package]` (主包)
- `name`: 主包名称。
- `version` / `release` / `epoch`: 全局版本元数据（所有子包默认继承）。
- `slot`: 可共存的 ABI 槽，默认 `0`。外置内核模块必须设为目标内核版本。
- `channel`: 默认通道作用域（子包可覆盖）。
- `dependencies`: 主包专属运行时依赖。

### 3.2 `[[subpackages]]` (独立拆分子包)
- `name`: 子包名称（如 `foo-libs`, `foo-dev`, `foo-doc`）。
- `description`: 子包专属描述（可选，默认继承主包）。
- `dependencies`: 子包专属依赖（如 `foo-dev` 依赖 `foo-libs`）。
- `provides`: 子包显式提供的额外虚拟符号。
- `[subpackages.payload]`:
  - `files`: Glob 模式列表，匹配归属于该子包的文件。
  - `excludes`: 额外排除的 Glob 列表。

### 3.3 Multiple sources and patches

Recipes that require additional distfiles use an ordered array of tables instead of
the legacy singleton `[source]`. The two forms are mutually exclusive and every
distfile is independently verified before the sandbox starts:

```toml
[[sources]]
url = "https://example.org/project-1.0.tar.xz"
sha256 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

[[sources]]
url = "https://example.org/project-languages-1.0.tar.xz"
sha256 = "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789"
strip_components = 1
destination = "translations"
```

The first archive supplies the primary source tree and is unpacked with its leading
directory removed. Later archives overlay their native layout in declaration order.
Both defaults can be overridden per source: `strip_components` controls leading
archive removal and `destination` selects a safe directory below the shared source
root. Arrays have no fixed length; a five-tarball recipe produces one ordered
manifest and independently verifies all five archives before any extraction begins.
Files in the recipe's `patches/` directory are copied into the sandbox and applied
with `patch -p1` in bytewise filename order during `src_prepare`.

Independently published raw inputs such as upstream patch-series files use
`kind = "file"`. They require a lowercase SHA-256 and a non-root `destination`,
are copied without extraction, and retain declaration order:

```toml
[[sources]]
kind = "file"
url = "https://example.org/project/fix-001.patch"
sha256 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
destination = ".source-patches/001"
```

The standard autotools class applies `.source-patches/*` bytewise and exposes
`source_patch_strip` for upstream series that do not use the repository's fixed
`patches/` convention.

Git inputs use the same ordered source array but replace archive integrity with an
exact full object ID:

```toml
[[sources]]
kind = "git"
url = "https://example.org/project.git"
commit = "0123456789abcdef0123456789abcdef01234567"
submodules = true
destination = "."
```

`kind` defaults to `archive`. Git accepts only network URLs and full 40- or
64-hex-digit commits; branches, tags, abbreviated IDs, `file://`, and Git external
helpers are rejected. Recursive submodules are checked out at commits recorded by
the superproject. The exported tree contains no `.git` metadata and joins archive
inputs in declaration order. Git sources do not accept `sha256` or
`strip_components`.

### 3.4 Private-channel ELF lookup

Packages whose channel is neither `system` nor `*/system` have their ELF RUNPATHs
rewritten before payload carving. Directories containing ELF files with a SONAME
are discovered automatically. Libraries supplied by a separate package can be
declared relative to the channel root:

```toml
[build]
private_library_dirs = ["lib", "lib64"]
```

The builder rejects absolute paths and parent traversal. It passes only computed
`$ORIGIN`-relative paths to the globally configured `patchelf` executable.

### 3.5 Out-of-tree kernel modules

Prebuilt Kmod packages use the target kernel version as their package Slot. The
declarative `kmod` rclass builds against that kernel's read-only headers and asks
the kernel build system to install into the versioned module tree:

```toml
[package]
name = "vendor-driver"
slot = "6.12.4"
version = "1.8.0"

[build]
inherit = ["kmod"]

[build.args]
make_args = ""
```

The package step rejects any `usr/lib/modules/<version>` tree that differs from
the declared Slot. This gives DKMS-style out-of-tree sources reproducible Kmod
artifacts whose installed versions are tracked independently by the solver and
database instead of being rebuilt as untracked host mutations.

### 3.6 Declarative installation lifecycle

Source-free data and policy packages use `[install]` entries instead of a
`custom` build class. All paths are relative to DESTDIR, duplicate or escaping
paths are rejected, and numeric modes use TOML decimal notation:

```toml
[[install.directories]]
path = "tmp"
mode = 1023 # 01777

[[install.files]]
path = "usr/lib/os-release"
content = "NAME=Example\n"
mode = 420 # 0644

[[install.symlinks]]
path = "etc/os-release"
target = "../usr/lib/os-release"

[[install.copies]]
source = "policy"
path = "usr/share/example/policy"
recursive = true
```

Recipes with no upstream source are valid when their payload is fully
declarative, including pure meta-packages whose only state is dependencies.

Recipes may declare system accounts with `[[sysusers]]`; Sage stores the
declaration as package metadata and reconciles `/etc/passwd`, `/etc/group`, and
`/etc/shadow` directly without calling an init-system utility. Optional `service.toml` and
`triggers.toml` files beside `recipe.toml` are schema-validated and copied into
the main package's `.METADATA` section. Package-specific triggers use the same
single-trigger schema as files under `/usr/share/sage/triggers`.

Executable lifecycle hooks such as `preinst`, `postinst`, `prerm`, and `postrm`
are rejected. Transactions never treat package metadata as executable content and
never open standard input for configuration prompts.

### 3.7 Features and build-only dependencies

Features are deterministic recipe transformations selected with
`sage build --feature <name>`. Defaults are enabled unless
`--no-default-features` is supplied:

```toml
[build]
inherit = ["meson"]
dependencies = ["wayland-protocols >= 1.37-1"]
target = "aarch64-linux-gnu"
target_dependencies = ["zlib-dev >= 1.3-1"]

[features.tls]
default = true
dependencies = ["openssl-libs >= 3.4-1"]
build_dependencies = ["pkgconf"]

[features.tls.args]
meson_args = "-Dtls=enabled"

[features.tls.env]
TLS_BACKEND = "openssl"
```

Rules fold once in bytewise name order. Runtime dependencies enter the normal
PubGrub graph through the artifact manifest. Build dependencies from the recipe,
features, and inherited rclasses are solved together, installed only under the
ephemeral `/toolchain`, and never recorded as host state. Unknown feature names
or unconfigured targets fail before source downloads begin.

For cross builds, `target_dependencies` are architecture-filtered and extracted
to a separate read-only `/sysroot`. Native build tools remain in `/toolchain`, so
host executables can never be replaced by target binaries. Features may add the
same field with `target_dependencies = [...]`.
