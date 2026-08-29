# 规范: 全局构建与审计策略 (`/etc/sage/build.toml` v1)

- **文件路径**: `/etc/sage/build.toml`
- **Schema 版本**: `1`
- **设计目标**: 定义主机全局构建优化标志、沙箱配置及受管工具链选择；资源字段保留供后续 cgroup adapter 实现。

---

## 1. 规范示例

```toml
schema_version = 1

# 沙箱基础设施
fakeroot = "fakeroot"
bwrap = "bwrap"
git = "git"                         # exact-commit Git source fetcher
sysroot = "/"

# 默认工具链偏好
cc = "clang"
cxx = "clang++"
linker = "ld.lld"
rustc = "rustc"
patchelf = "patchelf"               # 私有通道 ELF RUNPATH 改写器

# 回退工具链偏好
fallback_cc = "gcc"
fallback_cxx = "g++"
fallback_linker = "ld"

# 全局构建标志
cflags = "-O3 -march=x86-64-v3 -pipe"
cxxflags = ""                       # 为空时自动继承 cflags
cppflags = ""
ldflags = "-Wl,--as-needed -Wl,-O1"
rustflags = "-C target-cpu=x86-64-v3"

# 可重现构建固定时间戳 (Unix Epoch)
source_date_epoch = 1700000000

# 并发与资源限制
jobs = 0                            # 0 = 自动匹配在线 CPU 线程数
memory_limit = ""                   # Reserved; currently parsed but not enforced
pids_limit = 2048                   # Reserved; currently parsed but not enforced

# 编译器缓存策略 (none | auto | ccache | sccache)
compiler_cache = "auto"
ccache_dir = "/var/cache/sage/ccache"
```

---

## 2. 字段详细解析

### 2.1 编译器配置与 wrapper 溯源
`allowed_compilers` 与 `allowed_linkers` 用于验证所选配置。构建时只为受管编译器和链接器安装 wrapper，并记录实际调用及版本；当前 schema v1 不使用 ptrace，也不会拦截所有 `execve` / `execveat`。

### 2.2 确定性时间戳 (`source_date_epoch`)
沙箱内的环境变量 `SOURCE_DATE_EPOCH` 将被强行重设为该值，并且构建时间、归档时间戳均统一规范化，保证二进制可重现（Bit-for-Bit Reproducible Builds）。

### 2.3 私有通道 RUNPATH

非 `system` 子通道在拆包前自动扫描整个 DESTDIR。包含 SONAME 的 ELF 所在目录会成为私有库目录，`patchelf` 将动态 ELF 的 RUNPATH 改写为相对该文件的 `$ORIGIN` 路径。绝对 RPATH 不会继承到包中，因此构建主机路径不能泄漏进产物。

### 2.4 Cross-target table

```toml
build = "x86_64-linux-gnu"

[targets.aarch64-linux-gnu]
cc = "aarch64-linux-gnu-gcc"
cxx = "aarch64-linux-gnu-g++"
ar = "aarch64-linux-gnu-ar"
strip = "aarch64-linux-gnu-strip"
arch = "aarch64"
goos = "linux"
goarch = "arm64"
cmake_system_name = "Linux"
endian = "little"
rustflags = "-C target-feature=+crt-static"
```

Recipe `[build].target` selects an exact entry. Sage injects the compiler,
binutils, Go platform, Meson cross file, CMake platform, and Cargo target into
all inherited classes. Adding an architecture changes TOML only; Rust contains
no architecture mapping table.

Rclass templates also receive `CC_FAMILY`, derived from the selected native or
cross-target C compiler (`clang`, `gcc`, or its validated tool name). Classes
may use this fact for compiler-specific upstream switches without probing the
host or duplicating compiler selection in recipes.
