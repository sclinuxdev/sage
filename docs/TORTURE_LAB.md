# Sage Torture Lab

Each run creates an isolated rootfs, LMDB, signed package cache, and channel
roots, then drives public `sage::execute` without touching host paths.

## One-command entry points

```bash
# Stable release gate: lifecycle, dependencies, conflicts, channels and recovery
cargo run -p sage-tests --bin sage-torture -- quick

# Reproducible model-driven state machine
cargo run -p sage-tests --bin sage-torture -- random \
  --seed 1517166630 --operations 1000
```

The ordinary CI suite runs the quick integration tests once through
`cargo test --all-targets`. `.github/workflows/torture.yml` runs the long random
sequence nightly or on manual dispatch.

Random runs print their seed and cover install, remove, upgrade, rollback,
dependency replacement, retry, conflicts, and channel switching. After every
mutation the model checks package/journal state, ownership, exact payloads,
unowned debris, and idempotence. Preserve any failing sequence as a regression.

Normal production builds do not expose fault enums or honor crash markers. The
`sage-tests` enables the `torture` feature on `sage`. Test-only journal points
cover extraction, LMDB publication, removal, alternatives, and triggers.

CLI injection is enabled only by a marker inside the temporary target root:

```text
<root>/run/sage/crash-point = before-lmdb-write
<root>/run/sage/crash-point = abort:before-lmdb-write
```

The one-shot plain form returns an error; `abort:` terminates a worker so the
next process can prove lock release and journal recovery.

The quick suite covers unsafe paths and links, file/directory swaps, read-only
destinations, ownership conflicts, malformed archives, map-full rollback, and
one-way versus cyclic ownership handoff. Linux CI also exercises ELF/RUNPATH.

Concurrency tests use real child processes and an explicit parent-held lock as a
barrier. They cover two writers, a shared reader versus a writer, abrupt owner
death, anchored lock paths, and private lock-directory repair.

The build sandbox targets Linux; archive, LMDB, solver, model, and CLI tests also run on macOS.
