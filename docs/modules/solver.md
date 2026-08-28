# 模块实现: PubGrub 依赖求解与因果诊断 (`sage-solver`)

- **Crate 路径**: `crates/sage-solver`
- **选用生态**: `pubgrub` crate, `heed` (LMDB 读取)
- **代码预算**: ~900 行
- **职责**: 适配 PubGrub 依赖求解器，直接对远端与本地 **LMDB 索引库** 执行零拷贝按需查询，原生支持版本化 Sub-channel 隔离、跨通道基础继承与因果诊断树。

---

## 1. 原生多版本 Sub-channel 与跨通道继承

在 `pubgrub::solver::DependencyProvider` 中：
- **Package 标识**: `PackageKey` (`channel:name:slot`)，例如 `main/python3.12:numpy:0` 与 `main/python3.13:numpy:0`。
- **子通道作用域与 System 基础继承**:
  - 当在 `python3.12` 中安装包时，其 Python 专属依赖解析局限在 `python3.12` 上下文中。
  - **动态库自动穿透**: 子通道自动继承 `system` 根通道作为底层基础依赖作用域，Python C 扩展声明的 `virtual/libc` 或 `so:libopenblas.so.3` 可直接从 `system` 通道中解析满足，无需手动指定前缀。
- **版本隔离**: `python3.12` 与 `python3.13` 的依赖求解相互正交，各自独立决策。
- **Slot/版本内排他**: 同一 `channel:name:slot` 下仅允许单版本（互斥升级）。

---

## 2. 极致性能：整数化与预筛 (High Performance Pruning)

1. **Interned 符号匹配**: 将所有 `Channel`（如 `main/python3.12`）、`Name`、`Slot` 与依赖约束在进入求解主循环前完成符号整数化映射，热路径比较全部为整型比较。
2. **候选打分策略**:
   - 系统锁定目标优先 (+1000)。
   - 同名包优先 (+100)。
   - 版本倒序（优先选择最新满足约束者）。

---

## 3. 因果诊断树输出

当依赖发生不可调和冲突时，PubGrub 的 Incompatibility 树被转换为直观因果树输出：
```text
无法在 Sub-channel 'main/python3.12' 中求解依赖图:
├── 包 app-x 需要 python-requests (channel=python3.12, ver >= 2.30.0)
└── 仓库中最高可用版本为 python-requests 2.28.0
```

Build environments use `resolve_dependencies(channel, constraints)` with a
synthetic root. Repeated constraints intersect before PubGrub begins. Feature
runtime dependencies are folded into package manifests, so installation retains
the same candidate ordering, backtracking, and causality reporting.
