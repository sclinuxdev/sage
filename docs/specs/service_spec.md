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
not claim that the rebuild completed.

Interactive `service enable`, `service disable`, and `service adopt` transitions
use the same forward-recovery rule. The journal captures the service, active
generator, and exact before/after bytes for `services.toml` and rendered state
before the provider is changed. Declaration publication is idempotent, so a
write failure or interruption after the provider succeeds is completed by the
next mutating command instead of leaving permanent provider/declaration drift.
Provider state queries (`is_enabled_cmd`) require an explicitly distinguishable
disabled result (exit code 1) before skipping disable actions; query failures
(non-zero error exit codes or execution errors) leave the journal pending to
guarantee that managed-disabled declarations are never published while external
service state remains ambiguous or active. Preview operations (`--dry-run`) validate
this read-only query during `service enable` whenever the existing native definition
permits it (and unconditionally during `service disable` and `service adopt`),
ensuring query errors are surfaced before mutating state or persisting journals.

---

## 3. 服务状态模型与不变量 (Service State Model & Invariants)

### 3.1 核心不变量 (Core Invariant)
> **Sage 只撤销自己创建的状态，绝不擅自撤销管理员在 Sage 外部创建的状态。**
> (Sage only revokes state it created, never arbitrarily revoking state created by the administrator outside Sage.)

Sage 不采用 NixOS 式的“封闭世界绝对假定”。系统的真实状态为：
$$\text{Actual Init State} = \text{Sage Managed State} + \text{Administrator Manual State}$$

### 3.2 三态生命周期 (Three-State Lifecycle)
1. **`managed-enabled`**：
   - 明确在 `/etc/sage/services.toml` 的 `enabled` 中声明。
   - Sage 负责渲染配置并调用 Init Provider 的 `enable_cmd`，保证开机自启。
2. **`managed-disabled`**：
   - 在 `/etc/sage/services.toml` 的 `disabled` 中声明，或曾记录在 `var/lib/sage/rendered-services.toml` 的 `enabled` 集合中但在重配置后被移除。
   - Sage 负责在显式禁用操作中调用 Init Provider 的 `disable_cmd`。如果管理员随后在 Sage 外部启用它，Sage 报告 `managed-disabled (drift)`，但不会自动撤销管理员状态。
3. **`unmanaged`**：
   - 从未被 Sage 接管或声明。
   - Sage 绝对不擅自执行 `disable`。若管理员在外部执行了 `systemctl enable`，Sage 在调和时予以保留，绝不破坏管理员的手工操作。

### 3.3 漂移探测 (Drift Detection)
Init Provider 的 `rclass/init-*.toml` 可声明 `is_enabled_cmd`：
```toml
is_enabled_cmd = "/usr/bin/systemctl --root ${SYSROOT} is-enabled ${service.name}.service"
```
`is_enabled_cmd` 的退出码语义遵循明确约定：
- `0`：服务处于启用状态（enabled）；
- `1`：服务明确处于禁用状态（disabled）；
- 其他非零退出码或执行异常：表示查询命令内部故障或异常，生命周期事务将挂起 journal，拒绝假定服务已禁用。

在 `sage rebuild` 时，若发现已安装包中的某服务在底层 Init 中处于 `enabled` 状态，但属于 Sage 的 `unmanaged` 或 `managed-disabled` 集合，Sage 将输出漂移警告与处理建议（不中断构建也不强制禁用）：
```text
warning: service 'sshd' is enabled outside Sage
         systemd: enabled
         services.toml: unmanaged
Hint:
  sage service adopt sshd
  no automatic provider state change was made
```

### 3.4 交互管理命令 (`sage service`)
- `sage service enable <svc>`：将服务写入 `services.toml`（`enabled`），并立即调用底层 Init Provider 激活。
- `sage service disable <svc>`：从 `enabled` 移除并记录入 `disabled`，调用底层 Init Provider 禁用。
- `sage service adopt <svc>`：将管理员外部手工启用的服务平滑纳管至 Sage 声明式配置中（转为 `managed-enabled`）。
- `sage service list`：列出系统已知的所有服务及其管理状态（`managed-enabled`、`managed-disabled`、`managed-disabled (drift)`、`unmanaged`、`unmanaged (drift)`）。对尚未渲染原生单元定义（如刚安装包但尚未 `sage rebuild`）或查询状态不确定的服务，以声明状态与未知/未接管状态安全呈现，不中断只读查询。
