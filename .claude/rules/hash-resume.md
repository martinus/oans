---
paths:
  - "src/file_scan.c"
  - "src/csum.{c,h}"
  - "tests/integration/test_hash_resume.py"
  - "tests/integration/test_interrupt_resume.py"
---

# Hash resume for very large files (#159)

A 1 TiB file interrupted six hours into hashing used to restart at byte zero, so
on a big enough file a scheduled scan could never finish. Now `csum_whole_file()`
writes a **checkpoint** every `CHECKPOINT_INTERVAL_BYTES` (1 GiB) and the next
run picks the file up there.

- **The digest is unchanged — that was the whole design constraint.** The
  original plan (#158) was to redefine the file digest as a hash of extent
  digests, which is additive and so trivially resumable; it was rejected because
  it makes fragmentation part of file identity, so byte-identical files with
  different extent layouts stop matching. Instead the *running xxhash state* is
  snapshotted (`running_checksum_save/restore` in `csum.c`, the only file that
  knows xxhash). Nothing about what a digest means changed, and no existing
  hashfile is invalidated.
  - xxhash publishes no serialization, so this copies `XXH3_state_t` verbatim.
    Legitimate within one build (`XXH3_copyState` is the same copy) but not
    across, so the blob carries magic + `XXH_versionNumber()` + `sizeof(state)`
    and **restore refuses anything it cannot vouch for** — a distro xxhash
    upgrade just means a rehash, never a wrong digest. On restore, `extSecret`
    must be re-pointed at this process's static secret; everything else is data.
  - **`start_running_checksum()` memsets the state before resetting it.** A
    reset only initialises the fields it uses, leaving alignment padding and the
    unused tail of the internal buffer undefined — harmless until the struct is
    copied out, at which point those bytes get written to the hashfile. Valgrind
    caught exactly this. Don't drop the memset; it costs nothing measurable
    (`fragmented`, 262k extents: 2.53 → 2.54 s median).
- **The in-flight extent digest rides along in the checkpoint.** The first cut
  only checkpointed at extent boundaries, so no extent state was needed — and it
  never fired, because **btrfs reports a contiguously allocated file as a single
  fiemap extent however large it is**. That is precisely the shape this feature
  exists for. So a checkpoint carries the partial extent digest plus
  `ext_loff`/`ext_len`, and a resume that does not find that extent where it was
  left discards the checkpoint and starts over — which doubles as the one cheap
  check that the layout was not rewritten underneath (mtime and size need not
  move on a defrag).
- **A checkpoint is trusted on mtime + size, and that is not certain.** Measured:
  a `chattr +C` (nodatacow) file modified in place with the timestamp restored
  afterwards resumes across the change and yields a digest of a byte string that
  never existed. The point is that **the ordinary incremental path has the same
  blind spot** — measured on the same file, a full scan then the same
  modification leaves a stored digest that no longer describes it — so resume
  inherits oans's guarantee rather than weakening it, and the real safety net is
  that `FIDEDUPERANGE` byte-verifies (two files oans believed identical deduped
  to `0 B` with both intact). Don't "fix" this in the resume path alone; a
  stronger identity (statx `STATX_CHANGE_COOKIE`/`i_version`) would be a
  repo-wide change to change detection or nothing.
  - On CoW btrfs the extent check above usually catches that case anyway, since
    an in-place write splits the extent. Incidental, not a guarantee: it is gone
    on nodatacow and whenever a checkpoint lands on an extent boundary (no
    extent state, so nothing to check).
  - **The checkpoint stamps the mtime read when the file was *listed*, not the
    one current at checkpoint time.** Stamping the current one would make a file
    modified *during* run 1's hashing look consistent to run 2, which would then
    resume across the modification. Listing-time makes it fail closed.
- **Change detection had a matching hole.** A file's row is written with its
  mtime and size *before* hashing starts; only the digest says it was ever
  hashed. `SELECT_FILE_CHANGES` therefore also selects `digest is not null`, and
  the up-to-date shortcut requires it. Until now nothing noticed, because
  `dbfile_prune_unscanned_files()` deleted every digest-less row at startup —
  that prune now spares rows with a checkpoint, which is what keeps them alive.
- **A resumed file keeps its row but gets this run's `dedupe_seq`**
  (`dbfile_update_dedupe_seq`, a targeted UPDATE). Re-running the usual upsert
  would `INSERT OR REPLACE` the row and cascade away the checkpoint and stored
  hashes; leaving the generation alone lets a dedupe phase in between draw level
  with it, and a file at or below the watermark is one dedupe never looks at.
  Pinned by `test_a_resumed_file_is_deduped_by_the_run_that_finishes_it`.
- Hashes for the region just covered and the checkpoint claiming it land in **one
  transaction, force-committed** (`scan_write_flush`, not the batched writer's
  10 s cadence — a checkpoint still inside an open transaction protects nothing).
  Extent rows are only ever written by a checkpoint, so none exist past one;
  block rows flush on their own batch cadence (#161) and can, hence
  `dbfile_remove_hashes_from()` on resume.
- **Checkpointed files are handed to the pool before the walk starts**
  (`seed_checkpointed_files()`, from `filescan_walk_run()`). The walk would find
  them eventually, but on a large tree "eventually" is minutes — measured on a
  120k-file tree with the file six directories down, time-to-resume went
  **0.73 s → 0.02 s**, and the seeded figure doesn't grow with the tree. Three
  constraints, each with a test:
  - **Only under this run's roots, and only if `check_file()` accepts it.** One
    hashfile may cover several trees scanned on different days; seeding outside
    the named roots would hash terabytes nobody asked for. `scan_root_paths`
    exists solely for this (prefix match, guarding against `/data` vs
    `/database`).
  - **And only if the walk would reach it (#281).** A prefix match is not
    that: `--exclude` works by the walk never entering a directory, so a
    checkpointed file under an excluded one was seeded and hashed anyway, as
    was one in a subdirectory on a run without `-r`, or behind a directory
    since replaced by a symlink. `walk_would_reach()` walks the path down from
    its root - `-r` below the root, and each directory stat'ed without
    following symlinks and put through `check_file()`.
  - **A checkpoint without extent state inside a data extent is refused
    (#281).** `only_whole_files` keeps no extent digest, so a default run
    resuming its checkpoint hashed the extent from the checkpoint on and
    stored that. `adopt_resume()` declines when the offset is past the start
    of a data extent and no extent state came with it.
  - **Mark it seen (`mark_inode_seen` + `mark_file_seen`) or the walk queues it
    twice** and two workers hash one file at once. Pinned by asserting
    `run_history.files_scanned`, which is 2 instead of 1 when this is dropped.
  - **Seed *after* the walker handles are open, before the consume loop.** The
    csum pool is already running, so a seeded file produces a write transaction
    immediately, and every `dbfile_open_handle()` runs `CREATE TABLE IF NOT
    EXISTS` — which then can't get the write lock and aborts on `!wdb`. Seeding
    also force-flushes for the same reason. (Ordinary per-file writes never hit
    this: by then the walkers are long connected.)
- **Restarting converges, but only above the commit interval.** Measured on a
  30 GiB / 6006-file tree with repeated `SIGKILL`s: everything ends up hashed,
  no checkpoints left. The floor is `COMMIT_INTERVAL_SEC` (10 s) — nothing is
  durable until the batched writer commits, so **killing more often than that
  loops forever at zero progress**. Pre-existing, and this change strictly
  improves it: at 1.4 s kills v1.9.1 persisted 0 files over six rounds, where
  this code reached 1875 before stalling, because a checkpoint force-commits the
  whole shared batch. **SIGINT/SIGTERM now flush the open batch** (#201), which
  raises that floor from `COMMIT_INTERVAL_SEC` to "whatever finished hashing";
  only `SIGKILL` still discards it.
- Test hooks: `DUPEREMOVE_CHECKPOINT_BYTES` lowers the interval,
  `DUPEREMOVE_CHECKPOINT_STOP=N` abandons a file after N checkpoints — an
  interrupted run, deterministically, rather than racing a signal against a read.
  `tests/integration/test_hash_resume.py` drives a tree through dozens of
  interruptions and asserts the digests, extent hashes and block hashes are
  **identical to one straight-through scan**; that is the property that matters,
  since nothing downstream could tell a wrong digest from a file with no
  duplicate.
