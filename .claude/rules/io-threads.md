---
paths:
  - "src/storage.{c,h}"
  - "src/btrfs-util.{c,h}"
  - "src/oans.c"
---

# io-threads default (storage heuristic)

- `--io-threads` sizes three pools (walkers, csum/read, dedupe). The default is
  refined **only when `--io-threads` was not passed**
  (`options.io_threads == 0`), on the main thread once roots are known
  (`apply_storage_defaults()` in `oans.c`).
- `storage_detect()` (`src/storage.{c,h}`): rotational-ness and device count
  (btrfs via `BTRFS_IOC_FS_INFO`/`DEV_INFO` → `/sys/.../queue/rotational`).
  `storage_recommend_io_threads()`, pure and unit-tested: SSD/unknown
  `min(nproc,8)` (measured, `scan-walk.md`); single HDD ≤4; HDD pool
  ~2/device, cap 8.
- **HDD/pool constants are unmeasured guesses.** Given rotational storage,
  measure with `scripts/bench.py --walk-threads 4,8,16,32`; don't guess again.
- **Never reintroduce `--autotune` or any self-measuring mode that writes to
  the hashfile** (#153): it measured warm cache without root and its persisted
  count beat the heuristic forever. Use `scripts/bench.py`.
- `pscan_set_storage_rotational()` (scan-ETA weight) must be set from the same
  `storage_detect()` **even when `--io-threads` is explicit**; keep that
  ordering in `apply_storage_defaults()`.
