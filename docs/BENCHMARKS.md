# Torture Lab Benchmark Snapshot

Measured on 2026-08-30 with Rust 1.96.0, release mode, Darwin 25.5 arm64.
Each row uses a fresh temporary LMDB and temporary package root. Values are one
sample for regression orientation, not a cross-machine performance promise.

| Metric | 100 packages | 1,000 packages | 10,000 packages |
| --- | ---: | ---: | ---: |
| Empty LMDB startup (ms) | 8.079 | 8.308 | 7.167 |
| Individual insert transactions (ms) | 326.462 | 3,926.302 | 38,713.203 |
| One batch transaction (ms) | 3.826 | 5.974 | 21.981 |
| Hot package lookups total (ms) | 0.163 | 1.881 | 12.454 |
| Hot lookup average (ns) | 1,626 | 1,881 | 1,245 |
| Cold reopen + one lookup (ms) | 0.222 | 0.588 | 1.512 |
| Ownership lookups total (ms) | 0.052 | 0.915 | 7.798 |
| Conflict rejection (ms) | 0.015 | 0.026 | 0.031 |
| LMDB `data.mdb` logical bytes | 294,912 | 606,208 | 3,227,648 |
| One-file package install (ms) | 36.530 | 41.373 | 41.607 |
| 1,000-file package install (ms) | 4,107.308 | 4,222.314 | 4,169.260 |
| Model/audit operations per second | 24.2 | 22.1 | 22.6 |

The 10,000-package command completed in 48.11 seconds and `/usr/bin/time -l`
reported a 92,291,072-byte maximum resident set size (84,394,488-byte peak
memory footprint).

## Interpretation boundary

Measured facts:

- LMDB point lookups and ownership queries stayed close to linear total time
  with roughly stable per-lookup cost.
- One ACID batch was much faster than 10,000 separately durable commits.
- The 1,000-file end-to-end install cost was stable across database population
  sizes in this run.
- Conflict rejection remained below 0.02 ms in all three samples.

Likely explanations, not profiler proof:

- Individual insert time is dominated by one durable LMDB commit per package.
- The 1,000-file package path is likely dominated by per-file creation, sync,
  checksum, and rename work rather than LMDB lookup cost.

Not yet verified:

- Linux cold-cache behavior after dropping kernel page cache;
- real device ENOSPC latency and LMDB map-full recovery;
- filesystem differences among ext4, XFS, Btrfs and overlayfs; and
- a sampled CPU/flamegraph attribution for the 1,000-file install.

Reproduce the measured scales with:

```bash
for packages in 100 1000 10000; do
  cargo run --release -p sage-tests --bin sage-torture -- bench \
    --packages "$packages"
done
```
