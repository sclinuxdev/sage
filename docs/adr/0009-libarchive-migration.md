# 9. Archive Module Rewrite: libarchive Migration

## Status

Proposed (awaiting review)

## Context

The hand-rolled Tar+Zstd engine (~2200 lines across `detail.cppm`, `tape.cppm`,
`extract.cppm`, `pack.cppm`) accumulated several issues:

- TOCTOU false positives from ctime-based preflight checks
- No parallel extraction within a single archive (52k-file rust-bin took ~104s)
- Scalar SHA-256 hashing without hardware acceleration
- Complex custom USTAR parsing that duplicates libarchive functionality
- Tight coupling between durability mode, conffile handling, and path safety

libarchive 3.8.9 is available on both the host and the target chroot,
with zstd/lz4/bzip2/xz filter support built in.

## Decision

Replace the entire `sage::archive` module with a thin wrapper around
libarchive's streaming reader/writer API.

### New module layout

Single file `src/store/archive.cppm` — no partitions.

### Public API (unchanged signatures where possible)

| Function | Notes |
|---|---|
| `inspect_package(path, root?)` | Read manifest + file list |
| `extract_package(archive, root, opts)` | Parallel write via worker pool |
| `create_package(manifest, pkg_dir, out_path)` | Write .METADATA + data as tar.zst |
| `generate_repo_index(dir, name)` | Scan dir for *.pkg.tar.zst |
| `remove_path_anchored(root, path, ignore_nonempty)` | Anchored unlink |
| `conffile_modified(root, path, confs, prev)` | Compare disk hash vs recorded |
| `canonicalize_merge_claim(path)` | Pre-usr-merge path alias |

### Key design changes

1. libarchive handles tar/zstd parsing
2. Parallel file writes via bounded worker queue
3. No temp+rename in batch mode (fresh root writes directly)
4. TOCTOU simplified to dev+ino+size comparison

### Security model

- Path traversal: ARCHIVE_EXTRACT_SECURE_NOABSOLUTEPATHS
- Symlink escape: ARCHIVE_EXTRACT_SECURE_SYMLINKS
- Ownership: anchored-parent openat() relative to extraction root fd
- Integrity: SHA-256 via OpenSSL EVP (SHA-NI accelerated)

## Consequences

- Net reduction ~1600 lines of code
- Runtime dependency on libarchive.so.13 (already in chroot)
