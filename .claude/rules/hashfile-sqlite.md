---
paths:
  - "src/dbfile.{c,h}"
  - "src/file_scan.c"
  - "src/tsan.h"
  - "scripts/bench-queries.py"
  - "tests/integration/test_wal_checkpoint.py"
  - "tests/integration/test_write_failure.py"
  - "tests/integration/test_hardlinks.py"
---

# Hashfile / SQLite gotchas

## Connections

- **`cache_size = -65536` (64 MB) on every connection** (listing, writer, one
  per walker) dominates peak RSS. 256 -> 64 MB was perf-neutral, 16 MB ~3%
  slower: 64 MB is the floor; don't raise it without a measurement. Per-handle
  cache and `seen_inodes` (~50 B/file) memory work is unexplored.
- **One batched read transaction, refreshed ~10 s, not one per file** (`F_SETLK`
  ~2/file -> ~800 total, ~24% faster); the writer uses the same cadence. Reader
  and writer are **separate connections** (`db` vs `wdb`/`scan_writer`).

## Scan writer units (#274)

- **A failed write drops one unit, never the batch.** Each
  `scan_write_begin()` .. `end`/`flush`/`abort` (row, block flush, checkpoint,
  final digest) is a `SAVEPOINT` under the write lock. A whole-batch rollback
  lost queued row ids, and `files.id` (`INTEGER PRIMARY KEY`) was reused: one
  file's digest on another's row, exit 0.
- **If SQLite drops the transaction itself** (failed `COMMIT`, `SQLITE_FULL`/
  `SQLITE_IOERR`; `sqlite3_get_autocommit()`), `batch_lost()` stops walk,
  listing and csum workers; exit 1, the next run rehashes.
- Unit statements are prepared once per writer (`scan_exec()`;
  `sqlite3_exec()` of `SAVEPOINT`/`RELEASE` text cost 8% wall). Finalize them
  before closing the writer or the close fails as busy.
- **Every path after a begin must end the unit**; `scan_write_begin()` aborts
  on an open one (`store_file_row()`'s rename failure was one).
- Pinned by `test_write_failure.py` (`DUPEREMOVE_WRITE_FAIL_AT=N`,
  `DUPEREMOVE_WRITE_FAIL_LOSES_BATCH`). No bug block: unit suite has no writer.

## Long readers block WAL checkpoints (#261)

The WAL restarts only after a checkpoint copies all of it, and nothing newer
than an open snapshot is copied (an 8 TB image gave a 513 GB WAL). For the
listing read transaction and any new long-lived reader:

- **End it when the walk ends**, not when hashing ends.
- **End it when the consumer waits > `COMMIT_INTERVAL_SEC`** in
  `walk_fileq_pop()`; refreshes happen only when a file arrives.
- **Commit the write batch between old and new snapshot** (`scan_read_tick()`).
- Pinned (first rule) by `test_wal_checkpoint.py` with
  `DUPEREMOVE_CHECKPOINT_PAUSE=N` (SIGSTOP at the Nth hash checkpoint); the
  hook waits for the walk to end first, or CI flakes.
- **Wrap every GLib hand-off call in `src/tsan.h`**; a missing
  `g_async_queue_timeout_pop()` made every hand-off a race (exit 66).

## Query shape (#260, #265, #270)

- **Never build an `IN (...)` or `DISTINCT` set from an unsorted, tree-sized
  row stream (#260)**: the temp B-tree rewrites ~a page per row (20.6 GiB for
  a 450 MiB hashfile); `PRAGMA temp.cache_size` does not reach it and
  `temp_store=memory` grows RAM with the window. Use `GROUP BY` (sequential
  sorter runs) and probe older members per group via the digest index.
- **Pass loaders read the window once (#265).** `GET_DUPLICATE_BLOCKS`/
  `_EXTENTS` group it in `g0` and find each anchor (lowest earlier-pass rowid)
  with one probe; rows come from the window's files (`cross join`, files
  outer) plus anchors by rowid.
- **Row order is part of the contract** (loader comments). Pinned by
  `test_extent_groups_spanning_passes_load_with_their_older_member` (new
  member has the lower id, like a resumed file, #159).
- **No `AS MATERIALIZED`**: fails on SQLite < 3.35 (Debian 11, Ubuntu 20.04,
  RHEL 8); same plan without it.
- **Pre-analysis (#270):** `COUNT_*_WORK_SINCE` groups new rows and probes once
  per group; `_ALL` (`seq_lo == 0`) has no window, no probe. Benchmark with
  `scripts/bench-queries.py` (`performance.md`).
  - **`group by +f.digest` in `_SINCE`**, or SQLite walks the whole digest
    index (3 s instead of 0 on 2M files).
  - **`count(w), sum(w)` over a NULL-giving `case`**, not a `where` on the
    group (pushed into the subquery, probes twice).
  - `?1` appears in every form (always bound); `_ALL` writes `?1 > 0`.
  - **`EXTENTS_OLDER_COPY` is the one definition of an older extent copy**,
    shared by anchor probe and estimate (like `FILEDUP_MEMBER`); copies drift
    and the bar stops short or waits at 99%.

## Files and rows

- Don't hand-delete `.hashfile-wal`/`.hashfile-shm`. They inherit the db mode:
  `precreate_hashfile()` makes a new hashfile 0600 before `sqlite3_open_v2()`;
  an empty one (`touch`ed, `file:` URI) is chmod'ed on `HASHFILE_NEW` from
  `dbfile_identify()`, before the WAL pragma. **Never chmod a hashfile with
  data on open.**
- **Hardlinks:** `INSERT OR REPLACE` on `UNIQUE(ino, subvol)` can
  cascade-delete other links' rows; `seen_inodes` guards it (else the
  hashfile can empty with exit 0). Keep the regression test.
- **Pruning stays stat-based**: `dbfile_prune_missing_files()` drops rows whose
  path `stat()`s ENOENT (FK cascade), then `dbfile_maybe_vacuum` at >=25% free.
  Never "delete rows not walked": subdir scans and shared hashfiles would lose
  out-of-scope files. Pinned by `test_prune_is_stat_based_not_scope_based`.
