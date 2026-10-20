---
paths:
  - "src/file_scan.c"
  - "src/dbfile.c"
  - "scripts/bench*.py"
  - "scripts/perf-profile.sh"
---

# Measured dead-ends — don't re-attempt without new evidence

- **Separating the walk from hashing into two sequential phases is not worth it.**
  Prototyped (`DUPEREMOVE_SEPARATE_PHASES`: finish the whole parallel walk, then hash
  strictly largest-first with the full file list known — true global LPT + exact
  up-front byte total) and cold-benchmarked vs the current pipeline. Byte-identical
  hash output. `mixed`/`bigfile` (incl. io-threads=2, the idle-tail case):
  **identical** — those are disk-bandwidth-bound (6 GB cold = 2.7 s wall but only
  0.5 s user CPU), so read *order* can't change makespan. Cold `~/git`: pipelined
  was ~3% *faster* on the median (14.1 vs 14.5 s) because hashing overlaps the cold
  metadata walk — separation throws that overlap away for nothing. The one real win
  (exact ETA from a known total) is a UX gain that doesn't need serialization (the
  walk already counts files incrementally). Reverted.
- **`STATX_CHANGE_COOKIE` is not a userspace ABI — don't plan around it (#207).**
  It exists in `include/linux/stat.h` for in-kernel callers (NFSD's `i_version`),
  not in the UAPI: on 7.1.3 `struct statx` has no `stx_change_cookie`,
  `kernel-headers` does not define `STATX_CHANGE_COOKIE`, and asking for the bit
  anyway returns a mask without it (btrfs and tmpfs alike, spare words zero). So
  it is not privilege-gated — running as root would not help, and the filesystem
  is irrelevant.
  - **`ctime` closes the same hole and is already being read.** Re-measured: a
    `chattr +C` file rewritten in place with `touch -d` restoring the mtime keeps
    a stale digest (mtime and size both unchanged), but its ctime *does* move —
    `utimensat()` updates ctime as a side effect, so restoring a timestamp cannot
    hide a write. `ctime` is in `STATX_BASIC_STATS`, which the walk already asks
    for: no new syscall, no kernel floor.
  - **The objection that would have sunk it does not hold**: `FIDEDUPERANGE`
    leaves ctime alone on both source and destination (measured), as does
    `btrfs filesystem defragment` — so an incremental run would *not* re-hash
    everything it deduped last night. What does move ctime without changing
    content is metadata churn (`chmod`/`chown`/xattr/relabel) and a
    `rsync -a`-style restore: one extra re-hash of that subtree, cost only.
  - Only measured on btrfs; **re-measure the dedupe row on xfs before
    implementing**, since that is the load-bearing one. Design sketch and the
    full table are on #207.

- **Warm rescan is single-consumer pipeline-latency-bound**, not CPU/query. A
  no-op rescan of ~174k files is ~2s wall / ~0.9s CPU, invariant to
  `--io-threads` (1..16); the serial consumer (`__scan_file` queue handoff +
  per-file `dbfile_describe_file`, only ~0.12s CPU) sets the floor.
- **Bulk in-memory change-detection cache** (preload `files` to skip the per-file
  `describe_file`) was prototyped and **measured a regression** (2.0→3.1s): the
  query isn't the bottleneck and the preload adds ~1s.
- **statx-relative-to-dir-fd** gave ~5-8% less scan *CPU* but **zero wall**
  change; PR closed.
- **Scan write-lock (`io_mutex`) contention is a red herring — don't do the
  single-writer refactor.** `DUPEREMOVE_SCAN_STATS` on warm 173k-file `~/git`:
  contended% rises with `--io-threads` (2.3→4.8→9.9→16.4% at 4/8/16/32) but wall
  is flat past ~16 (12.21→12.22s) and total lock-wait stays ≤3.7 *thread*-seconds
  (<1% of runtime) — the lock is off the critical path (piling threads onto it
  just makes them wait, unchanged completion). The ~16% (and users' ~20%+ on
  bigger trees) is scary as a *ratio* but negligible as *time*; each wait is tens
  of µs, spread across the pool. On-disk `--hashfile` waits are ~2× longer per
  collision than in-memory (WAL disk I/O in the critical section), still absorbed.
- **"Idle" csum threads in the live UI are not starvation.** `empty-waits` is ~0
  (8 = the 8-worker startup ramp) across whole scans: the single `__scan_file`
  producer keeps the queue full. So batching worker dispatch/commits buys
  nothing. (The *display* idle-flicker is a separate, cosmetic thing — csum
  workers are persistent, so they hold one progress slot for life rather than
  re-claiming per file.)
- **A savings preview / dry-run isn't worth building.** Real reclaimed disk can't
  be predicted without doing the dedupe (compression, extent alignment, the
  kernel declining, snapshot/external refcounts), and dedupe is already safe and
  self-reporting. `--stats` gives the logical upper bound. (A compsize-style
  *physical* estimate needs root — `BTRFS_IOC_LOGICAL_INO` — so it can't back an
  unprivileged preview either.)
- **"Biggest-savings-first" order already exists:** `push_extents` qsorts by
  `cmp_dext_work = de_len*(de_num_dupes-1)` (reclaimable bytes, desc). Only per
  generation-pass, not global — not worth changing.
