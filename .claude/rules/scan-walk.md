---
paths:
  - "src/file_scan.{c,h}"
  - "src/threads.{c,h}"
---

# Scan parallelism (the walk)

The walk (`opendir`/`readdir`/`statx`) runs on N walker threads
(`--io-threads`); every regular file goes to a **single consumer** (the main
thread) running `__scan_file()`. Deliberate: all delicate per-file state (batched
writer, `dedupe_seq`, `seen_inodes`, batched read txn, `subvol_cache`) stays on
the one consumer, lock-free. Walkers only read `locked_fs` (set before they
spawn) and mutex-guarded `verified_devs`. **Don't move per-file DB state onto the
walkers.**

- **`DUPEREMOVE_SCAN_STATS=1`** prints one scan diagnostic to stderr at end of
  scan: csum-queue `pops`/`empty-waits` (worker starvation) and write-lock
  `acquisitions`/`contended`/`lock-wait` (total thread-seconds + avg µs per
  contended wait). Use it to tell producer-starvation from write-lock
  contention. In-memory (no `--hashfile`) shows ~3 `dbfile_lock()`/file — the
  per-file change-detection *read* serializes too (shared-cache in-memory has no
  WAL); `--hashfile` shows ~2 with lock-free WAL-snapshot reads.
- **Walker count plateaus at ~8 on btrfs — don't raise the cap.** Cold 149k-file
  `~/git`: 1→2→4 scaled (15→9.9→7.8s), 8 was the knee (7.3s), 16/32 gave no wall
  gain while `sys` exploded (16→23→46s). It's btrfs metadata b-tree lock
  contention (`btrfs_search_slot`/`_raw_spin_lock`), not I/O.
  - **Decoupling walkers from the hashing pool doesn't help either** (re-checked
    2026-07 with `DUPEREMOVE_WALK_THREADS`, which overrides *only* `walk_nthreads`,
    leaving the csum/dedupe pools at `--io-threads`). Cold `bench.py` walker sweep
    with io-threads pinned at 8: on `~/git` (media-mix) walk={4,8,16,32} gave
    medians 14.35/14.48/14.52/14.58 s, and on `many` (250k tiny files, the most
    walk-bound case) 8.33/8.01/7.88/8.01 s — flat, `sys` creeping up with more
    walkers. Verified real, not a no-op: the same harness's io-threads control
    swept 1→2→8 = 38.0/20.7/7.9 s, and thread counts + hash output were checked
    (`DUPEREMOVE_WALK_THREADS=32` → 32 live `walker` threads; each run stored
    250k extents). So **don't decouple or double the walkers**: they only feed the
    single `__scan_file` consumer, so past ~4 they're never the bottleneck — the
    csum pool (`--io-threads`) is. `DUPEREMOVE_WALK_THREADS` exists solely as a
    `bench.py --walk-threads` experiment hook; default (unset) is unchanged.
- **Walkers decide from their own `statx`, not from `d_type` (#278).** An
  entry replaced between `readdir()` and the `statx()` - a file by a
  directory, or by a symlink to one - was pushed as a file, and the consumer's
  `abort_on(!S_ISREG)` then killed the run with the open batch. The `statx`
  uses `AT_SYMLINK_NOFOLLOW`, the queue is chosen from `stx_mode`, the consumer
  skips and counts a non-regular file, and the hashing open adds `O_NOFOLLOW`
  for a swap after all that.
  - **A listed file is opened with `open_listed_file()`**, never a bare
    `open()`: a FIFO swapped in after the walk blocked the open forever, and
    on the consumer that stopped the walk. It opens `O_NONBLOCK|O_NOFOLLOW`
    and refuses anything `fstat` does not call regular.
- Cold-walk cost is fundamental btrfs metadata I/O (`statx→btrfs_iget→btree`);
  SQLite is <2%, so parallelizing the consumer wouldn't help.
