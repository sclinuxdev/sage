# 规范: 声明式触发器、系统用户与替代项 (`triggers` v1)

- **触发器文件位置**: 
  - 系统/通道级触发器: `/usr/share/sage/triggers/*.toml`
  - 管理员自定义覆盖: `/etc/sage/triggers.d/*.toml`
  - 软件包专属触发器: `.METADATA/triggers.toml`
- **Schema 版本**: `1`
- **核心原则**: **零硬编码触发器**。系统内部不内嵌任何特化触发逻辑（如 ldconfig、ca-certificates、font-cache 等），所有触发器均通过声明式 TOML 文件定义。

---

## 1. 声明式触发器规范 (`triggers/*.toml`)

每个触发器为一个独立的声明文件：

### 示例 1: `ldconfig.toml` (`/usr/share/sage/triggers/ldconfig.toml`)
```toml
schema_version = 1
name = "ldconfig"
description = "Update dynamic linker runtime bindings"
# 当事务中新增/修改/删除匹配下列 glob 路径的文件时激活
on_paths = [
    "usr/lib/*.so*",
    "lib/*.so*",
    "etc/ld.so.conf.d/*"
]
exec = ["/sbin/ldconfig", "-X"]
priority = 10                         # 执行优先级 (数值小者先执行)
ignore_missing_binary = true          # 若系统中尚未安装 /sbin/ldconfig 则安全忽略
```

### 示例 2: `ca-certificates.toml`
```toml
schema_version = 1
name = "ca-certificates"
description = "Update SSL/TLS certificate bundles"
on_paths = [
    "etc/ssl/certs/*",
    "usr/share/ca-certificates/*"
]
exec = ["/usr/sbin/update-ca-certificates"]
priority = 50
ignore_missing_binary = true
```

### 示例 3: `mime-database.toml`
```toml
schema_version = 1
name = "mime-database"
description = "Update MIME database cache"
on_paths = [
    "usr/share/mime/*"
]
exec = ["/usr/bin/update-mime-database", "/usr/share/mime"]
priority = 60
ignore_missing_binary = true
```

---

## 2. 触发器引擎执行流水线 (`TriggerEngine`)

```text
事务结束 (Payload 已写入，LMDB 事务已提交)
                   │
                   ▼
  扫描 /etc/sage/triggers.d/ 与 /usr/share/sage/triggers/
                   │
                   ▼ (将变动文件路径与 on_paths 进行 Glob 模式匹配)
         提取被激活的触发器集合
                   │
                   ▼ (按 priority 升序排序与去重)
  依次执行各触发器 exec 命令 (受 ignore_missing_binary 保护)
```

1. **完全动态注册**: 安装携带新特性的包（如 Python、GTK、Fonts）会自动在 `/usr/share/sage/triggers/` 中落地新的触发器，即刻对后续事务生效，`sage` 二进制代码无需任何改动。
2. **去重与合并**: 事务涉及 100 个 `.so` 文件时，`ldconfig` 仅在事务末尾**被聚合调用执行 1 次**。

---

## 3. 声明式系统用户 (`sysusers`)

包在配方中声明 `[[sysusers]]`，打包时生成 `usr/lib/sysusers.d/<pkgname>.conf`：

```toml
[[sysusers]]
type = "user"
name = "redis"
id = 75
description = "Redis Database Server"
home = "/var/lib/redis"
shell = "/usr/bin/nologin"
```

事务后，`sage-sys` 优先调用 `systemd-sysusers`，若不存在则回退调用标准 `useradd` / `groupadd` 工具以幂等方式创建系统账号。

---

## 4. 软链接替代项仲裁 (`alternatives`)

支持多个包提供同名命令（如 `vi`, `cc`, `awk`）：

```toml
[[alternatives]]
link = "usr/bin/vi"
target = "vim"
priority = 50
```

- **仲裁逻辑**: `sage-sys` 维护各 link 的所有已安装提供者及其优先级，始终将物理软链接指向优先级最高者。
- **自动降级**: 当卸载当前处于激活态的提供者时，自动原子降级指向剩余候选者中优先级最高的一项。
