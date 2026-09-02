# 模块实现: 极速 CLI 命令行前端 (`sage`)

- **Crate 路径**: `crates/sage`
- **选用生态**: `clap` (derive), `indicatif`
- **代码预算**: ~500 行
- **职责**: 面向用户的极速命令行参数解析、更新与升级控制、版本化子通道操作、终端输出与进度条渲染。

---

## 1. 架构目标与启动性能

- **冷启动开销**: 严格控制在 **< 5ms**，避免加载任何庞大终端渲染框架。
- **无 TUI 依赖**: 专注于纯粹、高效的 CLI 交互。

The binary target only parses `clap` arguments and enters the library. Command
orchestration is kept behind small deep interfaces: package state operations,
source builds, mass rebuilds, and bootstrap each own their implementation
boundary, so the parser does not know about databases, archives, or sandboxes.

---

## 2. CLI 命令设计 (`clap`)

```rust
#[derive(Parser)]
#[command(
    name = "sage",
    version = "0.4.0",
    about = "Ultra-fast declarative package manager"
)]
pub struct Cli {
    #[arg(short, long, global = true)]
    pub verbose: bool,

    #[arg(long, global = true)]
    pub dry_run: bool,

    #[arg(long, global = true, default_value = "/")]
    pub root: PathBuf,

    #[command(subcommand)]
    pub command: Commands,
}

#[derive(Subcommand)]
pub enum Commands {
    /// 求解并安装指定包 (支持 --channel 指定版本化子通道如 python3.12, gcc15)
    Install {
        #[arg(required = true)]
        packages: Vec<String>,
        #[arg(long)]
        channel: Option<String>,
        #[arg(long)]
        no_save: bool,
    },
    /// 卸载指定包
    Remove {
        #[arg(required = true)]
        packages: Vec<String>,
        #[arg(long)]
        channel: Option<String>,
    },
    /// 计算升级差集并执行安全事务升级 (默认同 Slot 内升级，校验反向依赖)
    Upgrade {
        /// 可选指定要升级的特定包列表 (为空则升级全系统或指定通道的所有已安装包)
        packages: Vec<String>,
        #[arg(long)]
        channel: Option<String>,
        /// 升级前首先执行源索引刷新
        #[arg(long)]
        sync: bool,
    },
    /// 刷新已启用软件源的远端 LMDB 索引 (支持 ETag 0 流量条件同步)
    Sync {
        #[arg(long)]
        channel: Option<String>,
    },
    /// 根据 /etc/sage/system.toml 调和系统全局状态
    Rebuild,
    /// 软件源与子通道管理
    Channel {
        #[command(subcommand)]
        action: ChannelAction,
    },
    /// 工具链与活动 Profile 切换
    Toolchain {
        #[command(subcommand)]
        action: ToolchainAction,
    },
    /// 极速构建包归档 (*.pkg.tar.zst)
    Build {
        recipe_dir: PathBuf,
    },
    /// LMDB-backed state queries (installed, owner, info)
    Query {
        #[command(subcommand)]
        query: QueryAction,
    },
}
```

### CLI 常见用法示例
```bash
# 1. 刷新源索引并全量升级系统已安装包
sage sync
sage upgrade
# 或一键完成:
sage upgrade --sync

# 2. 单包安全升级 (自动校验反向依赖与 ABI)
sage upgrade ripgrep

# 3. 仅升级指定 Python 3.12 子通道内的所有包
sage upgrade --channel python3.12

# 4. 切换全局活动 GCC 为 gcc15
sage toolchain use gcc15
```

## 5. Source fleet commands

`mass-rebuild` performs automatic recipe discovery and parallel dependency
layers. `bootstrap` executes explicitly staged self-hosting plans. Both support
dry-run topology output, bounded package concurrency, and a custom artifact pool.
