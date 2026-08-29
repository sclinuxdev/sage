# Torture Lab Benchmark Snapshot

Measured on 2026-08-30 with Rust 1.96.0, release mode, Darwin 25.5 arm64.
Each row uses a fresh temporary LMDB and temporary package root. Values are one
sample for regression orientation, not a cross-machine performance promise.

| Metric | 100 packages | 1,000 packages | 10,000 packages |
| --- | ---: | ---: | ---: |
| Empty LMDB startup (ms) | 4.728 | 5.729 | 10.649 |
| Individual insert transactions (ms) | 333.712 | 3,528.507 | 33,471.385 |
| One batch transaction (ms) | 3.996 | 3.956 | 12.023 |
| Hot package lookups total (ms) | 0.102 | 0.534 | 5.545 |
| Hot lookup average (ns) | 1,017 | 534 | 554 |
| Cold reopen + one lookup (ms) | 0.140 | 0.176 | 0.265 |
| Ownership lookups total (ms) | 0.035 | 0.321 | 3.677 |
| Conflict rejection (ms) | 0.011 | 0.011 | 0.017 |
| LMDB `data.mdb` logical bytes | 294,912 | 606,208 | 3,227,648 |
| One-file package install (ms) | 29.465 | 31.810 | 32.381 |
| 1,000-file package install (ms) | 4,071.157 | 4,041.059 | 4,023.220 |
| Model/audit operations per second | 59.4 | 62.1 | 55.7 |

The 10,000-package command completed in 39.28 seconds and `/usr/bin/time -l`
reported a 93,061,120-byte maximum resident set size (87,802,336-byte peak
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
