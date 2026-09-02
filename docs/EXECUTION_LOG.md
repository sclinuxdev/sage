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
| `cargo check -p sage --no-default-features` | passed without Torture Lab APIs |
| `cargo clippy --all-targets -- -D warnings` | passed with zero warnings |
| `cargo test --all-targets` | passed: 66 existing tests, 16 Torture Lab tests, 1 benchmark ignored |
| `cargo run -p sage-tests --bin sage-torture -- quick` | passed; 17 recorded real package operations |
| production Rust physical-line count | 9,861 lines; informational, not a release gate |
| fixed-seed 24-step state-machine test | passed with seed `0x5a6e2026` |
| expanded 200-step state-machine | passed with seed `1517166630`; 329 recorded steps |
| nightly-scale 1,000-step state-machine | passed with seed `1517166630`; 1,667 recorded steps |
| focused provider-backtracking and exact proxy-binding tests | passed |
| focused channel-path traversal test | passed, including dot-only channel, subchannel and alias values |
| focused local Git-daemon readiness test | passed without timing sleeps |
| focused multi-process and abrupt-termination tests | passed |
| Linux Torture Lab binary | 16 passed, 1 benchmark ignored in Debian sid as uid 1000 |
| focused filesystem matrix | passed: file/directory swaps, read-only target, long path, hard link and equal-content conflict |
| focused concurrency matrix | passed: same-package updates, cross-channel writers and reader/writer serialization |
| focused LMDB map-full | passed with a 1 MiB map and zero partial indexes |
| repeated recovery failure | passed for install and partial removal journals |
| ignored benchmark smoke | passed when invoked explicitly |
| ownership handoff regression | passed for one-way package split; cyclic swap remained atomic |
| nonzero operation-lock timeout | passed with bounded retry sleeps |

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
10,000-package run completed in 48.11 seconds with a 92,291,072-byte maximum
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

## Linux validation

The official `rust:1.96-bookworm` image pull and a Debian `apt-get update` both
stalled and were cancelled. A second path succeeded:

```bash
brew install zig
rustup target add aarch64-unknown-linux-gnu
cargo zigbuild --target aarch64-unknown-linux-gnu --all-targets
```

The first Homebrew attempt failed after a partial LLVM bottle transfer; the
retry resumed and installed Zig 0.16.0, LLVM 21 and LLD 21. The cross-build
completed and produced ARM64 GNU/Linux ELF binaries. Those exact binaries were
executed in the existing Debian sid image as uid 1000:

- Torture Lab: 16 passed, zero failed, one benchmark ignored. The ignored
  benchmark smoke passed when invoked explicitly.
- Main integration binary: 66 passed, zero failed, one filtered out. The only
  filtered test was the local git-daemon fixture because the minimal Debian
  runtime image does not contain git; that same fixture passed on macOS.

An attempt to create the remote SCLinux migration issue was rejected by the
external-write approval gate. The blocker remains recorded locally and no
workaround write was attempted.
