# Sage Torture Lab

Each run creates an isolated rootfs, LMDB, signed package cache, and channels,
then drives public `sage::execute` without touching host paths.

```bash
cargo run -p sage-tests --bin sage-torture -- quick
cargo run -p sage-tests --bin sage-torture -- random \
  --seed 1517166630 --operations 1000
```

`cargo test --all-targets` includes quick; the dedicated workflow runs the fixed
long seed. Random mutations check journal/package state, ownership, payloads,
unowned debris, retries, rollback, conflicts, and channel switching.

Normal production builds do not expose fault enums or honor crash markers. The
`sage-tests` enables the `torture` feature on `sage`. Test-only journal points
cover extraction, LMDB publication, removal, alternatives, and triggers.

```text
<root>/run/sage/crash-point = before-lmdb-write
<root>/run/sage/crash-point = abort:before-lmdb-write
```

The marker is one-shot; `abort:` proves process-death recovery. Attack tests
cover unsafe paths/links, malformed archives, ownership handoff, map-full, and
anchored locks. Real child processes cover writer and reader/writer ordering.
The build sandbox targets Linux; core tests also run on macOS.
