# 规范: 通用守护进程服务定义 (`service.toml` v1)

- **归档内部位置**: `.METADATA/service.toml`
- **Schema 版本**: `1`
- **设计目标**: 纯粹跨 Init 系统的声明式服务规范，**完全解耦于底层 Init 实现**。由对应 Init 系统的 `rclass/init-<provider>.toml` 模板引擎在 Rebuild 时自动渲染。

---

## 1. 规范示例

```toml
schema_version = 1

[service]
name = "sshd"
description = "OpenSSH Server Daemon"
command = ["/usr/sbin/sshd", "-D"]
stop_command = []
reload_command = ["/usr/bin/kill", "-HUP", "$MAINPID"]
user = "root"
group = "root"
working_dir = "/"
pid_file = "/run/sshd.pid"
restart = "always"             # "always" | "on-failure" | "no"
type = "simple"                # "simple" | "forking"
after = ["net", "syslog"]
before = []
runtime = ""                   # 绑定运行时，例如 "runtime/java:openjdk-21"
```

---

## 2. 声明式 Init 渲染工作流 (Zero Hardcoded Init in Engine)

```text
/etc/sage/system.toml [providers.init = "openrc"]
                   │
                   ▼ (加载对应 rclass)
         rclass/init-openrc.toml
                   │
                   ├─► 读取各个包的 .METADATA/service.toml
                   ├─► 展开 template 模板字符串
                   └─► 写入目标文件 /etc/init.d/<name> (mode 0755)
```

1. **引擎完全通用**: `sage-sys` 内部不包含任何针对特定 Init（如 OpenRC、Systemd、Loom、Runit、s6）的硬编码分支。
2. **完全可扩展**: 增加对新 Init 系统的支持，仅需在包仓库中添加 `rclass/init-<name>.toml`，无需重新编译 `sage` 二进制。

## Rebuild lifecycle and recovery

A rebuild solves desired and retained package roots before publishing any package.
The resolved `virtual/init` provider, including channel and slot, selects the
renderer. A configured provider is a preference; the renderer must follow the
concrete release selected after solver backtracking. Ordinary removal rejects a
currently bound provider and requires a rebuild to switch that binding.

Before journaling, Sage validates the complete package payload and the final
renderer/service inputs, including enabled declarations and planned command
executables. A path owned by a retired or replaced release cannot substitute for
a missing replacement. Dry runs solve and display the plan without requiring a
new renderer to be installed or downloading its payload.

The install journal carries the selected bindings, retired package records, and
planned native-service generation. Cleanup runs before any package is replaced
or retired, keeping old renderer commands and their runtime dependencies
available. Only previously enabled services are disabled; inactive definitions
are removed without a disable command. Cleanup progress is persisted before
publication. Recovery invoked by any later mutating command completes package
publication, retirement triggers, atomic binding replacement, service
rendering/enabling, and rebuild triggers using the original plan, even if
`system.toml` has changed. Pure package additions leave unchanged services enabled.

Whole packages pruned by rebuild retain captured `post-remove` handlers, distinct
from upgrade-only obsolete paths. Handed-off paths and preserved configurations
are excluded from the retirement event. Mandatory removal-trigger executables
are checked against the final package overlay before journaling. A completed
retirement-trigger batch has its own checkpoint and is not replayed by recovery.

Provider commands must be retry-safe: process termination can occur between an
external command's success and its durable checkpoint. Static executable checks
do not prove that an arbitrary compiler, validator, or runtime dependency will
succeed. A runtime error leaves the journal pending for forward recovery; it does
not claim that the rebuild completed. No legacy journal or rendered-state
migration is provided by the 0.4 implementation.
