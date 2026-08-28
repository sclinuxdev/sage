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
    pub validate_command: Option<String>,
}

impl TemplateServiceGenerator {
    pub fn from_rclass(rclass_path: &Path) -> Result<Self, SysError>;
    pub fn render_service(&self, svc: &ServiceSpec, sysroot: &Path) -> Result<(), SysError>;
}
```

---

## 3. 多版本化 Sub-channel 运行时管理与 Profile 聚合

1. **多版本 Python/工具链运行时共存**:
   - 每个自带版本的 Sub-channel（如 `python3.12`、`python3.13`、`gcc14`、`gcc15`）在 `/opt/channels/<subchannel>/` 下拥有独立的物理根。
2. **环境激活与 Shell 导出 (`sage shell`)**:
   - `sage shell --with python3.12` 自动将 `/opt/channels/python3.12/site-packages` 注入当前 Shell 会话的 `PYTHONPATH`。
3. **活动 Profile 软链切换 (`ProfileEngine`)**:
   - `/etc/sage/profiles/default/bin` 集中汇总系统活动工具链。
   - 切换活动编译器（如从 `gcc14` 切换为 `gcc15`）仅需原子更新软链接指向 `/opt/channels/gcc15/bin/gcc`，耗时 < 1ms。

---

## 4. 声明式系统调和 (`Reconciler`)

`sage rebuild` 执行流水线：
1. 读取 `/etc/sage/system.toml`。
2. 比对 LMDB 中当前已安装的 `(Channel, PackageName, Slot)` 集合。
3. 动态加载目标 Init 系统的 `init-*.toml` rclass 渲染器。
4. 调度 `sage-solver` 执行依赖求解，调度 `sage-archive` 执行两阶段文件原子交接。
5. 通过 `TemplateServiceGenerator` 渲染并激活全量服务。
6. 调用 `TriggerEngine` 扫描所有外部触发器并完成批量触发。

The trigger engine exposes three auditable lifecycle boundaries: post-change,
post-remove, and rebuild. Removal snapshots declarations before owned files
disappear; rebuild handlers run after package, provider, and service convergence.
