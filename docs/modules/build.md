# 模块实现: 密闭构建沙箱与 rclass 引擎 (`sage-build`)

- **Crate 路径**: `crates/sage-build`
- **选用生态**: `bwrap` (Bubblewrap), `fakeroot`, `goblin` (ELF 动态符号解析)
- **代码预算**: ~1,700 行
- **职责**: 驱动 Bubblewrap 密闭构建沙箱、执行 `rclass` 阶段脚本、记录受管工具 wrapper 溯源、切分多包产物并扫描 ELF。

---

## 1. 密闭沙箱驱动 (`SandboxRunner`)

组装 `bwrap` 隔离参数：
- 只读挂载 sysroot（`/`）。
- 读写挂载专有源码目录、构建目录与 `DESTDIR`。
- 清空所有宿主环境变量，注入固定值（`LC_ALL=C`, `TZ=UTC`, `SOURCE_DATE_EPOCH=...`）。
- 隔离网络命名空间（`--unshare-net`，除非配方显式声明）。
- 若指定了版本化工具链，将 `/opt/channels/<tool>/bin` 挂载并注入 `PATH` 首位。

---

## 2. `rclass` 单一 Runner 脚本生成与执行

为了保证最高执行性能与可调试性，`sage-build` 不在沙箱内逐阶段多次派生进程，而是采用**单一 runner 脚本生成模型**：

1. **模板合成**:
   - 加载配方指定的 `inherit` 列表（如 `cmake.toml` + `ninja.toml`）；
   - 注入全局构建优化标志（`${CFLAGS}`, `${LDFLAGS}`）与自定义参数（`${args.*}`）；
   - 将所有激活的阶段（`src_unpack`, `src_prepare`, `src_configure`, `src_compile`, `src_install` 等）合成一份带有统一错误陷阱的 Bash 脚本（`sage-build-runner.sh`，开启 `set -euo pipefail`）。
2. **单次进入沙箱**:
   - 在 `bwrap` + `fakeroot` 中单次执行该 runner 脚本，高效流转全部阶段。若某一步失败，即刻中止并输出精确行号日志。

---

## 3. 单配方多包切分流水线 (`PayloadCarver`)

构建完成后，`DESTDIR` 包含完整安装树。`sage-build` 按以下算法执行单次构建和互斥多包切分；当前实现使用常规文件复制，不宣称 reflink：

```rust
pub struct PayloadCarver;

impl PayloadCarver {
    /// 依次从 DESTDIR 中认领匹配文件，生成互斥的各个包暂存区
    pub fn carve_packages(
        destdir: &Path,
        recipe: &RecipeSpec,
    ) -> Result<Vec<PackageStagingArea>, BuildError>;
}
```

1. **逐个认领**: 遍历 `recipe.subpackages` 声明，对每个子包使用其 `[subpackages.payload.files]` Glob 列表匹配 `DESTDIR` 文件，并移入专属子包 staging 目录。
2. **主包收敛**: 未被任何子包认领的文件自动归入主包 staging 目录（除非主包显式指定了 allowlist）。
3. **独立封装**: 每个 staging 目录独立调用 `sage-archive` 生成对应的 `*.pkg.tar.zst`、`manifest.toml` 与 `files.idx`。

---

## 4. 工具 wrapper 溯源与 ELF 符号扫描

1. **工具链溯源**:
   - 为配置的编译器与链接器安装窄 wrapper，仅在实际执行时记录工具。
   - 当前实现不使用 ptrace，也不宣称观察 wrapper 之外的所有 `execve`。
2. **自动化 ELF 扫描 (`ElfScanner`)**:
   - 对每个独立切分后的子包 staging 目录，使用 `goblin` 扫描其内部的 ELF 动态可执行文件与动态库。
   - 提取 `DT_SONAME` 自动追加至该子包的 `provides = ["so:libfoo.so.1"]`。
    - 提取 `DT_NEEDED` 自动追加至该子包的 `dependencies = ["so:libbar.so.2"]`。

## 5. Per-build toolchain provenance

The sandbox installs narrow wrappers for the configured C compiler, C++
compiler, linker, and Rust compiler. Each wrapper logs its resolved executable
only when it is invoked. The resulting `managed_build_tools` entries are
written into every archive produced by that build with the role, resolved
executable, family, first `--version` line, and non-empty configured flag
channels. Unused configured tools are omitted. CRT objects and auxiliary
tools such as `ar`, `as`, and `ranlib` are not observed or reported.

## 6. Ephemeral inputs, features, and cross targets

Explicit recipe dependencies, selected feature build dependencies, and rclass
implicit dependencies form one constrained PubGrub root. Verified archives are
checked for file conflicts, extracted with the dirfd safety rules, and mounted
read-only at `/toolchain`; host LMDB state is untouched.

Cross-architecture headers and libraries are solved with the configured package
architecture and mounted separately at `/sysroot`. Compiler, pkg-config, and
CMake sysroot variables point there, while executable build tools remain native.

Feature folding and target selection happen before runner composition, leaving
phase execution free of per-feature and per-architecture branches. Cross tools
and platform facts come exclusively from the configured target table.

Git source inputs are fetched before sandbox entry with system and user Git
configuration disabled. Only explicit network transports are accepted, local
file transport is disabled for the superproject and recursive submodules, and
the resulting checkout is exported without VCS metadata into the immutable
distfile area. Build rclasses materialize archive and Git inputs in one ordered
plan.

Node and JVM classes reuse the same phase runner and build-only dependency
environment. npm/pnpm caches, Gradle home, and the Maven local repository live
under disposable `/build`; installation writes only to DESTDIR and package
install lifecycle scripts are not executed.

Mass rebuild constructs producer/consumer edges across main packages,
subpackages, provides, default features, and rclass dependencies. Deterministic
Kahn layers run with bounded package parallelism. Completed artifacts enter a
transient local repository view and are locked into later PubGrub solves.
Bootstrap plans compose several such graphs, allowing explicit seed stages to
break unavoidable self-hosting cycles.
