# 规范: 全局构建与审计策略 (`/etc/sage/build.toml` v1)

- **文件路径**: `/etc/sage/build.toml`
- **Schema 版本**: `1`
- **设计目标**: 定义主机全局构建优化标志、沙箱资源配额及 Ptrace/Seccomp 审计工具链白名单。

---

## 1. 规范示例

```toml
schema_version = 1

# 沙箱基础设施
fakeroot = "fakeroot"
bwrap = "bwrap"
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
memory_limit = ""                   # Cgroups v2 内存限制 (例如 "8G", 为空表示不限制)
pids_limit = 2048                   # Cgroups v2 最大进程数

# 编译器缓存策略 (none | auto | ccache | sccache)
compiler_cache = "auto"
ccache_dir = "/var/cache/sage/ccache"
```

---

## 2. 字段详细解析

### 2.1 编译器审计白名单与 Ptrace 监督
在沙箱运行期间，`sage-build` 会拦截所有的 `execve` / `execveat` 调用：
- 仅允许执行配方及其 `rclass` 声明的 `allowed_compilers`（如 `clang`, `gcc`, `rustc`）与系统核心命令（`sed`, `awk`, `sh` 等）。
- 阻止外部未经授权的二进制偷渡执行，确保构建产物 100% 来源透明与防篡改。

### 2.2 确定性时间戳 (`source_date_epoch`)
沙箱内的环境变量 `SOURCE_DATE_EPOCH` 将被强行重设为该值，并且构建时间、归档时间戳均统一规范化，保证二进制可重现（Bit-for-Bit Reproducible Builds）。

### 2.3 私有通道 RUNPATH

非 `system` 子通道在拆包前自动扫描整个 DESTDIR。包含 SONAME 的 ELF 所在目录会成为私有库目录，`patchelf` 将动态 ELF 的 RUNPATH 改写为相对该文件的 `$ORIGIN` 路径。绝对 RPATH 不会继承到包中，因此构建主机路径不能泄漏进产物。
