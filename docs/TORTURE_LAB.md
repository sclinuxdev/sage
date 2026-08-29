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

The ordinary CI suite runs the quick integration tests and the explicit quick
binary gate. `.github/workflows/torture.yml` runs the long random sequence and
10,000-package benchmark nightly or on manual dispatch.

## Failure reproduction

Every random run prints its seed before the first operation. A failed audit adds
the operation number and the recorded action sequence to the error. Re-run the
same command with the printed seed and operation count. Reduce a failure by
halving `--operations` until the failing prefix is minimal, then add that prefix
as a deterministic regression.

The reference model tracks installed `(channel, name, slot, version)` instances
and expected physical payload contents. After every mutation the Lab verifies:

- command result versus the requested transition;
- package records and pending journals;
- forward and reverse ownership indexes;
- file existence and exact contents;
- absence of unowned debris in managed fixture roots; and
- idempotence of repeated install/remove operations.

## Fault injection

Production calls never select library fault enums. Tests opt in explicitly:

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

The quick suite covers parent/absolute/empty/non-UTF-8 paths, intermediate
symlink redirection, escaping symlink targets, unsupported hard links and entry
types, duplicate payload aliases, file conflicts, partial writes, rename
boundaries, and LMDB rollback. Linux CI additionally exercises real ELF and
private RUNPATH behavior. Read-only directories and generic I/O failures use
the same error/cleanup path as injected write faults.

## Platform limits

- The release runtime and build sandbox target Linux. `bwrap`, ELF scanning,
  RUNPATH rewriting, and `/proc` peak-RSS collection are Linux-only.
- Archive, LMDB, solver, model, and CLI tests also compile and run on macOS.
  Darwin uses a narrower `mode_t`; archive modes are converted explicitly.
- Case-folding collisions depend on the backing filesystem. CI uses a
  case-sensitive Linux filesystem; macOS case behavior is reported separately.
- Peak RSS is emitted when `/proc/self/status` is present. On macOS use
  `/usr/bin/time -l` around the benchmark command.
