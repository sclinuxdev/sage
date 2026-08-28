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
mode = 0755

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
mode = 0644

template = """
[Unit]
Description=${service.description}
After=${service.after_space}

[Service]
Type=${service.type}
ExecStart=${service.command_str}
User=${service.user}
Group=${service.group}
WorkingDirectory=${service.working_dir}
Restart=${service.restart}

[Install]
WantedBy=multi-user.target
"""
```

### 示例 4: `rclass/init-loom.toml`
```toml
schema_version = 1
name = "init-loom"
description = "Loom Service Generator Class"

[service_generator]
target_path = "/usr/lib/loom/services/${service.name}.toml"
mode = 0644

# 生成完成后触发全局拓扑校验命令 (Fail-Closed)
validate_command = "/usr/lib/loom/loom validate --root ${SYSROOT}"

template = """
name = "${service.name}"
description = "${service.description}"
exec = ${service.command_json}
user = "${service.user}"
group = "${service.group}"
depends_on = ${service.after_json}
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
| `${service.command_json}`| JSON 数组格式命令 `["/usr/sbin/sshd", "-D"]` |
| `${service.user}` / `${service.group}` | 运行用户与用户组 |
| `${service.after_space}` | 空格分隔的依赖项（如 `network.target syslog.target`） |
| `${service.after_json}` | JSON 数组格式依赖 `["net", "syslog"]` |
| `${service.working_dir}` | 工作目录 |
| `${service.pid_file}` | PID 文件路径 |
| `${service.restart}` | 重启策略 (`always`, `on-failure`, `no`) |
| `${SYSROOT}` | 目标系统的根文件系统挂载点 |

## 5. Standard build-class library

The standard library contains `autotools`, `meson`, `python` (PEP 517), `go`,
`cmake`, `cargo`, and `kmod`. Tool defaults remain data-owned:

```toml
implicit_build_dependencies = ["meson", "ninja", "pkgconf"]

[defaults]
"args.test" = "0"
"args.meson_args" = ""
```

Later rclasses and recipe arguments override defaults. Implicit dependencies
participate in the constrained build-environment solve. The verified result is
mounted read-only at `/toolchain` and exposed through standard tool search paths.
