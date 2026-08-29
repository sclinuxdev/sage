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
exec = ["/usr/sbin/ldconfig", "-X"]
priority = 10                         # 执行优先级 (数值小者先执行)
ignore_missing_binary = true          # 若系统中尚未安装 /usr/sbin/ldconfig 则安全忽略
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

### 示例 4: `depmod.toml`

```toml
schema_version = 1
name = "depmod"
description = "Regenerate dependency maps for changed kernel module slots"
on_paths = ["usr/lib/modules/**"]
exec = ["/usr/bin/depmod", "-a", "${path[3]}"]
priority = 20
ignore_missing_binary = true
```

`${path}` 展开为匹配的事务相对路径，`${path[N]}` 展开为从零开始的路径组件，`${sysroot}` 展开为事务目标根。引擎不调用 shell、关闭标准输入，并对展开后的完整命令去重；同一事务写入同一内核 Slot 的多个 `.ko` 文件只运行一次 `depmod`，不同 Slot 则各运行一次。

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
2. **去重与合并**: 事务涉及 100 个 `.so` 文件时，`ldconfig` 仅在事务末尾**被聚合调用执行 1 次**；包含路径变量的命令按展开结果分别去重。

---

## 3. 声明式系统用户 (`sysusers`)

包在配方中声明 `[[sysusers]]`，打包时生成仅由 Sage 解释的
`.METADATA/sysusers.toml`：

```toml
[[sysusers]]
package = "redis-server" # optional subpackage owner
type = "user"
name = "redis"
id = 75
description = "Redis Database Server"
home = "/var/lib/redis"
shell = "/usr/bin/nologin"
```

发布事务把声明绑定到完整包身份并存入 `/usr/share/sage/sysusers/`。
Sage 自身随后以确定顺序合并 `/etc/passwd`、`/etc/group` 和
`/etc/shadow`；无需 init provider 或外部 sysusers 实现。显式数字 ID
冲突会使事务失败，省略 ID 时则从 Sage 的动态系统账户区间稳定分配。

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

## 4. Declarative lifecycle boundaries

Triggers may declare `events = ["post-change", "post-remove", "rebuild"]`.
Omitting the field preserves schema-v1 compatibility and enables post-change and
post-remove. Package declarations are retained in memory through removal and run
only after filesystem and ownership publication. Standard input stays closed,
arguments expand without a shell, and identical commands are deduplicated.

| Legacy hook purpose | Sage declaration |
| :--- | :--- |
| create users or groups | `[[sysusers]]` and the sysusers post-change trigger |
| rebuild caches or ABI maps | path trigger on post-change and post-remove |
| register and enable a daemon | `service.toml` plus init rclass on rebuild |
| select command providers | alternatives declaration |
| interactive configuration | administrator-owned `/etc` state |

Arbitrary pre-install execution remains forbidden. Preconditions belong in
solver constraints and archive validation; package code before publication would
break reproducibility and rollback safety.

## 5. Standard trigger library

The base data library covers dynamic linker maps, kernel modules, system users,
GLib schemas, the shared MIME database, icon themes, fontconfig caches, desktop
application handlers, and GIO modules. Each declaration is an independent TOML
file, so distributions can override or replace commands without rebuilding Sage.

Initramfs regeneration is deliberately not part of this base library. A kernel
only declares `virtual/initramfs-generator`; the selected provider package must
ship `.METADATA/triggers.toml` named `initramfs-generator` and use its own
command-line contract. For example, a mkinitcpio package may use `mkinitcpio
-P`, while a dracut package may use `dracut --regenerate-all`. Sage never assumes
that all systems use either implementation.

All cache refreshes run after change and removal, use `${sysroot}` rather than
assuming `/`, and collapse identical expanded commands. Theme- and ABI-specific
triggers derive their cache directory from validated path components.
