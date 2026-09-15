# 模块实现: 基础核心模型与锁 (`sage-core`)

- **Crate 路径**: `crates/sage-core`
- **代码预算**: ~800 行
- **职责**: 基础领域实体、版本代数算法、版本化 Sub-channel 与 Slot 多版本模型、符号 Interning、Schema 结构与主机互斥锁。

---

## 1. 核心实体与多版本实例标识 (`PackageKey`)

为了在逻辑上原生支持**子通道自带版本**以及**跨版本环境独立共存**，实体的唯一实例主键为 `(Channel, Name, Slot)`：

```rust
#[derive(Debug, Clone, PartialEq, Eq, Hash, PartialOrd, Ord, Serialize, Deserialize)]
pub struct PackageKey {
    pub channel: String, // 版本化通道标识 (如 "main/system", "main/python3.12", "main/python3.13", "main/gcc15")
    pub name: String,    // 包名 (如 "numpy", "gcc", "ripgrep")
    pub slot: String,    // Slot 槽位标识 (默认为 "0")
}

#[derive(Debug, Clone, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub struct Version {
    pub epoch: u32,
    pub upstream: String,
    pub release: u32,
}
```

### 极致性能设计：符号 Interning
在求解器与数据库热路径上，所有的 `Channel`（如 `"main/python3.12"`）、`Name`、`Slot` 字符串全部映射为 32 位整型索引（`SymbolId`），哈希与比对开销降低至纳秒级。

---

## 2. 依赖约束模型 (`Dependency`)

```rust
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub enum ConstraintOp {
    Any,
    Equal,          // =
    NotEqual,       // !=
    Greater,        // >
    GreaterOrEqual, // >=
    Less,           // <
    LessOrEqual,    // <=
}

#[derive(Debug, Clone, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub struct Dependency {
    pub name: String,
    pub slot: Option<String>,
    pub channel: Option<String>, // 可显式限定特定版本子通道（如 "python3.12"）
    pub op: ConstraintOp,
    pub version: Option<Version>,
}
```

---

## 3. 全局主机锁 (`HostLock`)

- **锁路径**: `/run/sage/operation.lock`
- **锁行为**:
  - 只读与 `--dry-run`: 获取共享锁 (`flock(LOCK_SH)`)。
  - 安装、卸载、Rebuild: 获取独占排他锁 (`flock(LOCK_EX)`).
- **RAII Guard**: 零锁泄漏，析构即自动释放。

---

## 4. 版本代数算法与前置发布排序 (`Version` & `~` Pre-release)

Sage 版本由 `epoch:upstream-release` 构成。在版本比对逻辑中原生支持 Debian/RPM 风格的 `~` (波浪号) 预发布版本语义：
- **`~` 最小优先级规则**：`~` 字符的排序优先级低于空字符（字符串结尾）和任何其他 ASCII 字符。
- **典型排序序列**：
  `1.0~~ < 1.0~ < 1.0~beta < 1.0~rc1 < 1.0 < 1.0.1`
- **分段比对状态机**：版本字符串交替切分为数字段与非数字段。数字段按数值大小比对（忽略前导零），非数字段按字符比对，遇 `~` 时强制提前判定为较低版本。

---

## 5. 虚拟符号体系与依赖分类 (`is_virtual_symbol`)

Sage 支持三类虚拟与提供者接口符号：
1. **虚拟接口 (`virtual/<name>`)**：如 `virtual/init`, `virtual/awk`。
2. **共享库动态符号 (`so:<soname>`)**：如 `so:libc.so.6`, `so:libssl.so.3`。
3. **命令提供者符号 (`cmd:<command>`)**：如 `cmd:bash`, `cmd:grep`。

`sage_core::is_virtual_symbol(symbol: &str) -> bool` 与 `Dependency::is_virtual(&self) -> bool` 统一识别上述三类符号，引导求解器将其映射为跨通道虚拟代理节点而非具象包名。

---

## 6. 包坐标与符号强校验 (`Package::validate`)

包坐标直接决定了数据库键、求解器 Universe 实体以及底层文件系统发布与解包路径。为了杜绝路径逃逸、越权覆盖与注入攻击，`sage-core` 在领域模型层确立了统一的安全校验原语：

1. **`valid_package_component(s: &str) -> bool`**:
   - 适用于包名（`name`）、槽位（`slot`）、架构（`arch`）。
   - 必须非空，仅允许 ASCII 字母、数字、`+`、`-`、`_`、`.`。
   - **严格禁止路径与目录穿越**：坚决拒绝 `.` 或 `..` 作为独立组件，拒绝包含 `/` 或 `\`。
2. **`valid_version_string(s: &str) -> bool`**:
   - 适用于上游版本字符串（`upstream`）。
   - 必须非空，包含可打印非空白字符，拒绝 `/`、`\` 以及 ASCII 控制字符。
3. **`valid_channel_name(s: &str) -> bool`**:
   - 适用于软件源通道标识（`channel`，如 `main/system`, `main/python3.12`）。
   - 按 `/` 拆分为多段，每段均必须满足非空且非 `.`/`..` 的组件约束，禁止以 `/` 开头或结尾。
4. **`valid_provider_symbol(s: &str) -> bool`**:
   - 验证虚拟接口提供者符号（如 `virtual/init`, `so:libc.so.6`, `cmd:bash`, `virtual/provider/init`）。
   - 拒绝空白、控制字符、路径分隔符及赋值符号。
5. **`Dependency::validate(&self) -> Result<(), CoreError>`**:
   - 校验依赖项自身：依赖名必须符合 `valid_provider_symbol`，通道必须符合 `valid_channel_name`，槽位必须符合 `valid_package_component`，版本约束必须合法有效。
6. **`Package::validate(&self) -> Result<(), CoreError>`**:
   - 综合递归校验整包元数据（channel, name, slot, arch, version, license，以及所有 dependencies, provides 与 conflicts），确立全局不可变安全边界。


