# 规范: 二进制包归档格式 (`*.pkg.tar.zst`)

- **扩展名**: `<name>-<version>-<release>-<arch>.pkg.tar.zst`
- **压缩算法**: Zstandard (zstd) 流式压缩
- **设计目标**: 极速流式检视、低开销元数据解析与确定性解包。

---

## 1. 归档内部层级结构

```text
pkgname-1.0.0-1-amd64.pkg.tar.zst
├── .METADATA/                    # 严格置于归档最前端
│   ├── manifest.toml             # 包核心元数据 (Schema v1)
│   ├── files.idx                 # 逐文件路径、大小、mode、SHA-256 索引
│   ├── service.toml              # 通用服务定义 (可选)
│   └── triggers.toml             # 自定义触发器 (可选)
└── data/                         # 根文件系统有效载荷 (Payload)
    ├── usr/
    │   ├── bin/
    │   └── lib/
    └── etc/
```

`.METADATA` 采用严格白名单，仅允许上图中的声明式 TOML 与必要索引。`preinst`、`postinst`、`prerm`、`postrm` 等可执行生命周期脚本属于非法归档内容；检视、创建和解包路径均不会执行包内程序。

---

## 2. 元数据清单 (`.METADATA/manifest.toml` v1)

```toml
schema_version = 1

name = "ripgrep"
slot = "0"                        # 原生多版本 slot
version = "14.1.0"
release = 1
epoch = 0
arch = "amd64"
channel = "system"
description = "Fast line-oriented search tool"
license = "MIT OR Unlicense"
installed_size = 5384912
build_time = 1700000000

dependencies = [
    "virtual/libc",
    "so:libpcre2-8.so.0"
]
provides = ["rg"]
conflicts = []

# Present only for artifacts produced by a managed Sage source build.
[[managed_build_tools]]
role = "cc"
executable = "/usr/bin/clang"
family = "clang"
version = "clang version 18.1.8"
version_argument = "--version"
parameters = ["CPPFLAGS=-D_FILE_OFFSET_BITS=64", "CFLAGS=-O2"]
```

`managed_build_tools` is an observation of this archive's build, not a package
or repository default. Sage records only compiler and linker wrappers that
actually executed (`cc`, `cxx`, `linker`, or `rustc`); CRT objects and helper
tools such as `ar`, `as`, and `ranlib` are intentionally excluded. `parameters`
contains the non-empty Sage-configured flag channels used for that role.

---

## 3. 文件索引清单 (`.METADATA/files.idx`)

采用紧凑的 TSV / 纯文本格式逐行记录：
```text
# path\tmode\tsize\tsha256
usr/bin/rg	0755	5380000	33c616959def5f80a763a51cf1feed8c8ea9db583556862e3c6a84fa42f95499
usr/share/man/man1/rg.1	0644	4912	0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
```

---

## 4. 恒定代价检视保证 ($O(1)$ Inspect)

打包时将 `.METADATA/` 目录的内容严格写入 Tar 归档的最开始位置。`sage-archive` 在检视或读取元数据时，仅需流式读取前数十 KB 数据并在遇到第一个 `data/` 条目时立即中止解压，消耗接近 $O(1)$ 的时间和极小内存。

`manifest.toml` may include `features = ["name", ...]`. This sorted audit list
records the selection baked into the immutable artifact. Conditional runtime
dependencies are already expanded into `dependencies`, keeping repository lookup
and the PubGrub hot path identical to ordinary packages.
