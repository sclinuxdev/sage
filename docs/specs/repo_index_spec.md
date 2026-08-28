# 规范: 软件源远端 LMDB 索引 (`index.mdb.zst` v1)

- **文件位置**: `<channel_url>/index.mdb.zst` 与 `<channel_url>/index.mdb.sig`
- **Schema 版本**: `1`
- **设计目标**: 基于 **LMDB (`heed`)** 提供 mmap B+ 树点查和紧凑 Bincode 值；当前安装流程会有序扫描并解码候选，再按架构过滤。

---

## 1. 软件源 LMDB 数据库表布局 (`index.mdb`)

软件源发布端将所有包元数据直接编译为 LMDB B+ 树数据库 `index.mdb`，并划分如下专用表 (Named DBIs)：

| 表名 (DBI) | Key 格式 | Value 格式 | 核心用途与查询优化 |
| :--- | :--- | :--- | :--- |
| `packages` | `name:slot` (例如 `ripgrep:0`) | `Vec<IndexedRelease>` (Bincode) | 点查后解码该 name/slot 的版本集合 |
| `provides` | `symbol` (例如 `so:libpcre2-8.so.0`) | `Vec<String>` (`name:slot` 列表) | 虚拟提供者与动态库符号纳秒级反查 |
| `dependencies` | `name:slot` (例如 `ripgrep:0`) | 最新版本的 `Vec<Dependency>` | 索引辅助表；当前求解器使用 packages 记录中的依赖 |
| `metadata` | `"channel"` / `"arch"` / `"timestamp"` | 字符串或 64 位整数 | 软件源全局元信息 |

---

## 2. 同步、验签与加载流程

1. **发布端构建**:
   - 遍历配方产物生成 `index.mdb`。
   - 使用私钥对 `index.mdb` 签名生成 `index.mdb.sig`。
   - 通过 Zstandard 压缩为 `index.mdb.zst` 上传镜像站。
2. **客户端同步 (`sage-repo`)**:
   - 下载 `index.mdb.zst` 与 `index.mdb.sig`。
   - 流式 Zstd 解压至缓存目录，并通过 Ed25519 公钥直接验证 `index.mdb` 的完整性与真实性。
   - 验签通过后，客户端通过 `heed::EnvOpenOptions` 以 **只读内存映射 (`MDB_RDONLY | MDB_NOLOCK`)** 模式打开。
3. **当前求解器消费 (`sage-solver`)**:
   - CLI 有序扫描启用索引，解码 release 记录，过滤不兼容架构后构建内存候选集合。按需 LMDB 展开需要未来的基准与实现支持。
