# Sage Torture Lab
Each run drives `sage::execute` against an isolated rootfs, LMDB, cache, and channels.
```bash
cargo run -p sage-tests --bin sage-torture -- quick
cargo run -p sage-tests --bin sage-torture -- random \
  --seed 1517166630 --operations 1000
```
The gates cover model/state agreement, recovery, hostile archives, ownership, map-full, and process locking; fault markers exist only under `torture`.
