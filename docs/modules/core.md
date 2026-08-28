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
