# Sage Torture Lab
Each run drives `sage::execute` against an isolated rootfs, LMDB, cache, and channels.
```bash
cargo run -p sage-tests --bin sage-torture -- quick
cargo run -p sage-tests --bin sage-torture -- random \
  --seed 1517166630 --operations 1000
```
The gates cover model/state agreement, recovery, hostile archives, ownership, map-full, and process locking; fault markers exist only under `torture`.

Scope is package installation, upgrade, removal, and their recovery paths. Production
changes in this PR support those gates: payload/ownership preflight, configuration
handoffs, journal checkpoints, and lock isolation. Provider selection, declarative
rebuild policy, and init-service switching keep the existing main-branch behavior;
they are not acceptance criteria for this lab. The earlier hardening work remains
available in commit `3ce73b6` for a separate review.

## Focused correctness gates

`cargo test --workspace --all-targets` also runs provider lifecycle transactions,
service query failures with missing native definitions, signed HTTP index replay
fixtures, orphan reachability, and binding-only transaction previews. These paths
are separate from the randomized package-state model above.

The cgroup spawn unit test uses a subprocess deadline and verifies that failed
attachment never executes the payload. Linux baseline CI additionally enables the
memory and pids controllers and runs
`cgroup::tests::delegated_scope_confines_exec_and_descendants` under a 30-second
deadline. That test verifies kernel limits and inherited membership in the target
scope; it is ignored in ordinary test runs that cannot create writable cgroups.
