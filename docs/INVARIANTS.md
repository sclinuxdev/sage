# Sage Package-State Invariants

These invariants are the release contract exercised by the Torture Lab. They
describe observable state, not implementation details.

## Package lifecycle

1. Recipe and archive metadata are parsed into one `Package` identity:
   `(repository channel, package name, slot, epoch-version-release)`.
2. A build creates a deterministic `tar.zst` containing a leading metadata
   section and a payload whose regular files and symbolic links exactly match
   `files.idx`.
3. Install and upgrade solve the complete retained installed world plus the new
   roots. A package conflict is rejected before a recovery journal is created.
4. Upgrade publishes the replacement, commits its LMDB record, and then removes
   obsolete paths whose ownership set is empty. The journal retains the prior
   package record until obsolete cleanup completes.
5. Remove rejects dependency breakage, snapshots the removed records, updates
   LMDB, removes only paths with no surviving owner, reconciles alternatives,
   and executes post-remove triggers.
6. Repeating an install converges without changing state. Repeating an absent
   remove may return an error, but it must not mutate LMDB or the rootfs.
7. A repository rollback does not silently downgrade an installed package.
   Explicit downgrade selection is not part of the 0.4 CLI contract.

## LMDB and rootfs

- `packages`, `files`, and `provides` are updated in one LMDB write transaction.
- A regular file or symbolic link has exactly one package owner. Batch conflicts
  abort the complete LMDB transaction.
- Every file listed by an installed package exists below the target root, and
  the reverse ownership index contains that exact package identity.
- Every ownership entry names an installed package whose file list contains the
  same path.
- `sage verify` reports missing managed paths, unsafe persisted paths, orphaned
  ownership records, and package/reverse-index disagreement.
- A failed preflight does not leave an operation journal. Once publication
  starts, exactly one integrity-sealed journal remains until forward recovery
  reaches `complete`.

## Channels and solving

- System payloads are relative to `/`; runtime and toolchain payloads are
  prefixed by their configured `target_root` before ownership checks.
- Equal relative paths in different physical channel roots do not conflict.
- Channel names, aliases, signing-key paths, and target roots are validated
  before they participate in cache or rootfs path construction.
- A channel operation cannot claim or overwrite a path owned by another package
  at the same physical location.
- Package selection and dependency ordering use ordered maps/sets. Equal inputs
  yield the same solution, install order, archive bytes, and trigger order.
- Configured virtual providers are preferences. PubGrub may backtrack to another
  provider, and the persisted binding records the provider actually selected.

## Recovery and external effects

- Recovery is forward-only and idempotent at package, alternatives, and journal
  checkpoint boundaries.
- Abrupt process termination releases the kernel advisory lock. The next writer
  validates and resumes every pending journal before planning new work.
- A second failure while replaying installation or partial removal retains the
  same integrity-sealed journal and remains retryable.
- Trigger completion is checkpointed before journal retirement. A crash after
  that checkpoint does not replay the trigger group.
- Arbitrary external trigger commands have at-least-once semantics in the narrow
  interval between process success and checkpoint persistence; trigger authors
  must make commands idempotent.

## Paths and archives

- Empty, absolute, parent-relative, non-UTF-8 metadata paths, duplicate canonical
  payload paths, hard links, device nodes, and unsupported top-level entries are
  rejected.
- Every payload header is validated before the first write, so a malicious late
  archive entry cannot leave earlier files published.
- Intermediate directories are opened relative to the target root with
  `O_NOFOLLOW`; a pre-existing symbolic link cannot redirect extraction.
- Symbolic link targets must be relative and must not walk above the package
  root. Hard links are unsupported and rejected.
- Regular files are written to exclusive temporary files, checked, synced, and
  renamed within one directory. A failed write leaves no temporary debris.

## Concurrency

- Writers use one target-root operation lock. Multiple writers are serialized;
  shared readers may coexist, and a zero-duration bounded wait fails immediately.
- Lock directories and the final file are opened through an anchored
  `openat(O_NOFOLLOW)` walk. `--lock-timeout` provides a deterministic bounded
  wait without changing the historical blocking default.
- LMDB remains single-writer/multi-reader. A package operation never relies on
  timing sleeps for correctness.
- LMDB map-full aborts the complete write transaction; package, owner and
  provider indexes remain unchanged.
- The release tests use process boundaries and lock barriers to cover concurrent
  installs, install/remove races, lock release, and read/write contention.
