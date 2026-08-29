# Release Reliability Execution Log

Commands below were executed against the working branches during this hardening
run. Inspection-only `rg`, `sed`, `git log`, and `git status` commands are
omitted; every build, test, lint, benchmark, and integration gate is recorded.

## Sage

| Command | Result |
| --- | --- |
| `xmake f -m debug -y` on the former C++ branch | configured; warned that libc++ module metadata was missing |
| `xmake -v` on the former C++ branch | failed on macOS because Linux `elf.h` and external LMDB/Zstd headers were unavailable |
| `cargo fmt --all -- --check` | passed after final formatting |
| `cargo check --all-targets` | passed |
| `cargo clippy --all-targets -- -D warnings` | passed with zero warnings |
| `cargo test --all-targets` | passed: 65 existing integration tests and 11 Torture Lab tests |
| `cargo run -p sage-tests --bin sage-torture -- quick` | passed; 17 recorded real package operations |
| production Rust physical-line count | 9,568 lines; recorded as a release risk |
| fixed-seed 24-step state-machine test | passed with seed `0x5a6e2026` |
| focused provider-backtracking test | passed |
| focused channel-path traversal test | passed |
| focused local Git-daemon readiness test | passed without timing sleeps |
| focused multi-process and abrupt-termination tests | passed |
| full Torture Lab test target | passed: archive faults, DB faults, attacks, locks, concurrency, recovery, verification, model and benchmark smoke |

The first macOS Rust baseline exposed the `u32`/Darwin `mode_t` compile error.
After that fix, an in-sandbox run still produced expected `EPERM` failures for
LMDB locking and local network fixtures; the same command was rerun outside the
filesystem sandbox and passed apart from Linux-only ELF assertions, which are
now correctly scoped to Linux.

## Benchmarks

All three commands completed successfully in release mode:

```bash
cargo run --release -p sage-tests --bin sage-torture -- bench --packages 100
cargo run --release -p sage-tests --bin sage-torture -- bench --packages 1000
/usr/bin/time -l cargo run --release -p sage-tests --bin sage-torture -- bench --packages 10000
```

The complete results and measurement boundary are in `BENCHMARKS.md`. The
10,000-package run completed in 39.28 seconds with a 93,061,120-byte maximum
resident set size.

## SCLinux integration

| Command | Result |
| --- | --- |
| `sh tests/test-shc.sh` | 21 passed |
| `shellcheck -s sh scripts/shc tests/test-shc.sh` | passed |
| `python3 tests/test-build.py` | 156 passed |
| `python3 tests/test-validate-recipes.py` | 39 passed |
| `python3 tests/validate-recipes.py` | 315 recipes passed; zero checksum debt |
| `python3 tests/test-check-links.py` | 13 passed |
| `python3 tests/check-links.py` | 11 Markdown files, zero problems |
| `python3 tests/test-check-pins.py` | 15 passed |
| `python3 tests/check-pins.py --require-network` | failed: Stage1 Sage pin `77b0e29` is not on the upstream default branch |

The final failure is a verified integration blocker, not a flaky test. All C++
Sage 0.2.x/0.3.x tags are outside the current Rust default-branch ancestry, and
SCLinux Stage1 has no Rust/Cargo bootstrap closure.

## Linux validation boundary

An official `rust:1.96-bookworm` image pull was attempted for a fresh Linux
rerun. The registry transfer remained near 2 MiB across the large layers and
was cancelled. Therefore the current checkout has complete macOS evidence and
Linux CI definitions, while a fresh local Linux execution of this exact branch
remains pending. Linux-only ELF/RUNPATH tests are still present in normal CI.
