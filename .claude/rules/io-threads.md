---
paths:
  - "src/storage.{c,h}"
  - "src/btrfs-util.{c,h}"
  - "src/oans.c"
---

# io-threads default (storage heuristic)

`--io-threads` sizes three pools (walkers, csum/read, dedupe). One mechanism
refines its default, **only when the user didn't pass `--io-threads`** (the
sentinel is `options.io_threads == 0`), resolved on the main thread after the
roots are known (`apply_storage_defaults()` in `oans.c`).

- **`src/storage.{c,h}` — heuristic from device type.** `storage_detect()`
  reports rotational-ness + device count (btrfs pools enumerated via
  `BTRFS_IOC_FS_INFO`/`DEV_INFO` → `/sys/.../queue/rotational`).
  `storage_recommend_io_threads()` is pure and unit-tested: SSD/unknown keep
  `min(nproc,8)` (the validated path); single HDD ≤4; HDD pool ~2/device capped
  at 8.
- **The HDD/pool constants are unmeasured guesses** and there is now no in-tree
  way to validate them — we have no spinning-disk target. The SSD/unknown path
  is the measured one (see the walker plateau in `scan-walk.md`). If you get
  access to real rotational storage, measure with `scripts/bench.py
  --walk-threads 4,8,16,32` and fix the constants; don't guess again.
- **`--autotune` was removed** (#153) — it measured warm-cache throughput
  whenever it couldn't drop caches (i.e. without root), recommended and
  *persisted* a thread count above the measured btrfs plateau, and the stored
  value then won over the heuristic on every later run. Don't reintroduce a
  self-measuring mode that writes to the hashfile. `scripts/bench.py` answers
  the same question properly, as a dev tool.
- The same `storage_detect()` call also feeds the scan-ETA rotational weight
  (`pscan_set_storage_rotational()`), which must be set **even when io-threads
  is fixed** by an explicit flag — hence the ordering in that function.
