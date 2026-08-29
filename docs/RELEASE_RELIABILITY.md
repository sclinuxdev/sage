# Release Reliability Report

## Verified findings and fixes

1. **Darwin archive modes did not compile.** `files.idx` stores `u32`, while
   Darwin `mode_t` is `u16`. Archive modes now convert through the host type;
   the full macOS suite compiles and runs.
2. **Provider preferences were hard roots.** Rebuild could not backtrack from a
   conflicting preferred provider and could persist the wrong binding. Provider
   bindings now come from the actual PubGrub solution.
3. **Rejected conflicts left permanent recovery journals.** The journal was
   created before archive/identity/ownership checks. Complete transaction
   preflight now runs before journal creation, including conflicts among two
   packages in the same transaction.
4. **Late hostile archive entries could follow valid files.** Every payload
   header is now scanned before the first write; hard links, duplicate aliases,
   unsafe links, unsupported types and index disagreement fail before mutation.
5. **Trigger checkpoint recovery replayed completed groups.** A durable
   `complete` stage now precedes journal retirement. The counting-trigger test
   proves a crash after that checkpoint does not execute it twice.
6. **Second-resolution operation IDs repeated.** IDs now include nanoseconds and
   an in-process sequence, keeping concurrent/retried diagnostics distinct.
7. **No production state consistency command existed.** `sage verify` now checks
   package records, reverse ownership and managed-path existence. `sage count`
   restores the SCLinux boot-gate compatibility command.
8. **The local Git fixture guessed readiness with sleeps.** It now waits on the
   daemon readiness stream and continuously drains diagnostics.
9. **Channel configuration paths could contain parent traversal.** Repository
   channel identifiers, aliases, signing keys and target roots are now rejected
   unless they are normalized values that remain within their configured roots.

Each item has a deterministic regression in `crates/sage-tests`.

## SCLinux integration status

The current SCLinux Stage1 recipe pins C++ Sage commit
`77b0e29a22cd6e3883aaf7e1823980e140432f0d`; Stage2 builds the local C++/xmake
tree. The live upstream-pin gate now reports that `77b0e29` is no longer an
ancestor of Sage's default branch. No C++ 0.2.x/0.3.x tag is an ancestor of the
current Rust default branch. Neither bootstrap stage contains Rust or Cargo
recipes, so repointing those recipes to Sage 0.4 would break the bootstrap
closure rather than prove integration.

The safe integration delivered here is the compatibility behavior SCLinux's
QEMU gate consumes (`sage count` and `sage verify`) plus a documented migration
gate. A full pin migration requires an explicit Rust bootstrap/toolchain recipe,
new Cargo build commands, updated 0.4 configuration/package schemas, and a fresh
rootfs/QEMU boot. Package-repository evidence alone is not that proof.

## Remaining explicit risks

- The CLI does not expose an explicit downgrade selector. Repository rollback
  is fail-stable and does not silently downgrade.
- External trigger commands are at-least-once in the narrow interval between a
  successful process exit and its LMDB checkpoint. Shipped trigger commands must
  be idempotent.
- `sage verify` detects missing files and ownership disagreement; it does not
  reconstruct missing payloads automatically or hash every non-configuration
  file because schema-v1 installed records do not retain every archive hash.
- LMDB uses a fixed 8 GiB virtual map. The 10,000-package benchmark exercises
  normal growth, not a real map-full recovery at multi-gigabyte scale.
- Real ENOSPC requires a bounded filesystem/loop device and remains a privileged
  platform job; ordinary CI covers equivalent short-write and pre-commit abort
  cleanup through deterministic injection.
- The macOS suite excludes ELF/RUNPATH assertions and does not claim that the
  Linux package-build sandbox runs on Darwin.
- SCLinux's network pin gate currently fails on the orphaned `77b0e29` Sage
  bootstrap pin. Resolving it requires the Rust bootstrap migration or an
  upstream branch-retention policy for the legacy C++ source.

These are release decisions or schema/platform projects, not hidden passing
tests. None is silently treated as successful behavior.
