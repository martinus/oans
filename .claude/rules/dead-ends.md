---
paths:
  - "src/file_scan.c"
  - "src/dbfile.c"
  - "scripts/bench*.py"
  - "scripts/perf-profile.sh"
---

# Measured dead-ends

Re-attempt one only with new evidence against the measurement given.

- **Walk then hash in two phases** (`DUPEREMOVE_SEPARATE_PHASES`: whole walk,
  then global largest-first hashing): reverted. `mixed`/`bigfile` (incl.
  io-threads=2) identical, disk-bandwidth-bound (6 GB cold: 2.7 s wall, 0.5 s
  user); cold `~/git` ~3% slower (14.5 vs 14.1 s), losing the overlap of
  hashing with the metadata walk. An exact ETA needs no serialization: the
  walk already counts files.
- **`STATX_CHANGE_COOKIE` is not userspace ABI (#207)**, only in-kernel
  (`include/linux/stat.h`, NFSD's `i_version`). On 7.1.3 `struct statx` has
  no `stx_change_cookie`, `kernel-headers` lacks the macro, the mask never
  returns the bit (btrfs, tmpfs; root irrelevant).
  - `ctime` closes the hole and is already read (`STATX_BASIC_STATS`): a
    `chattr +C` rewrite with mtime restored by `touch -d` keeps a stale
    digest, but `utimensat()` moves ctime.
  - `FIDEDUPERANGE` (both ends) and `btrfs filesystem defragment` keep ctime,
    so deduped files are not rehashed; `chmod`/`chown`/xattr/relabel and
    `rsync -a` restores cost one rehash. Measured on btrfs only:
    **re-measure the dedupe row on xfs before implementing** (sketch on #207).
- **Warm rescan is single-consumer latency-bound**: ~174k files take ~2 s
  wall / ~0.9 s CPU at any `--io-threads` (1..16); `__scan_file` handoff +
  per-file `dbfile_describe_file` (~0.12 s CPU) is the floor.
- **Preloading `files` to skip per-file `describe_file`**: 2.0 → 3.1 s; the
  query is not the bottleneck, the preload costs ~1 s.
- **statx relative to a dir fd**: ~5-8% less CPU, zero wall; PR closed.
- **Single-writer refactor for `io_mutex` contention: don't.**
  `DUPEREMOVE_SCAN_STATS`, warm 173k files: contended% 2.3/4.8/9.9/16.4 at
  `--io-threads` 4/8/16/32, wall flat past ~16 (12.21 → 12.22 s), total
  lock-wait ≤3.7 thread-seconds (<1%), each wait tens of µs. Users' ~20%+ is a
  ratio, not time; `--hashfile` waits ~2× longer, still absorbed.
- **Batching csum dispatch/commits buys nothing**: `empty-waits` ~0 (8 = the
  startup ramp); the single `__scan_file` producer keeps the queue full. Idle
  csum rows in the live UI are cosmetic (one slot per worker for life).
- **No savings preview / dry-run**: reclaimed disk depends on compression,
  alignment, kernel refusals and snapshot/external refcounts; dedupe is safe
  and self-reporting; `--stats` gives the logical bound; a physical estimate
  needs root (`BTRFS_IOC_LOGICAL_INO`).
- **Biggest-savings-first exists**: `push_extents` qsorts by `cmp_dext_work`
  (`de_len*(de_num_dupes-1)`, desc) per generation pass; going global is not
  worth it.
