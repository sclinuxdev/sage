# 规范: 声明式系统配置 (`/etc/sage/system.toml` v1)

- **文件路径**: `/etc/sage/system.toml`
- **Schema 版本**: `1`
- **设计目标**: 定义目标 Linux 系统的最终期望状态，作为 `sage rebuild` 状态调和的唯一真相源。

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

# 全局启用的服务列表 (Rebuild 时将自动由 rclass 编译并激活)
services = [
    "sshd",
    "udev",
    "dhcpcd"
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
- **求解器优先权**: 当求解器在依赖图中遇到 `virtual/<interface>` 符号时，自动以最高权重（+1000）选取 `[providers]` 中指定的提供者包。
- **原子状态切换**: 当用户将 `init = "loom"` 修改为 `init = "systemd"` 并执行 `sage rebuild` 时，系统自动计算差集，完成旧包卸载、新包安装以及全量服务配置重编译。

### 2.3 `packages` 与 `services`
- `packages`: 系统声明式常驻包列表。
- `services`: 开机自启的服务名列表，对应各包携带的 `.METADATA/service.toml`。
