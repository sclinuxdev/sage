# Sage Architecture Specification
**Version:** 2.0  
**Status:** Approved  
**Language:** Modern C++23 (100% C++23 Modules)  
**Build System:** xmake  

---

## 1. System Architecture Overview

Sage is structured as a layered, modular package manager designed around five core subsystem layers:

```mermaid
graph TB
    subgraph StorageLayer["1. 存储与状态层 (Storage Layer)"]
        LMDB["<b>LMDB Zero-Copy Database</b><br/>/var/lib/sage/data.mdb<br/>(Packages, Files, Provides, Channels, System Providers)"]
    end

    subgraph ChannelLayer["2. Channel 运行时层 (Channel Runtime Layer)"]
        SysChannel["<b>System Channel (`/`)</b><br/>Root FHS Filesystem"]
        RuntimeChannel["<b>Runtime Channel (`/usr/lib/runtimes/`)</b><br/>Shared SDKs, LLVM, CUDA"]
        ToolchainChannel["<b>Toolchain Channel (`/opt/channels/`)</b><br/>Isolated Language Environments"]
        UserChannel["<b>User Channel (`~/.local/`)</b><br/>Non-root User Tools"]
        ProfileEngine["<b>Profile Engine</b><br/>Symlinks & /etc/profile.d/sage-channels.sh"]
    end

    subgraph ServiceLayer["3. 通用服务转换层 (Universal Service Layer)"]
        ServiceDef["<b>service.toml Specification</b><br/>(ExecStart, User, After, Restart)"]
        Generators["<b>Service Generators</b><br/>-> Loom / OpenRC / Runit / Systemd / Dinit / s6"]
    end

    subgraph SolverLayer["4. 依赖求解与重构层 (Solver & Reconcile Layer)"]
        PubGrub["<b>Native C++23 PubGrub / CDCL SAT Solver</b><br/>(Version Ranges, Virtual Providers, SONAMEs)"]
        RebuildEngine["<b>Reconcile Engine (`sage rebuild`)</b><br/>(Diff system.toml vs LMDB -> Guarded State Transition)"]
    end

    subgraph ArchiveLayer["5. 归档与解包层 (Streaming Archive Layer)"]
        Archive["<b>Native Streaming Tar + Zstd</b><br/>(64KB Ring Buffer, Zero Legacy Tar Overhead)"]
        ELFScanner["<b>Automated ELF Scanner</b><br/>(Extracts DT_NEEDED & DT_SONAME)"]
    end

    StorageLayer --> SolverLayer
    ChannelLayer --> ProfileEngine
    ServiceLayer --> RebuildEngine
    SolverLayer --> RebuildEngine
    ArchiveLayer --> StorageLayer
    RebuildEngine --> StorageLayer
```

---

## 2. LMDB Database Schema

The state database `/var/lib/sage/data.mdb` uses dedicated named databases (tables):

| Table Name (DBI) | Key | Value | Purpose |
| :--- | :--- | :--- | :--- |
| `packages` | `pkg_name` | Serialized Package Metadata | Complete metadata, version, release, channel, license |
| `files` | `rel_path` (e.g. `usr/bin/rg`) | `pkg_name:channel_name` (newline-separated set; shared directories carry one claim per installing package) | Instant conflict detection & file ownership lookup |
| `provides` | `symbol` (e.g. `virtual/init`, `so:libzstd.so.1`) | `pkg_name` | Fast symbol & virtual provider reverse lookup |
| `channels` | `channel_name` | Channel Scope, Target Root, Triplet, Priority | Channel registry |
| `system` | `interface` (e.g. `virtual/init`) | Active provider (`openrc`) | Declarative system state lock |

### Package-state operation synchronization

Root-only `install`, `remove`, and `rebuild` use
`/run/sage/operation.lock` as one host-wide advisory lock. Sage creates a
root-owned `0700` namespace and `0600` regular file, opens them without following
symlinks, and validates ownership, type, and mode. The namespace is directly
beneath root-owned `/run`, so an unprivileged user cannot pre-create it; Sage
does not create `/run` or use the public `/run/lock` hierarchy. Preview
operations take `LOCK_SH`; real
mutations take `LOCK_EX`. The operation lock is held by an RAII descriptor in
the CLI entry point until the command and all of its LMDB environments have
returned. This intentionally serializes operations for distinct target roots
in exchange for a stable lock identity even when a target root does not exist.

After locking, the target root and `data.mdb` are probed once. Existing-state
previews open LMDB with `MDB_RDONLY | MDB_NOLOCK`; the external shared lock then
excludes every Sage LMDB writer for the environment's complete lifetime.
Absent-state previews do not open LMDB, and channel indexes are fetched and
parsed without persisting cache files. Creating the root-only ephemeral host
lock under `/run` is outside the target-root zero-write guarantee.

---

## 3. Streaming Tar + Zstd Archive Format (`*.pkg.tar.zst`)

```
pkgname-1.0.0-1-x86_64.pkg.tar.zst
├── .METADATA/
│   ├── manifest.toml     # Package name, version, release, license, provides, dependencies
│   ├── files.idx         # Relative path, size, mode, SHA256
│   ├── triggers.toml     # Initramfs, bootloader, ldconfig hooks
│   └── service.toml      # Universal daemon specification (optional)
└── data/                 # Direct filesystem payload (usr/bin/..., etc/...)
```

Extraction is fail-closed: package paths are normalized and preflighted before
the payload is written, and every parent directory is then opened relative to a
trusted target-root file descriptor with symlink following disabled. Temporary
regular files and their final rename stay relative to that verified directory,
which prevents archive-controlled symlink traversal and pathname replacement.
This is not a filesystem transaction against a privileged process concurrently
moving an already-open target directory.

Install replacement, remove plans, and provider rebuilds revalidate the state
they planned from inside the LMDB write transaction. Existing files may migrate
only while the `pkg_name:channel_name` owner recorded by that transaction is
still present in the path's ownership set; a concurrent package or provider
change aborts the stale operation. Shared directories release one claim per
removal and are deleted only when the last owner lets go. LMDB state
updates are transactional, but filesystem extraction/removal is not journaled
for rollback if a later step fails.

---

## 4. Multi-Init Service Generation Mapping

| Target Init | Generated Output Path | Generation Format |
| :--- | :--- | :--- |
| **Loom** | `/usr/lib/loom/services/<name>.toml` | Native strict TOML generated by Loom's offline compiler |

Loom generation is fail-closed: one invalid package service aborts generation,
and Sage invokes `loom validate --root` on the complete generated graph before
running post-transaction triggers.
| **OpenRC** | `/etc/init.d/<name>` | `#!/sbin/openrc-run` shell script |
| **Runit** | `/etc/sv/<name>/run` & `finish` | `#!/bin/sh` runscript with `chpst` |
| **Systemd** | `/usr/lib/systemd/system/<name>.service` | INI unit file (`[Unit]`, `[Service]`, `[Install]`) |
| **Dinit** | `/etc/dinit.d/<name>` | Dinit service definition (`type = process`, `command = ...`) |
| **s6** | `/etc/s6/services/<name>/run` | execlineb script with `s6-setuidgid` |
