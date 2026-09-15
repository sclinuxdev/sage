# 模块实现: 系统调和、多 Init 服务与 Channel 聚合 (`sage-sys`)

- **Crate 路径**: `crates/sage-sys`
- **代码预算**: ~1,600 行
- **职责**: 驱动 `rclass` 声明式 Init 模板引擎、声明式 Glob 触发器批处理器、多版本化 Sub-channel 独立管理、Channel Profile 软链聚合与 `sage rebuild` 状态调和。

---

## 1. 声明式触发器执行引擎 (`TriggerEngine`)

**彻底零硬编码触发器**：`sage-sys` 内部不包含任何针对特定命令的预设代码，全部由外部 TOML 规范驱动：

```rust
pub struct TriggerSpec {
    pub name: String,
    pub description: String,
    pub on_paths: Vec<String>,       // Glob 匹配模式
    pub exec: Vec<String>,           // 触发执行命令与参数
    pub priority: u32,               // 排序优先级
    pub ignore_missing_binary: bool, // 是否在命令不存在时静默跳过
}

pub struct TriggerEngine;

impl TriggerEngine {
    pub fn load_triggers(sysroot: &Path) -> Result<Vec<TriggerSpec>, SysError>;
    pub fn execute_triggers(
        triggers: &[TriggerSpec],
        modified_paths: &[PathBuf],
        sysroot: &Path,
    ) -> Result<(), SysError>;
}
```

- 事务完成后，自动根据受影响的物理路径对所有触发器进行 Glob 模式匹配并按 `priority` 聚合执行。

---

## 2. 模板化 Init 服务生成器 (`TemplateServiceGenerator`)

**彻底零硬编码 Init**：`sage-sys` 仅实现通用的模板渲染引擎：

```rust
pub struct TemplateServiceGenerator {
    pub target_path_template: String,
    pub mode: u32,
    pub template: String,
    pub dependency_aliases: BTreeMap<String, String>,
    pub service_dependency_suffix: String,
    pub supported_types: Vec<String>,
    pub compile_command: Vec<String>,
    pub managed_directory: Option<String>,
    pub validate_command: Option<String>,
    pub enable_command: Option<String>,
    pub disable_command: Option<String>,
    pub is_enabled_command: Option<String>,
    pub activations: BTreeMap<String, TemplateActivationAdapter>,
}

pub struct TemplateActivationAdapter {
    pub automatic: bool,
    pub service_template: Option<String>,
    pub artifacts: Vec<TemplateArtifact>,
    pub enable_command: Option<String>,
    pub disable_command: Option<String>,
    pub is_enabled_command: Option<String>,
}
```

### 2.1 跨 Init 服务激活模型 (Cross-Init Service Activation)
- **直接服务 (`kind = "service"`)**：默认模式。由 `enable_cmd` / `disable_cmd` 管理系统开机启动状态。
- **套接字激活 (`kind = "socket"`)**：声明 UNIX stream 监听套接字（`listen_stream`, `accept = false`, `mode`），遵循 `sd-listen-fds` 协议描述符传递契约。生命周期动作（enable/disable/query）自动映射至监听器单元（如 systemd 的 `<name>.socket` 或 Loom 对应激活节点）。
- **系统总线激活 (`kind = "dbus"`)**：声明已知系统 D-Bus 名称（`name`, `bus = "system"`, `user"`）。随包安装自动就绪（`automatic = true`），严禁写入 `services.toml` 的 `enabled`/`disabled` 避免非法引导状态。

### 2.2 双阶段服务生成与原子回滚 (`CommitGuard`)
在 `render_service_set` 批量渲染服务时（特别是管理整体服务目录的 Loom 或生成额外激活附加产物的 systemd/D-Bus）：
1. **暂存与备份**：主服务文件通过进程级临时文件原子写出；被覆盖的已存在激活描述符自动备份至内存；受管目录创建全新临时工作副本。
2. **两阶段提交与守卫**：渲染全部服务并执行 `validate_command`（如 `/usr/lib/loom/loom validate`）。若任一服务渲染失败或验证命令非零退出，`CommitGuard` 自动触发回滚：清空或还原受管目录，并将已覆盖的激活描述符全量恢复，防止磁盘残留坏死或半生不熟的服务定义。

### 2.3 已安装元数据优先原则 (Metadata Priority)
`load_installed_services` 与 `load_rendered_services` 严格分离。调和与生命周期操作优先读取当前已安装包释放的 `.METADATA/service.toml`，仅对未随包携带新元数据的历史服务采用 `rendered-services.toml`，确保包升级后新服务契约立即生效，彻底杜绝已废弃历史渲染规范覆盖新规范。

---

## 3. 多版本化 Sub-channel 运行时管理与 Profile 聚合

1. **多版本 Python/工具链运行时共存**:
   - 每个自带版本的 Sub-channel（如 `python3.12`、`python3.13`、`gcc14`、`gcc15`）在 `/opt/channels/<subchannel>/` 下拥有独立的物理根。
2. **环境激活与 Shell 导出 (`sage shell`)**:
   - `sage shell --with python3.12` 自动将 `/opt/channels/python3.12/site-packages` 注入当前 Shell 会话的 `PYTHONPATH`。
3. **活动 Profile 软链切换与孤儿链接清理 (`ProfileEngine`)**:
   - `/etc/sage/profiles/<profile>/bin` 集中汇总系统活动工具链。
   - 切换活动编译器（如从 `gcc14` 切换为 `gcc15`）仅需原子更新软链接指向 `/opt/channels/gcc15/bin/gcc`，耗时 < 1ms。
   - **孤儿链接自动清理 (Orphan Pruning)**：在应用新 Profile 时，引擎自动扫描该 Profile 目录下所有指向 `/opt/channels/` 的旧软链接。任何在新配置中已被剔除的工具链残留链接均被安全自动清除，防止悬空或失效的旧工具链入口长期留存。


---

## 4. 声明式系统调和 (`Reconciler`)

`sage rebuild` 执行流水线：
1. 读取 `/etc/sage/system.toml`（包集合与 provider 映射）与 `/etc/sage/services.toml`（启用服务集合）。
2. 比对 LMDB 中当前已安装的 `(Channel, PackageName, Slot)` 集合。
3. 动态加载目标 Init 系统的 `init-*.toml` rclass 渲染器。
4. 调度 `sage-solver` 执行依赖求解，调度 `sage-archive` 执行两阶段文件原子交接。
5. 通过 `TemplateServiceGenerator` 渲染并激活全量服务。
6. 调用 `TriggerEngine` 扫描所有外部触发器并完成批量触发。

The trigger engine exposes three auditable lifecycle boundaries: post-change,
post-remove, and rebuild. Removal snapshots declarations before owned files
disappear; rebuild handlers run after package, provider, and service convergence.

The shipped trigger directory supplies generic declarations for `ldconfig`,
`depmod`, sysusers, GLib schemas, MIME data, icon themes, fonts, desktop entries,
and GIO modules. A unit test parses the complete directory and rejects invalid or
duplicate declaration names.

---

## 5. 基于 Dirfd 的安全文件系统操作 (`sage-sys::fs`)

针对目标 `sysroot` 的变动操作，`sage-sys` 全面采用基于目录文件描述符（`dirfd`）的锚定安全原语：
- **`open_root`**: 以 `O_DIRECTORY | O_CLOEXEC` 打开目标 sysroot，后续所有相对路径操作严格锚定于该根 fd。
- **`clean_cache_beneath`**: 扫描并清理缓存目录时，以 `O_DIRECTORY | O_NOFOLLOW` 递归向下展开。如果遇到符号链接，直接将其视为叶子节点解绑，严禁跨越符号链接递归进入宿主机物理文件系统。
- **`unlink_beneath`**: 服务卸载与残留清理通过 `unlinkat` 锚定执行，杜绝跨目录层级的非预期删除。
- **`write_atomic_under_root`**: 针对 `/etc/sage/system.toml` 与 `/etc/sage/services.toml` 的状态持久化，在目标同级目录下创建临时文件并进行 `renameat` 原子替换，消除 TOCTOU 窗口并保证并发一致性。

