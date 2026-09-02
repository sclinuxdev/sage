# Sage Torture Lab

The Torture Lab creates a fresh temporary rootfs, LMDB state directory, signed
repository index, content-addressed package cache, and channel roots for every
run. It drives the public `sage::execute` path and real archive/database code.
It never targets the host `/`, `/usr`, or `/opt`.

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

## Failure reproduction

Random runs print their seed and cover install, remove, upgrade, rollback,
dependency replacement, retry, conflicts, and channel switching. After every
mutation the model checks package/journal state, ownership, exact payloads,
unowned debris, and idempotence. Preserve any failing sequence as a regression.

## Fault injection

Normal production builds do not expose fault enums or honor crash markers. The
`sage-tests` enables the `torture` feature on `sage`, which exposes bounded LMDB
and journal test seams only to the harness:

| Layer | Hook | Boundary |
| --- | --- | --- |
| CLI journal | `extraction` | after one package extraction |
| CLI journal | `before-lmdb-write` | filesystem complete, DB not published |
| CLI journal | `lmdb-publication` | DB published, obsolete cleanup pending |
| CLI journal | `remove-after-path` | one removal path completed |
| CLI journal | `alternatives` | alternatives/sysusers checkpoint |
| CLI journal | `triggers` | before external triggers |
| CLI journal | `trigger-complete` | triggers checkpointed, journal retained |

CLI injection is enabled only by a marker inside the temporary target root:

```text
<root>/run/sage/crash-point = before-lmdb-write
<root>/run/sage/crash-point = abort:before-lmdb-write
```

The one-shot plain form returns an error; `abort:` terminates a worker so the
next process can prove lock release and journal recovery.

To add a new point, place it immediately after a durable boundary, add it to the
table above, and add a regression that checks both the interrupted state and a
fresh-process retry. Keep the expected recovery state beside that regression.

## Attack and boundary coverage

The quick suite covers unsafe paths and links, file/directory swaps, read-only
destinations, ownership conflicts, malformed archives, map-full rollback, and
one-way versus cyclic ownership handoff. Linux CI also exercises ELF/RUNPATH.

Concurrency tests use real child processes and an explicit parent-held lock as a
barrier. They cover two writers, a shared reader versus a writer, abrupt owner
death, anchored lock paths, and private lock-directory repair.

The runtime/build sandbox targets Linux; archive, LMDB, solver, model, and CLI
tests also run on macOS, with explicit conversion for Darwin's narrower mode.
