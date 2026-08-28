# 模块实现: 软件源同步与分块下载 (`sage-repo`)

- **Crate 路径**: `crates/sage-repo`
- **选用生态**: `reqwest` (rustls), `heed` (LMDB 绑定), `ed25519-dalek`, `zstd`
- **代码预算**: ~600 行
- **核心原则**: **统一 LMDB 存储**、零硬编码 URL、子通道 Base URL 自动派生、HTTP 条件缓存 (0 流量校验)、Ed25519 验签与 HTTP Range 流式并发下载。

---

## 1. 软件源同步与 LMDB 索引加载

### 1.1 条件同步与带宽优化 (Conditional Sync)
```rust
pub async fn sync_channel_index(
    channel: &ChannelConfig,
    cache_dir: &Path,
) -> Result<heed::Env, RepoError>;
```
1. **轻量条件探测**:
   - 客户端首先发起轻量 HTTP 请求检查远端 `index.mdb.sig` 的 HTTP ETag / Last-Modified 状态或拉取微小签名文件。
   - 若远端签名或哈希与本地缓存一致，**直接跳过大文件传输（0 流量消耗）**。
2. **原子更新**:
   - 若索引发生变化，流式下载 `index.mdb.zst`，解压至临时影子文件并由 `ed25519_dalek::Verifier` 进行公钥验签。
   - 验签通过后，通过原子重命名替换 `/var/cache/sage/channels/<channel>/index.mdb`。
3. **只读挂载**: 以只读无锁模式（`MDB_RDONLY | MDB_NOLOCK`）打开 `heed::Env` 并移交 `sage-solver` 进行零拷贝点查。

---

## 2. 异步并行分块下载器 (`DownloadEngine`)

- **Range 并发**: 对于大体积包文件，探测服务端 `Accept-Ranges: bytes` 头，使用 Tokio 任务并发拉取分块并合并。
- **流式哈希**: 边接收数据边通过 SHA-256 流式计算摘要，下载完成后即刻比对，失败则自动尝试备用镜像。
- **进度通知**: 通过异步通道向 CLI 前端报告实时下载速率与百分比。
