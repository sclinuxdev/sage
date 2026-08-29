# 规范: 构建类与 Init 服务生成类 (`rclass/*.toml` v1)

- **文件位置**: `rclass/<name>.toml` (与配方仓库同级维护)
- **Schema 版本**: `1`
- **核心目标**: **彻底零硬编码**。不仅将编译器调用抽离到 rclass，而且将**所有 Init 系统的服务生成与转换逻辑全部交由 `init-*.toml` rclass 声明**。

---

## 1. `rclass` 双重职责

1. **构建类 (`rclass/<tool>.toml`)**: 提供源码编译阶段生命周期（如 `cargo.toml`, `cmake.toml`）。
2. **Init 服务生成类 (`rclass/init-<provider>.toml`)**: 定义如何将通用 `service.toml` 模板化渲染为特定 Init 系统的配置文件（如 `init-openrc.toml`, `init-systemd.toml`, `init-loom.toml`）。

---

## 2. 构建类示例

### 示例 1: `rclass/cmake.toml`
```toml
schema_version = 1
name = "cmake"
description = "CMake and Ninja build class"

implicit_build_dependencies = ["cmake", "ninja"]
allowed_compilers = ["clang", "gcc"]
allowed_linkers = ["lld", "mold", "ld"]

[env]
CMAKE_BUILD_PARALLEL_LEVEL = "${JOBS}"

[phases]
src_configure = """
cmake -B "${BUILD_DIR}" -G Ninja \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DCMAKE_INSTALL_LIBDIR=lib \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_FLAGS="${CFLAGS}" \
    -DCMAKE_CXX_FLAGS="${CXXFLAGS}" \
    -DCMAKE_EXE_LINKER_FLAGS="${LDFLAGS}" \
    ${args.cmake_args} \
    "${SRC_DIR}"
"""

src_compile = """
ninja -C "${BUILD_DIR}" -j "${JOBS}"
"""

src_install = """
DESTDIR="${DESTDIR}" ninja -C "${BUILD_DIR}" install
"""
```

---

## 3. Init 服务生成类示例 (Zero Hardcoded Init)

### 示例 2: `rclass/init-openrc.toml`
```toml
schema_version = 1
name = "init-openrc"
description = "OpenRC Service Generator Class"

[service_generator]
target_path = "/etc/init.d/${service.name}"
mode = 493 # 0755

template = """
#!/usr/bin/openrc-run
description="${service.description}"

command="${service.command[0]}"
command_args="${service.command[1:]}"
pidfile="${service.pid_file}"
command_user="${service.user}:${service.group}"
directory="${service.working_dir}"

depend() {
    need ${service.after}
}
"""
```

### 示例 3: `rclass/init-systemd.toml`
```toml
schema_version = 1
name = "init-systemd"
description = "Systemd Service Generator Class"

[service_generator]
target_path = "/usr/lib/systemd/system/${service.name}.service"
mode = 420 # 0644
service_dependency_suffix = ".service"

template = """
[Unit]
Description=${service.description}
After=${service.after_space}
Before=${service.before_space}

[Service]
Type=${service.type}
ExecStart=${service.command_quoted}
ExecStop=${service.stop_command_quoted}
ExecReload=${service.reload_command_quoted}
User=${service.user}
Group=${service.group}
WorkingDirectory=${service.working_dir}
Restart=${service.restart}
PIDFile=${service.pid_file}

[Install]
WantedBy=multi-user.target
"""

[service_generator.dependency_aliases]
network = "network.target"
syslog = "syslog.target"
```

### 示例 4: `rclass/init-loom.toml`
```toml
schema_version = 1
name = "init-loom"
description = "Loom Service Generator Class"

[service_generator]
target_path = "/usr/lib/loom/services/${service.name}.toml"
mode = 420 # 0644

# 生成完成后触发全局拓扑校验命令 (Fail-Closed)
validate_command = "/usr/lib/loom/loom validate --root ${SYSROOT}"

template = """
name = "${service.name}"
description = "${service.description}"
exec = ${service.command_json}
stop_exec = ${service.stop_command_json}
reload_exec = ${service.reload_command_json}
user = "${service.user}"
group = "${service.group}"
depends_on = ${service.after_json}
before = ${service.before_json}
runtime = ${service.runtime_json}
"""
```

---

## 4. 变量展开机制 (Variable Interpolation)

在 `service_generator.template` 中支持以下展开变量：

| 变量 | 说明 |
| :--- | :--- |
| `${service.name}` | 服务唯一标识名称 |
| `${service.description}` | 服务描述字符串 |
| `${service.command[0]}` | 服务执行可执行文件绝对路径 |
| `${service.command[1:]}` | 传给可执行文件的命令行参数 |
| `${service.command_str}` | 空格拼接的完整启动命令 |
| `${service.command_quoted}` | 每个参数独立引用的完整启动命令 |
| `${service.command_json}`| JSON 数组格式命令 `["/usr/sbin/sshd", "-D"]` |
| `${service.stop_command_*}` / `${service.reload_command_*}` | 停止和重载命令的 `str`、`quoted`、`json` 形式 |
| `${service.user}` / `${service.group}` | 运行用户与用户组 |
| `${service.after_space}` | 空格分隔的依赖项（如 `network.target syslog.target`） |
| `${service.after_json}` | JSON 数组格式依赖 `["net", "syslog"]` |
| `${service.before_space}` / `${service.before_json}` | 服务先后约束；由 init rclass 映射为原生名称 |
| `${service.working_dir}` | 工作目录 |
| `${service.pid_file}` | PID 文件路径 |
| `${service.restart}` | 重启策略 (`always`, `on-failure`, `no`) |
| `${service.runtime}` / `${service.runtime_json}` | 可选运行时约束 |
| `${SYSROOT}` | 目标系统的根文件系统挂载点 |

`dependency_aliases` and `service_dependency_suffix` belong to the init rclass,
not to package service declarations. This keeps package dependencies expressed
as init-independent logical names while allowing each provider to render its
native identifiers.

## 5. Standard build-class library

The standard library contains `autotools`, `meson`, `python` (PEP 517), `go`,
`cmake`, `cargo`, `npm`, `pnpm`, `gradle`, `maven`, `kmod`, and `kernel`. Tool defaults
remain data-owned:

```toml
implicit_build_dependencies = ["meson", "ninja", "pkgconf"]

[defaults]
"args.test" = "0"
"args.meson_args" = ""
```

Later rclasses and recipe arguments override defaults. Implicit dependencies
participate in the constrained build-environment solve. The verified result is
mounted read-only at `/toolchain` and exposed through standard tool search paths.

### 5.1 Linux kernel class

The `kernel` class consumes the kernel source, patches, and configuration only
through ordered recipe source declarations. It applies `.source-patches/*` in
filename order, accepts compressed `.zst` patch inputs, runs `olddefconfig`, and
requires the resulting `kernelrelease` to equal the package Slot. The install
phase writes the kernel image and modules below `usr/lib/modules/<Slot>` and
produces the external-module build tree for a separate headers subpackage.

The class does not select a kernel configuration, patch URL, hostname, initramfs
tool, or init system. Those are recipe and system declarations. A kernel recipe
that needs an out-of-tree module build must declare the matching header package
with its Slot in `[build].dependencies`, for example
`linux-zen-headers:7.1.11-zen1`.

Toolchains selected by kernel configuration are recipe dependencies rather than
unconditional class dependencies. For example, the Arch linux-zen configuration
sets `CONFIG_RUST=y`, so its recipe declares `rust-bin`, `rust-src`, and
`rust-bindgen` explicitly. The kernel build does not invoke Cargo; recipes should
not add `cargo` unless their selected kernel configuration or an extra build step
actually uses it.

The CMake class accepts `args.source_dir` and `args.patch_root` for monorepos,
and applies recipe-declared `.source-patches/*` in bytewise order. This keeps
LLVM and its Clang/LLD/compiler-rt outputs in one build unit while retaining
separate package payloads.

### 5.2 Node.js classes

`npm` requires `npm ci`; `pnpm` requires a frozen lockfile. Both isolate their
cache/store below `/build`, run a configurable build and test script, pack one
deterministic tarball, and install it below DESTDIR without running installation
lifecycle scripts. Native addons use the ordinary configured C/C++ toolchain.

### 5.3 JVM classes

`gradle` uses a private `GRADLE_USER_HOME`, disables the daemon, and caps workers
to `${JOBS}`. `maven` uses a private local repository, batch mode, and the same
parallelism cap. Commands, tasks/goals, artifact glob, and final JAR name are
recipe arguments. Artifact globs must resolve to exactly one file.

Ecosystem dependency access follows the global sandbox rule: builds have no
network unless the recipe explicitly sets `allow_network = true`. Vendored or
pre-populated inputs therefore remain the reproducible default.
