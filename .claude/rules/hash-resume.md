---
paths:
  - "src/file_scan.c"
  - "src/csum.{c,h}"
  - "tests/integration/test_hash_resume.py"
  - "tests/integration/test_interrupt_resume.py"
---

# Hash resume for very large files (#159)

- `csum_whole_file()` checkpoints every `CHECKPOINT_INTERVAL_BYTES` (1 GiB);
  the next run resumes there.
- **The file digest must not change meaning.** A hash of extent digests (#158)
  was rejected: it makes fragmentation part of file identity. The running
  xxhash state is snapshotted instead (`running_checksum_save/restore`;
  `csum.c` is the only file that knows xxhash).
  - The blob is `XXH3_state_t` copied verbatim (as `XXH3_copyState`), valid
    within one build only: it carries magic + `XXH_versionNumber()` +
    `sizeof(state)`, and **restore refuses anything it cannot vouch for** (an
    upgrade means a rehash, never a wrong digest). Re-point `extSecret` at this
    process's static secret on restore.
  - **Keep the memset in `start_running_checksum()`**: a reset leaves padding
    and the buffer tail undefined, and they get written to the hashfile.
- **A checkpoint carries the in-flight extent digest plus `ext_loff`/`ext_len`**
  (btrfs reports a contiguous file as one fiemap extent however large, so
  boundary-only checkpoints never fired). A resume that does not find that
  extent where it was left discards the checkpoint; this also catches a
  rewritten layout (a defrag need not move mtime or size).
- A checkpoint is trusted on mtime + size: a `chattr +C` file modified in place
  with its timestamp restored resumes into a digest of bytes that never
  existed. The incremental path has the same blind spot and `FIDEDUPERANGE`
  byte-verifies. **Don't "fix" this in the resume path alone**; a stronger
  identity (`STATX_CHANGE_COOKIE`/`i_version`) is repo-wide or nothing. The
  extent check catches it on CoW btrfs only incidentally (not on nodatacow, nor
  at an extent boundary).
- **Stamp the checkpoint with the mtime read at listing time**, not checkpoint
  time, or a file modified during run 1's hashing looks consistent to run 2.
- A row (mtime, size) is written before hashing; only the digest says it was
  hashed. `SELECT_FILE_CHANGES` selects `digest is not null` and the
  up-to-date shortcut requires it. `dbfile_prune_unscanned_files()` deletes
  digest-less rows at startup but spares rows with a checkpoint.
- **A resumed file keeps its row but gets this run's `dedupe_seq`**
  (`dbfile_update_dedupe_seq`). Never re-run the upsert: `INSERT OR REPLACE`
  cascades away checkpoint and hashes. The old generation would leave it at or
  below the watermark, never deduped. Pinned by
  `test_a_resumed_file_is_deduped_by_the_run_that_finishes_it`.
- A region's hashes and its checkpoint land in **one transaction,
  force-committed** (`scan_write_flush`, not the 10 s batch cadence). Extent
  rows are written only by a checkpoint, so none exist past one; block rows
  flush on their own cadence (#161), hence `dbfile_remove_hashes_from()` on
  resume.
- `seed_checkpointed_files()` (from `filescan_walk_run()`) hands checkpointed
  files to the pool before the walk (resume in 0.02 s instead of 0.73 s on
  120k files). Each constraint has a test:
  - **Only under this run's roots and only if `check_file()` accepts it** (one
    hashfile may cover several trees). `scan_root_paths` exists for this;
    prefix match guards `/data` vs `/database`.
  - **Only if the walk would reach it (#281)**: `walk_would_reach()` walks down
    from the root (`-r` below it, each directory stat'ed without following
    symlinks and put through `check_file()`). A bare prefix match seeded files
    under `--exclude`d directories, without `-r`, or behind a symlink.
  - **`adopt_resume()` refuses a checkpoint without extent state past the start
    of a data extent (#281)**: an `only_whole_files` checkpoint resumed by a
    default run stored a partial extent digest.
  - **Mark it seen (`mark_inode_seen` + `mark_file_seen`)** or two workers hash
    it. Pinned via `run_history.files_scanned` (2 instead of 1).
  - **Seed after the walker handles are open, before the consume loop, and
    force-flush**: the csum pool already runs, so a later
    `dbfile_open_handle()` (`CREATE TABLE IF NOT EXISTS`) cannot get the write
    lock and aborts on `!wdb`.
- **Kills more often than `COMMIT_INTERVAL_SEC` (10 s) loop forever at zero
  progress**: nothing is durable before the batched writer commits (a
  checkpoint force-commits the shared batch). SIGINT/SIGTERM flush the open
  batch (#201); only `SIGKILL` discards it.
- Hooks: `DUPEREMOVE_CHECKPOINT_BYTES` lowers the interval;
  `DUPEREMOVE_CHECKPOINT_STOP=N` abandons a file after N checkpoints
  (deterministic, no signal race). `tests/integration/test_hash_resume.py`
  asserts digests, extent and block hashes after dozens of interruptions are
  **identical to one straight-through scan**; nothing downstream can tell a
  wrong digest from a file without duplicates.
