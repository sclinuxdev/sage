# 模块实现: 流式归档与安全解包 (`sage-archive`)

- **Crate 路径**: `crates/sage-archive`
- **选用生态**: `tar`, `zstd`, `nix` / `libc`
- **代码预算**: ~800 行
- **职责**: 流式创建/解压 `*.pkg.tar.zst`，提供防路径逃逸的 `openat` 安全解包模型与三方配置冲突保护。

---

## 1. 恒定代价检视 (`inspect_package`)

流式读取 Zstandard 解压流：
- 逐个匹配 Tar Entry 头部。
- 读取 `.METADATA/manifest.toml`、`files.idx`、`service.toml`、`triggers.toml`。
- 一旦检测到 `data/` 目录立即停止解包，耗时 < 1ms。

---

## 2. 基于 `openat` / `dirfd` 的防逃逸解包

彻底杜绝 `..` 路径穿越与符号链接逃逸攻击：

```text
sysroot ──── fd_root (open(O_DIRECTORY | O_CLOEXEC))
    │
    ├─► 逐级校验并进入子目录 ──── fd_dir (openat(fd_cur, sub, O_NOFOLLOW))
    │
    └─► 创建临时文件 ─────────── fd_tmp (openat(fd_dir, ".tmp.XXX", O_CREAT | O_EXCL))
          │
          └─► 原子替换 ──────── renameat(fd_dir, ".tmp.XXX", fd_dir, "target_file")
```

### 安全规则
1. **`O_NOFOLLOW` 强制限制**: 打开中间路径若遇到软链接，拒绝跳转并报错。
2. **完整性验证**: 解压过程中比对 `files.idx` 中声明的 SHA-256 校验和。
3. **权限与时间戳还原**: 解压完成后设置文件 `mode` (如 `0755`) 与 `mtime`。

---

## 3. 三方哈希比对与配置文件冲突保护 (`.sage-new`)

在升级涉及 `/etc/` 目录下的配置文件时，`sage-archive` 协同 `sage-db` 采用三方哈希仲裁规则：

| 现场文件状态 (Live SHA) | 历史安装状态 (DB SHA) | 新包配置状态 (New SHA) | 动作决策 |
| :--- | :--- | :--- | :--- |
| 未修改 (`Live == DB`) | 任何 | 任何 (`New`) | **静默覆盖**: 安全升级至新配置 |
| 用户已修改 (`Live != DB`) | 历史原始 | 无变化 (`New == DB`) | **保持现有**: 继续使用用户修改版本 |
| 用户已修改 (`Live != DB`) | 历史原始 | 上游有更新 (`New != DB`) | **保护现有并写出 `<file>.sage-new`**: 不破坏现场配置，向终端发出 Diff 审查提示 |
