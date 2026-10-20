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

- **`cache_size = -65536` (64 MB per connection).** Applied to every connection
  (listing handle, batched writer, one per walker), so it dominates peak RSS on
  a large hashfile (a NAS user hit ~870 MB on 1.7M files). Measured on 174k-file
  `~/git`: 256 MB→64 MB is **perf-neutral** (warm rescan is btrfs-metadata-bound
  and the OS page cache backs the DB; `-rd` group-loading unaffected); 16 MB was
  ~3% slower on the dedupe joins, so 64 MB is the floor. Don't raise it back
  without a measured reason. (Further per-handle-cache / `seen_inodes` ~50 B/file
  memory work is unexplored.)
- **One batched read transaction, not one per file.** In WAL mode a connection
  holds its read snapshot across queries; wrapping the per-file change-detection
  reads in a single transaction (refreshed ~10s) cut `F_SETLK` from ~2/file
  (283k on a 141k rescan) to ~800, ~24% faster. The writer batches on the same
  cadence. Reader and writer must be **separate connections** (`db` listing
  handle vs `wdb`/`scan_writer`).
- **A failed write drops one unit, never the batch (#274).** Every
  `scan_write_begin()` .. `end`/`flush`/`abort` pair is one unit (a file's
  row, a block flush, a checkpoint, the final digest) inside a `SAVEPOINT`,
  and the write lock is held for all of it, so units never interleave. The
  batch used to be rolled back whole: every row id already queued for hashing
  was then gone, and `files.id` is a plain `INTEGER PRIMARY KEY`, so the next
  insert reused one. Injected once on 8,001 files: 7,145 constraint failures,
  0-996 files kept, and in 2 of 25 runs one file's digest on another's row,
  all with exit 0.
  - **When SQLite drops the transaction itself** (a failed `COMMIT`, or
    `SQLITE_FULL`/`SQLITE_IOERR` mid-statement; `sqlite3_get_autocommit()`
    says so), the queued ids are stale and nothing makes them right:
    `batch_lost()` stops the walk, the listing and the csum workers, and the
    run exits 1. The next run rehashes those files.
  - **The unit statements are prepared once per writer (`scan_exec()`).**
    Every file runs two units, and `sqlite3_exec()` parsing `SAVEPOINT` and
    `RELEASE` from text for each, under the write lock, cost the scan 8% wall
    and 18% CPU on a 96k-file tree (`bench.py -p git`, cold: 2.52 → 2.72 s).
    Prepared: 2.58 s. A build without the savepoints ran at 2.50 s, so the
    remaining ~2% is SQLite's savepoint work, which #274 needs. The statements
    are finalized before the writer closes, or the close fails as busy.
  - **Every path after a begin must end the unit.** `scan_write_begin()`
    aborts on a unit left open; `store_file_row()`'s rename failure was one.
  - Pinned by `test_write_failure.py`, through `DUPEREMOVE_WRITE_FAIL_AT=N`
    (fail the Nth file's final write) and `DUPEREMOVE_WRITE_FAIL_LOSES_BATCH`.
    The unit suite never opens the scan writer, so there is no bug block.
- **An open read transaction stops WAL checkpoints (#261).** A checkpoint
  cannot copy a frame that is newer than the snapshot of an open reader. If the
  snapshot saw a fully checkpointed WAL, the checkpoint copies nothing. And the
  WAL file restarts from its start only after a checkpoint has copied all of
  it. Three rules follow for the listing read transaction:
  - **End it when the walk ends.** It used to stay open until all hashing was
    done. One 8 TB image then gave a 513 GB WAL next to a 336 MB hashfile.
    Measured on one 3 GiB file at 4K blocks: before, the database stayed at
    0 MiB and the WAL grew for the whole scan; after, the WAL stayed at 4 MiB.
  - **End it when the consumer waits longer than `COMMIT_INTERVAL_SEC`** for
    the next file (`walk_fileq_pop()`), because a refresh happens only when a
    file arrives.
  - **Commit the write batch between the old and the new snapshot**
    (`scan_read_tick()`). Snapshots taken back to back always lag the last
    commit, so no checkpoint copies everything and the WAL never restarts while
    the walk runs.
  Any new long-lived reader must obey the same rules. Pinned by
  `test_wal_checkpoint.py`, which uses `DUPEREMOVE_CHECKPOINT_PAUSE=N` (SIGSTOP
  at the Nth hash checkpoint) to hold a run in the middle of a file. It covers
  only the first rule. The hook waits for the walk to end before it stops the
  run: without that wait, a loaded CI runner reached the checkpoint before the
  consumer ended its snapshot, and the test failed on the fixed code.
  - **The consumer's timed pop needs its own ThreadSanitizer wrapper.**
    `src/tsan.h` annotates each GLib queue call by name, and
    `g_async_queue_timeout_pop()` was not among them, so every item handed from
    a walker to the consumer read as a race (exit 66 in the sanitizer leg).
    Wrap any new GLib hand-off call the same way.
- **Never build an `IN (...)` or `DISTINCT` set from an unsorted stream of
  rows whose count grows with the tree (#260).** SQLite stores that set in a
  temp B-tree with a tiny page cache, fills it in random key order, and so
  rewrites almost one page per inserted row. `PRAGMA temp.cache_size` does not
  reach it; `temp_store=memory` does, but then RAM grows with the window, and a
  single 8 TB file is one window. Measured on the pass loaders: 20.6 GiB of
  temp writes for a 450 MiB hashfile (blocks) and 8.7 GiB for a 334 MiB one
  (fragmented extents). Group the new rows with `GROUP BY` instead — that goes
  through the sorter, which writes sequential runs (129 MiB and 66 MiB) — and
  probe for older members per group through the digest index.
  - **The pass loaders read the window once (#265).** `GET_DUPLICATE_BLOCKS`
    and `_EXTENTS` group the window in `g0`, and the same step finds each
    group's older member (the anchor, the lowest rowid from an earlier pass)
    with one index probe. The rows then come from the window's files
    (`cross join`, so the files stay the outer loop) plus the anchors by rowid.
    Before, the window was grouped once and then read a second time, digest by
    digest, together with every older copy. Measured with SQLite 3.45 on
    synthetic hashfiles, over all passes: syn blocks 31.7 → 24.6 s,
    fragmented extents 11.3 → 7.4 s, snapshot extents 67.9 → 35.7 s. One new
    generation over a full hashfile, the scheduled-run case: fragmented
    extents 10.3 → 0.14 s, syn blocks 0.54 → 0.25 s, snapshot extents
    0.79 → 0.34 s. The sorter can write up to 2× more temp data, but in
    sequential runs.
  - **The row order is part of the contract** (the loader comments say why).
    The new SQL gave the same member sequence in every group as the old SQL,
    pass by pass, on four synthetic hashfiles and on a real 192k-file tree.
    Pinned by `test_extent_groups_spanning_passes_load_with_their_older_member`,
    whose new member has the lower id, as a resumed file (#159) does.
  - **No `AS MATERIALIZED`**: it would fail to prepare on Debian 11, Ubuntu
    20.04 and RHEL 8 (SQLite < 3.35). Measured: the same plan and times
    without it.
  - **The pre-analysis has the same shape since #270.** It used the scoped
    `IN` form and churned temp pages the same way, and it asked "is there an
    older member" once per extent row. Now `COUNT_*_WORK_SINCE` groups the new
    rows and probes once per group; `_ALL` (at `seq_lo == 0`) has no window and
    no probe. Measured with `scripts/bench-queries.py` (see
    `performance.md`), same results in every case: fragmented extents 19.8 s /
    5.6 GiB temp -> 7.8 s / 0, snapshot extents 29.7 s / 7.2 GiB -> 12.7 s /
    40 MiB, a 2M-file hashfile's whole-file count 5.0 s / 3.1 GiB -> 3.4 s / 0.
    - **`group by +f.digest` in `_SINCE`, and it matters.** Without the `+`,
      SQLite reads the whole digest index in group order and filters the
      window after: on 2M files a one-generation run took 3 s instead of 0.
    - **`count(w), sum(w)` over a `case` that gives NULL**, not a `where`
      on the group: SQLite pushes that `where` into the subquery and then runs
      the probe twice per group.
    - `?1` must appear in every form, since the C code always binds it; `_ALL`
      spells its "no older member" as `?1 > 0`.
    - **One definition of an older extent copy, `EXTENTS_OLDER_COPY`**, used
      by both the loader's anchor probe and the estimate, like
      `FILEDUP_MEMBER`. Two copies of that rule can drift, and then the bar
      stops short or waits at 99% with nothing failing.
- `.hashfile-wal` / `.hashfile-shm` are SQLite WAL sidecars — don't hand-delete.
  SQLite creates them with the database file's mode, so a new hashfile is
  created 0600 *before* `sqlite3_open_v2()` (`precreate_hashfile()`); a chmod
  after the open left them 0644 for the first scan. An empty file the run
  initialises (`touch`ed, or named by a `file:` URI, which `open(2)` cannot
  read) is chmod'ed when `dbfile_identify()` says `HASHFILE_NEW`, still before
  the WAL pragma. A hashfile with data in it keeps its owner's mode: never
  chmod it on open.
- **Hardlink hazard:** `INSERT OR REPLACE` on `UNIQUE(ino, subvol)` can
  cascade-delete rows for other links to the inode; an in-memory `seen_inodes`
  set guards it (a batch aborting here could silently empty the hashfile while
  exiting 0). Keep the regression test.
- **Deleted-file pruning is stat-based.** After each scan
  `dbfile_prune_missing_files()` drops rows whose path `stat()`s ENOENT (hashes
  cascade via FK), then `dbfile_maybe_vacuum` compacts at ≥25% free. It must
  stay stat-based, not "delete rows not walked this run" — else scanning a subdir
  or a shared hashfile would nuke out-of-scope files. Pinned by
  `test_prune_is_stat_based_not_scope_based`.
