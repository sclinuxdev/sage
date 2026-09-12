# 规范: 声明式系统配置 (`/etc/sage/system.toml` v1)

- **文件路径**: `/etc/sage/system.toml`
- **Schema 版本**: `1`
- **设计目标**: 定义目标 Linux 系统的软件包与 provider 期望状态，作为 `sage rebuild` 包调和的声明源；服务激活状态由独立的 `/etc/sage/services.toml` 定义。

---

## 1. 规范示例

```toml
schema_version = 1

[system]
# 系统默认架构
architecture = "amd64"
# 活动 Profile 软链接指向 (聚合 /etc/sage/profiles/<profile>)
profile = "default"

# 虚拟接口提供者映射 (完全动态键值对，引擎内部零硬编码接口)
# 注: Shell (如 bash/zsh/dash) 为普通独立包或由 alternatives 仲裁，不属于 providers 管理范围
[providers]
init = "loom"                 # 候选: loom | systemd
# Optional: choose the provider package; its package trigger owns the exact
# command and arguments (for example mkinitcpio or dracut).
# initramfs-generator = "mkinitcpio"
udev = "eudev"               # 候选: eudev | systemd-udev | busybox-mdev
libc = "glibc"               # 候选: glibc | musl
coreutils = "gnu-coreutils"  # 候选: gnu-coreutils | uutils-coreutils | busybox
depmod = "kmod"              # 候选: kmod | busybox
awk = "gawk"                 # 候选: gawk | mawk | busybox-awk

# 声明式系统核心软件包集合 (Rebuild 必须确保安装)
packages = [
    "base-files",
    "shadow",
    "bash",
    "loom",
    "eudev",
    "glibc",
    "gnu-coreutils",
    "kmod",
    "dhcpcd",
    "openssh",
    "neovim",
    "ripgrep"
]
```

---

## 2. 字段语义与约束

### 2.1 `[system]`
- `architecture`: 主机基础架构。
- `profile`: 活动 Profile 名称，所有 Channel 安装的文件软链接将聚合至 `/etc/sage/profiles/<profile>/`。

### 2.2 `[providers]` (动态虚拟接口提供者映射)
- **零硬编码接口**: 系统不硬编码固定的虚拟接口枚举，`[providers]` 表现为动态的 `HashMap<String, String>`（`interface -> provider_pkg`）。
- **非 providers 范围**: 诸如 Shell（`/bin/sh`、`bash`、`zsh`）等基础命令为标准独立软件包，由常规包依赖或 `alternatives` 机制管理，不通过 `[providers]` 进行互斥锁定。
- **Provider binding**: A configured provider is a required, exact `name[:slot]` binding in `main/system`; an omitted slot means `0`. Both install and rebuild reject incompatible bindings instead of switching implementations.
- **initramfs provider**: `initramfs-generator` is optional system policy. The
  selected provider package must carry its own package trigger with the
  provider-specific command-line interface; Sage does not assume mkinitcpio,
  dracut, or any other generator.
- **原子状态切换**: 当用户将 `init = "loom"` 修改为 `init = "systemd"` 并执行 `sage rebuild` 时，系统自动计算差集，完成旧包卸载、新包安装以及全量服务配置重编译。

### 2.3 `packages` 与服务管理解耦 (`/etc/sage/services.toml`)
- `packages`: 系统声明式常驻包列表。
- **服务配置解耦**: 服务不存放在 `system.toml` 中，由独立的 `/etc/sage/services.toml` (`ServicesConfig`) 维护，定义 **Sage 声明式管理的服务激活状态 (Sage-managed service activation state)**：
  ```toml
  # /etc/sage/services.toml
  schema_version = 1
  enabled = [
      "sshd",
      "udev",
      "dhcpcd"
  ]
  ```
  在执行 `sage rebuild` 时，系统加载 `system.toml` 获得声明式软件包与 provider，并加载 `services.toml` 获得需激活的服务列表，由对应 Init rclass 模板引擎编译生成服务配置。未声明的服务属于 `unmanaged` 状态，遵循**“Sage 只撤销自己创建的状态，不擅自撤销管理员在 Sage 外部创建的状态”**的不变量。详见 [service_spec.md](service_spec.md)。

### 2.4 Provider selection and retained packages

Each configured provider declares a required virtual interface and an exact
concrete package identity. The binding includes channel and slot and cannot
backtrack to another implementation or slot. Version selection can still
backtrack within that identity. Bindings apply only to requirements routed to
the same repository system channel; a `main/system` policy cannot constrain
`vendor/system` dependencies. During rebuild, an otherwise unused configured interface is
added as a virtual root so its selected release must actually provide the symbol.

`sage install -P interface=package[:slot]` (or `--provider`) overrides a binding
in the requested repository's system channel for the transaction. Only
`main/system` choices are saved in the system provider table.
`virtual/interface` and exact shared-library symbols such as
`so:libfoo.so.1` are accepted as interface spellings;
empty, malformed and duplicate overrides are rejected. Direct `virtual/interface`
requests are resolved as virtual edges, not unchecked concrete package roots.
An omitted virtual slot allows any compatible provider slot; an explicit `:0`
restricts selection to slot `0`, just like any other explicit slot. Installation,
upgrade, and saved concrete roots preserve that distinction.
Automatic choices come from a satisfiable dependency graph. Interactive choices
are restricted to providers that can satisfy the entire graph; absent a terminal,
the solver's feasible choice is used. Automatic bindings retain the full
requirement, including channel, slot, and version constraints. Only unqualified interfaces with one consistent provider
choice are saved as global policy in successful main/system installs unless
`--no-save` is set. Slot-specific dependency choices remain local to the solved
graph; directly requested scoped virtual packages are saved as concrete
`name:slot` package roots so a later rebuild retains them without imposing a
global binding on other slots.
`--dry-run` reports the choices without modifying declarations. Installing a
provider changes the package/declaration; `sage rebuild` activates system provider
bindings and renders services transactionally.

Normal rebuilds retain installed packages outside `main/system`; `--no-prune`
retains all installed package identities. Retention preserves channel/name/slot
roots, not exact version pins. Installed versions remain preferred candidates,
including releases no longer present in the repository. PubGrub may move those
versions and their dependencies when other desired roots require compatible
releases. A normal pruning rebuild may remove an obsolete provider when no
desired or retained package requires it. `--no-prune` does not authorize removing
a conflicting retained package: an unsatisfiable combined graph fails planning.
