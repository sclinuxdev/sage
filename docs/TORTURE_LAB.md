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

# Database, ownership, package-path and random-throughput measurements
cargo run --release -p sage-tests --bin sage-torture -- bench --packages 10000

# Quick + random + benchmark
cargo run --release -p sage-tests --bin sage-torture -- all \
  --seed 1517166630 --operations 1000 --packages 10000
```

The ordinary CI suite runs the quick integration tests once through
`cargo test --all-targets`. `.github/workflows/torture.yml` runs the long random
sequence and 10,000-package benchmark nightly or on manual dispatch.

## Failure reproduction

Every random run prints its seed before the first operation. The generated state
machine includes install, remove, reinstall, upgrade, repository rollback
attempts, dependency replacement, failed install/retry, conflicts, and runtime /
toolchain channel switching. A failed run automatically replays shorter prefixes
and reports the smallest failing prefix plus its operation list. Re-run that seed
and prefix length, then preserve the sequence as a deterministic regression.

The reference model tracks installed `(channel, name, slot, version)` instances
and expected physical payload contents. After every mutation the Lab verifies:

- command result versus the requested transition;
- package records and pending journals;
- forward and reverse ownership indexes;
- file existence and exact contents;
- absence of unowned debris in managed fixture roots; and
- idempotence of repeated install/remove operations.

## Fault injection

Normal production builds do not expose fault enums or honor crash markers. The
`sage-tests` crate explicitly enables the `torture` feature on `sage`, which
forwards it to `sage-archive` and `sage-db`:

| Layer | Hook | Boundary |
| --- | --- | --- |
| Archive | `BeforeTemporary` | before the exclusive temporary file |
| Archive | `AfterPartialWrite` | after a bounded partial payload write |
| Archive | `BeforeRename` | after checksum/fsync, before publication |
| Archive | `AfterRename` | after the final name becomes visible |
| LMDB | `BeforeWrite` | after opening the write transaction |
| LMDB | `BeforeCommit` | after all indexes, before commit |
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

The plain form returns an error. The `abort:` form terminates a worker process,
allowing the next process to prove kernel-lock release and journal recovery.
The marker is one-shot and removed before the injected failure.

To add a new point, place it immediately after a durable boundary, add it to the
table above, and add a regression that checks both the interrupted state and a
fresh-process retry. Do not add a point whose expected recovery state is not
specified in `INVARIANTS.md`.

## Attack and boundary coverage

The quick suite covers parent/absolute/empty/non-UTF-8 paths, duplicate
separators, 300-byte components, intermediate symlink redirection, escaping
symlink targets, hard links, file/directory swaps, read-only destinations,
duplicate payload aliases, equal/different-content ownership conflicts, partial
writes, rename boundaries, LMDB pre-commit rollback and a synthetic map-full.
Linux CI additionally exercises real ELF and private RUNPATH behavior.
Multi-package upgrade fixtures cover one-way ownership handoff and atomic
rejection of cyclic swaps.

Concurrency tests use real child processes and an explicit parent-held lock as a
barrier. They cover two installs, two updates of the same package, install/remove,
different runtime/toolchain channels, a shared read-only verifier versus a
writer, abrupt owner death, fail-fast probes, and bounded `--lock-timeout`.

## Platform limits

- The release runtime and build sandbox target Linux. `bwrap`, ELF scanning,
  RUNPATH rewriting, and `/proc` peak-RSS collection are Linux-only.
- Archive, LMDB, solver, model, and CLI tests also compile and run on macOS.
  Darwin uses a narrower `mode_t`; archive modes are converted explicitly.
- Case-folding collisions depend on the backing filesystem. CI uses a
  case-sensitive Linux filesystem; macOS case behavior is reported separately.
- Peak RSS is emitted when `/proc/self/status` is present. On macOS use
  `/usr/bin/time -l` around the benchmark command.
