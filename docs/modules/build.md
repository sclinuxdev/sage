# 模块实现: 密闭构建沙箱与 rclass 引擎 (`sage-build`)

- **Crate 路径**: `crates/sage-build`
- **选用生态**: `bwrap` (Bubblewrap), `fakeroot`, 自研轻量零依赖 ELF 动态符号解析
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
   - 对每个独立切分后的子包 staging 目录，扫描其内部的 ELF 动态可执行文件与动态库。
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

---

## 7. 源码集群构建流水线: Bootstrap 与 Mass-Rebuild 语义差异

在全源码构建生命周期中，`sage` 对两种典型构建场景采用了明确分化的产物重用与调度策略：

### 7.1 闭环恢复与强制重构策略 (`resume_existing`)
1. **`sage bootstrap` (支持断点续建)**:
   - 调度参数设置 `resume_existing: true`。
   - 针对多阶段自举方案中的各个单元，若目标构建池（`pool`）中已存在对应版本与架构的完整归档（`should_skip_existing_unit`），则**自动跳过构建并复用已有产物**。这避免了自举流水线在后置阶段失败重启时，反复对极其耗时的底层工具链（seed compiler, binutils, glibc）执行重复编译。
2. **`sage mass-rebuild` (无条件全量重构)**:
   - 调度参数设置 `resume_existing: false`。
   - 即使包池中已存在同名版本产物，**仍然无条件重新调度并编译每一个被发现的配方**，新产物原子覆盖旧文件，确保源码仓库变动或外部全局编译配置调整能够完整反映到产物中。

### 7.2 通道限定与多生产者符号调度 (Multi-Producer Symbols)
- **虚拟符号通道限定**: 导出的虚拟符号 `BuildSymbol::Provided` 严格限定所属 Channel（`{ channel, name }`），彻底杜绝跨通道符号污染。
- **提供者符号自动分类**: 构建图生成时通过 `sage_core::is_virtual_symbol(&dependency.name)` 自动将 `virtual/*`、`so:*` 以及 `cmd:*` 依赖映射为 `BuildSymbol::Provided`，与配方主包及子包中声明的 `provides` 列表严格对齐。
- **多候选生产者容错**: 调度器在层级构建开始前对所有层级的 candidate producers 建立全量倒排索引。仅当某符号的**所有候选生产者均宣告失败**时，该符号才会被标记为 `blocked_symbols` 并级联阻塞依赖它的下游单元；若存在任一候选生产者构建成功，下游单元仍能正常调度执行。
- **任务失败致命性保证**: Tokio 异步 worker 发生 Panic 或被异常取消时，调度器将其视为不可恢复的致命故障（Fatal Error），立即中止构建流程，防止丢失的生产符号导致下游构建进入未定义状态。

### 7.3 多架构配方树过滤与坐标去重 (`BuildGraph::discover_for_arch`)
- **跨架构别名适配**: 通过 `sage_sys::arch_matches(&recipe.package.arch, target_arch)` 兼容 `x86_64` / `amd64`、`aarch64` / `arm64`、`riscv64` / `riscv64gc` 以及 `any` / `noarch`。
- **同坐标精准度优先去重**: 当配方树中针对相同的 `(channel, name, slot)` 存在多个配方候选时（例如特定架构优化版本与通用版本并存），优先采纳与目标架构完全一致的精准配方，回退采纳通用 `any`/`noarch` 配方，拒绝将无关架构配方引入构建图。


---

## 8. DESTDIR 目录隔离与 Dirfd 安全声明式安装

针对免源码的数据、元包与声明式安装配方（`[install]`），`sage-build` 通过 `dirfd` 与 `O_NOFOLLOW` 建立了严苛的容器级文件系统安全边界：

1. **防路径穿越与 Symlink 逃逸**:
   - 声明式文件写出（`stage_declarative_install`）与目录递归复制（`copy_dir_entries_beneath`）完全锚定于 `DESTDIR` 的文件描述符。
   - 创建目录、写入文件或建立软链接均使用 `openat` / `symlinkat` / `fchmod`，强制附加 `O_NOFOLLOW | O_DIRECTORY`。如果 `DESTDIR` 内部存在构建脚本预先埋设的指向宿主机敏感目录（如 `/etc` 或 `/usr`）的软链接，`openat` 立即拒绝跟踪并报错拦截。
2. **源目录祖先路径合法性审计**:
   - 在从配方目录复制文件至 `DESTDIR` 时，逐级校验源路径与其各级祖先目录，拒绝跨越符号链接读取配方仓库外部的宿主机文件。

---

## 9. 包坐标校验与产物发布收敛 (Publishing Containment)

1. **核心坐标强约束**:
   - 配方加载（`RecipeSpec::load`）与发布时统一调用 `Package::validate`，严格验证 `channel`、`name`、`slot`、`arch`、`version` 与 `license`。
   - 严禁包含路径分隔符（`/`、`\`）、目录穿越成分（`..`、`.`）以及 ASCII 控制字符。
2. **发布路径严格收敛**:
   - 在产物归档移入全局存储池（`pool`）时，发布路径被数学证明严格位于 `.slots/<channel>/<name>/<slot>/` 目录之内，杜绝通过恶意 channel 名或 slot 命名实施的任意路径覆盖攻击。

