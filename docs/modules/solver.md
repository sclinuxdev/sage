# 模块实现: PubGrub 依赖求解与因果诊断 (`sage-solver`)

- **Crate 路径**: `crates/sage-solver`
- **选用生态**: `pubgrub` crate, `heed` (LMDB 读取)
- **代码预算**: ~900 行
- **职责**: 适配 PubGrub 依赖求解器，消费经过架构过滤的内存候选集合，支持 Slot、跨通道基础继承、provider 回溯与因果诊断树。

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

## 2. 候选预筛与排序 (Candidate Pruning)

1. **当前表示**: repository 记录先按目标架构过滤，再解码为拥有所有权的 `Package`/`PackageKey` 集合；求解热路径使用 Rust 字符串键，尚未使用 symbol interning。
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

---

## 4. 虚拟依赖与提供者符号解析体系 (Virtual Symbols & Proxy Keys)

Sage 将虚拟接口（`virtual/*`）、共享库（`so:*`）与命令提供者（`cmd:*`）统一抽象为由 `is_virtual_symbol` 识别的提供者符号：

1. **显式通道虚拟依赖 (Explicit Channel Virtuals)**:
   - 依赖项支持显式通道前缀（如 `extra/virtual/graphics`, `main/system/so:libc.so.6`, `main/system/cmd:sh`）。
   - 求解器优先使用依赖显式指定的通道；若未显式指定，则回退至父级包通道或对应的 `system` 基础通道。
2. **合成代理节点 (`__sage` Synthetic Proxy Keys)**:
   - 虚拟符号在求解器内部映射为合成代理包键：`PackageKey::new("__sage", format!("{channel}/{requirement}"), slot)`。
   - **通道嵌套防重入收敛**：在构造合成键前，强制清空 `requirement.channel = None`，杜绝产生如 `"main/system/main/system/virtual/..."` 的损坏递归通道标识。
3. **三类边界符号逆向解包 (`virtual_requirement`)**:
   - 求解器通过 `/virtual/`、`/so:` 以及 `/cmd:` 三向边界精准切分通道与原始需求，恢复用户约束并绑定具象发布。
4. **虚拟冲突反向索引消解 (Virtual Conflict Invalidation)**:
   - 当包声明与虚拟符号或库发生冲突（如 `conflicts = ["virtual/awk", "cmd:sh"]`）时，求解器通过 `universe.providers` 逆向查出同通道下提供该符号的所有具象包版本，并在这些版本上生成反向排他标记，保证因果树诊断能够精准定位虚拟冲突源头。

