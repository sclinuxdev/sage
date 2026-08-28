# 规范: 软件源远端 LMDB 索引 (`index.mdb.zst` v1)

- **文件位置**: `<channel_url>/index.mdb.zst` 与 `<channel_url>/index.mdb.sig`
- **Schema 版本**: `1`
- **设计目标**: 全链路统一基于 **LMDB (`heed`)**，实现软件源索引的零反序列化开销、内存映射 (mmap) 与微秒级按需查询。

---

## 1. 软件源 LMDB 数据库表布局 (`index.mdb`)

软件源发布端将所有包元数据直接编译为 LMDB B+ 树数据库 `index.mdb`，并划分如下专用表 (Named DBIs)：

| 表名 (DBI) | Key 格式 | Value 格式 | 核心用途与查询优化 |
| :--- | :--- | :--- | :--- |
| `packages` | `name:slot` (例如 `ripgrep:0`) | `PackageManifest` (Bincode 紧凑二进制) | 零拷贝内存映射直接切片读取单包元数据 |
| `provides` | `symbol` (例如 `so:libpcre2-8.so.0`) | `Vec<String>` (`name:slot` 列表) | 虚拟提供者与动态库符号纳秒级反查 |
| `dependencies` | `name:slot` (例如 `ripgrep:0`) | `Vec<Dependency>` (依赖约束列表) | 依赖求解器按需展开，杜绝全量加载 |
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
3. **求解器零拷贝消费 (`sage-solver`)**:
   - PubGrub 在推进依赖图时，仅对访问到的符号进行 B+ 树快速查找，内存开销 < 1MB，耗时 < 100ns。
