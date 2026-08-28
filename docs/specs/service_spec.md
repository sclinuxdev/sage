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
