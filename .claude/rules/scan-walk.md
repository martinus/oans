---
paths:
  - "src/file_scan.{c,h}"
  - "src/threads.{c,h}"
---

# Scan parallelism (the walk)

- Walker threads (`opendir`/`readdir`/`statx`, `--io-threads` of them) feed
  one consumer, the main thread in `__scan_file()`. **Never move per-file DB
  state onto walkers**: batched writer, `dedupe_seq`, `seen_inodes`, read txn,
  `subvol_cache` are lock-free because only the consumer touches them. Walkers
  read only `locked_fs` (set before spawn) and mutex-guarded `verified_devs`.
- `DUPEREMOVE_SCAN_STATS=1`: end-of-scan stderr line with csum-queue
  `pops`/`empty-waits` and write-lock `acquisitions`/`contended`/`lock-wait`;
  tells producer starvation from lock contention. Normal: ~3 `dbfile_lock()`
  per file in-memory, ~2 with `--hashfile`.
- **Don't raise the walker cap** (~8 on btrfs; 16/32 add no wall, `sys`
  16 → 46 s: b-tree lock contention in `btrfs_search_slot`/`_raw_spin_lock`).
  **Don't decouple or double walkers**: walker count 4..32 at fixed
  `--io-threads` measured flat; the csum pool is the bottleneck.
  `DUPEREMOVE_WALK_THREADS` (overrides only `walk_nthreads`) is solely a
  `bench.py --walk-threads` hook.
- **Walkers classify from their own `statx` (`AT_SYMLINK_NOFOLLOW`,
  `stx_mode`), not `d_type` (#278)**: a swapped entry queued as a file hit the
  consumer's `abort_on(!S_ISREG)`. The consumer skips and counts non-regular
  files; the hashing open adds `O_NOFOLLOW`.
- **Open a listed file with `open_listed_file()`, never bare `open()`**
  (`O_NONBLOCK|O_NOFOLLOW`, refuses non-regular per `fstat`): a swapped-in FIFO
  otherwise blocks the consumer forever.
- Cold-walk cost is btrfs metadata I/O (`statx→btrfs_iget→btree`), SQLite <2%:
  parallelizing the consumer won't help.
