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
memory_limit = ""                   # 内存上限 (例如 "4G", "512M", 为空表示不设限)
pids_limit = 2048                   # 最大进程数配额 (0 表示不限制)

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

### 2.5 资源配额与 Cgroup v2 启动同步屏障

Sage 构建沙箱原生支持 Linux cgroup v2 资源约束 (`memory_limit` 与 `pids_limit`)：
- **层次发现**：自动定位当前进程所属控制组、systemd 委托的非 root `user@<uid>.service/app.slice` 或统一层次根目录，创建临时独立子切片 `sage-build-<pid>-<seq>`。
- **预执行同步屏障 (Pre-Exec Barrier)**：通过 Unix Domain Socket 建立父子进程双向同步。子进程在 `fork` 之后、`execve` 之前于 `pre_exec` 中阻塞等待；父进程将子进程真实 PID 写入 `cgroup.procs` 并严格校验。校验成功后父进程通知解除阻塞，子进程才执行 `execve`。彻底杜绝子进程在加入控制组前突发 Fork 或申请大量内存逃逸配额管控。
- **无静默吞咽**：配额写入、进程挂载与限制清理全路径严格校验错误并抛出 `BuildError::CgroupFailed`，杜绝任何静默失败。
