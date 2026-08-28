# 模块实现: LMDB 状态存储与事务日志 (`sage-db`)

- **Crate 路径**: `crates/sage-db`
- **选用生态**: `heed` (LMDB Rust 绑定)
- **代码预算**: ~1,100 行
- **职责**: mmap 支持的 LMDB 持久化、版本化 Sub-channel 实例追踪、文件所有权与可重放事务日志。

---

## 1. 数据库布局与命名表 (Named DBI)

持久化路径：`/var/lib/sage/data.mdb`

| 表名 (DBI) | Key | Value | 极致性能特性 |
| :--- | :--- | :--- | :--- |
| `packages` | `pkg_key` (`"channel:name:slot"`) | `InstalledPackage` (Bincode) | mmap 点查后解码为所有权对象 |
| `files` | `rel_path: &str` | `String` (换行符分隔的 `channel:name:slot`) | 极速所有权匹配与冲突检测 |
| `provides` | `symbol: &str` | `Vec<String>` (实例标识列表) | 虚拟提供者与 SONAME 符号索引 |
| `system` | `interface: &str` | `String` (激活的包名与 Slot) | 声明式系统核心提供者锁定 |
| `operations` | `op_id: &str` | `JournalRecord` | 未完成事务日志 |

---

## 2. 版本化子通道所有权隔离机制

1. **天然物理隔离**:
   - `python3.12` 与 `python3.13` 的包分别存储在 `/opt/channels/python3.12/site-packages` 与 `/opt/channels/python3.13/site-packages`。
   - `gcc14` 与 `gcc15` 分别存储在 `/opt/channels/gcc14` 与 `/opt/channels/gcc15`。
   - `files` 表记录例如：
     ```text
     opt/channels/python3.12/site-packages/numpy/__init__.py -> main/python3.12:numpy:0
     opt/channels/python3.13/site-packages/numpy/__init__.py -> main/python3.13:numpy:0
     ```
   - 相对路径完全不同，天然杜绝文件冲突。
2. **独立升级与回滚**:
   - 升级 `python3.13` 上的包不会对 `python3.12` 的环境产生任何副作用。

---

## 3. 崩溃恢复状态机 (`operations` 表)

`sage` 启动时扫描 `operations` 表并验证 `journal_sha256`。journal 保存安装版本、删除快照和声明式元数据，可从 packages、alternatives、triggers 阶段继续；重复的文件与 LMDB 操作均为幂等操作。
