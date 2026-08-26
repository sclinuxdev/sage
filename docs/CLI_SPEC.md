# Sage CLI Command Specification

The `sage` command-line interface provides clean, high-performance commands for package management, system reconciliation, channel routing, and packaging.

---

## 1. Global Options

```
sage [OPTIONS] <COMMAND> [ARGS...]

Global Options:
  --verbose, -v        Enable detailed verbose diagnostic output
  --dry-run            Preview install/remove/rebuild without persistent writes
  --wait[=SECONDS]     Wait for another package-state operation
  --root <DIR>         Operate on an alternate target root
  --target <TRIPLET>   build: select the cross-compilation target triplet
  --arch <ARCH>        build: override the package architecture
  --quiet, -q          Suppress informational output
  --help, -h           Show help message
  --version, -V        Display Sage version
```

`install`, `remove`, and `rebuild` require root and serialize package-state
access on one host lock: `/run/sage/operation.lock`. Sage creates the
namespace as `root:root` mode `0700` and the file as `root:root` mode `0600`,
then validates ownership, type, and mode before locking. Dry-runs take a shared
advisory lock and real mutations take an exclusive lock; operations for
different target roots therefore also serialize. The root-owned `/run` parent
prevents unprivileged namespace pre-creation; Sage neither creates `/run` nor
uses the public `/run/lock` hierarchy.

A dry-run probes the target root and database exactly once after locking. An
absent database is modeled as empty for install, is a no-op for remove, and is
an explicit uninitialized-state error for rebuild. An existing database is
opened with `MDB_RDONLY | MDB_NOLOCK` while the shared host lock remains held.
Dry-run channel indexes are parsed in memory without updating the target-root
cache, so previews do not change package files, LMDB files/tables, legacy PID
locks, or cache state. The root-only ephemeral host coordination state under
`/run` is the sole allowed dry-run write.

---

## 2. Command Reference

### `sage install <PKG...>`
Resolves dependencies via PubGrub SAT solver, unpacks `*.pkg.tar.zst` streams to target channel scope, writes LMDB state records, and executes triggers.
Archive writes are anchored to the target root without following parent symlinks. If the installed identity changes concurrently after dependency resolution, the command exits without applying the stale package migration. Fresh non-conflicting batches inspect and extract in parallel; `/etc/sage/build.toml` `jobs` controls package-operation concurrency and `0` uses the online CPU count. The separate `compile_jobs` setting controls parallelism inside one package build. Upgrades and path-conflict cases preserve ordered installation.
Conflicts are evaluated against the ownership map the whole transaction will produce: a file moving from one package to another inside the same install batch (e.g. a monolithic `foo` splitting into `foo` + `foo-libs`) is a handover, not a conflict, while any path claimed by two packages that both keep it is still rejected before extraction.
```bash
# Install packages into system channel (root)
sage install ripgrep neovim

# Install into specific channel (e.g. isolated toolchain)
sage install --channel python312 python

# Dry run / preview transaction without modifying filesystem
sage install --dry-run waybar
```

### `sage remove <PKG...>`
Removes installed package files, unregisters LMDB records, and removes generated service scripts.
The complete installed-package snapshot is checked again under the LMDB writer lock before any files are removed; a concurrent package change aborts the stale removal plan.
```bash
sage remove nginx
```

### `sage rebuild`
**Declarative System Reconcile**: Compares `/etc/sage/system.toml` against active LMDB state. Performs guarded swaps of exclusive virtual providers (`virtual/init`, `virtual/udev`) and automatically re-generates all native daemon service scripts. Provider locks and packages scheduled for removal are revalidated inside the LMDB write transaction before the plan is applied.
```bash
# Preview what rebuild would change
sage rebuild --dry-run

# Execute system reconciliation
sage rebuild
```

### `sage channel [list|add|remove|sync]`
Manages multi-layer Channel sources, scopes, and priorities.
`install` and `rebuild` resolve remote channels from the last atomically cached
index and therefore do not add one network round trip per operation. Run
`sage channel sync` when fresh repository metadata is required; this command
always fetches and replaces the cache, while a missing or corrupt cache is
fetched automatically during resolution.
```bash
# List active channels and their scopes
sage channel list

# Add a remote channel repository
sage channel add core https://channels.example.invalid/core --scope system --priority 100
sage channel add rust-nightly https://channels.example.invalid/rust --scope toolchain --priority 50

# Sync channel metadata indexes
sage channel sync
```

### `sage build <RECIPE_DIR>`
Builds a `.pkg.tar.zst` binary package from a `recipe.toml` definition, automatically extracting ELF `DT_NEEDED` and `DT_SONAME` symbols.
All v1 recipe phases and v2 Sage-managed steps run through the exact
`fakeroot` executable configured in `/etc/sage/build.toml`. Sage aborts before
executing a recipe when it cannot probe that executable; it never silently
runs the recipe outside fakeroot. This virtualizes file metadata only and does
not provide a security sandbox or elevated privileges.

For recipe-v2, `package.check_dependencies` are resolved against the installed
package metadata of `build.sysroot` before source work starts. They are visible
only inside the read-only build namespace. A declared `[[build.steps]]` with
`phase = "check"` runs after compilation and before installation; any failed
check stops the build and produces no archive. Check dependencies are recorded
in the build attestation, not as runtime install dependencies.
```bash
# Build package archive in current directory
sage build ./recipes/ripgrep
```

### `sage query [installed|info|files|owner]`
Queries package information, files manifest, and file ownership in nanoseconds via LMDB mmap.
```bash
# List all installed packages
sage query installed

# Show details of a package
sage query info ripgrep

# List all files owned by a package
sage query files ripgrep

# Find which package owns a specific file path
sage query owner /usr/bin/rg
```

### `sage service [list|status|generate]`
Inspects daemon definitions and manually re-generates native Loom or legacy init service definitions.
```bash
# List all installed services and their active init mapping
sage service list

# Re-generate service configuration for current active init
sage service generate sshd
```
