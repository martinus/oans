# oans — working notes for Claude

`oans` (a fork of duperemove) finds duplicate extents and deduplicates them via
the kernel `FIDEDUPERANGE` ioctl (atomic, byte-verified). Hashes live in a
SQLite **hashfile** (WAL mode, `synchronous=OFF`, `cache_size=-256000`).

All C sources live under `src/` (main program `src/oans.c`); man page sources
under `docs/man/`. The binary is `oans`; `make install` adds a `duperemove`
compat symlink. Some identifiers keep the old name on purpose: the
`DUPEREMOVE*` env vars and the `DuperemoveTest` python test base class.

## Build & test

```sh
make -j$(nproc)                         # builds oans
make check                              # C unit tests (src/tests.c) + Python integration suite
DUPEREMOVE=./oans python3 tests/run.py           # integration suite only
```

Integration tests are Python stdlib `unittest` (no extra deps); they drive the
built binary against a scratch tree and assert on the hashfile and on-disk
sharing. Dedupe cases need a reflink-capable fs — override with
`DUPEREMOVE_TEST_DIR=/mnt/btrfs`. Keep tests in `tests/`; don't add shell tests.

### Building on Fedora

Missing headers (`uuid/uuid.h`, libbsd `queue.h`, xxhash) come from a dev shim:
put `.pc` files under `/tmp/devroot/pc` and build with
`PKG_CONFIG_PATH=/tmp/devroot/pc make`. This is a local workaround, not part of
the repo — don't commit it.

## Profiling & measurement — read this before optimizing

Startup/scan cost has burned us before. The rules:

- **`scripts/perf-profile.sh`** wraps all of this: it runs an oans command
  under `perf` and prints the self/leaf view, the caller/stack view, `perf stat`,
  and a syscall summary. E.g.
  `scripts/perf-profile.sh --cold -- -dr --hashfile=/tmp/prof.db ~/git`.
- **Never draw conclusions from `strace -c`.** Its per-syscall interception
  overhead inflates whatever is called most often. It once reported `statx` at
  66% (it's really ~7%) and completely hid the actual hotspot, which was
  per-file SQLite WAL locking. Use **`perf record -g --call-graph dwarf`** +
  `perf report` for where time goes, and **`perf stat -e task-clock`** for A/B
  wall-clock.
- **When A/B-testing two builds, build two distinctly-named binaries** (e.g.
  `/tmp/dm-base`, `/tmp/dm-batch`) and confirm they actually differ before
  trusting the numbers. A `git stash` + `make` that silently reused a stale
  object file once made a binary look identical to itself → a real ~24% win got
  wrongly dismissed as "no effect." Interleave runs to cancel drift.
- **A single surprising result that contradicts prior profiling is probably a
  measurement bug**, not a discovery. Reconcile before acting.

## Hashfile / SQLite gotchas

- In WAL mode a connection holds its read snapshot across queries. Wrapping the
  per-file change-detection reads in **one batched read transaction** (refreshed
  ~10s), instead of an implicit transaction per file, cut `F_SETLK` from ~2/file
  (283k on a 141k-file rescan) to ~800 total, ~24% faster. The writer batches on
  the same ~10s cadence so the reader snapshot doesn't pin the WAL against
  checkpointing. Reader and writer must stay **separate SQLite connections**.
- The listing thread reads through the listing handle (`db`) and writes through
  the batched writer handle (`wdb`/`scan_writer`) — keep that split.
- `.hashfile-wal` and `.hashfile-shm` are SQLite WAL sidecars. Normal, don't
  hand-delete them.
- **Hardlink hazard:** `INSERT OR REPLACE` on `UNIQUE(ino, subvol)` can cascade-
  delete rows for other links to the same inode. An in-memory `seen_inodes` set
  guards this; a batch that aborts here can silently empty the hashfile while
  exiting 0. There's a regression test — keep it.
- **Deleted-file pruning is automatic and stat-based.** After each scan,
  `dbfile_prune_missing_files()` drops rows whose path `stat()`s to ENOENT
  (extent/block hashes cascade via the FK), then `dbfile_maybe_vacuum` compacts
  when ≥25% is free. It must stay stat-based, not "delete rows not walked this
  run" — otherwise scanning a subdir (or a shared hashfile) would nuke
  out-of-scope files. Pinned by `test_prune_is_stat_based_not_scope_based`.

## Scan parallelism (the walk)

The directory walk (`opendir`/`readdir`/`statx`) runs on N walker threads
(`--io-threads`); every regular file is handed to a **single consumer** (the
main thread) that runs `__scan_file()`. That split is deliberate: all the
delicate per-file state — the batched writer, `dedupe_seq`, `seen_inodes`, the
batched read transaction, `subvol_cache` — stays on the one consumer and needs
no locking. Walkers only read `locked_fs` (set on the main thread before they
spawn) and the mutex-guarded `verified_devs`. **Don't move per-file DB state
onto the walker threads.**

- **Walker count plateaus at ~8 on btrfs — do not raise the default cap.**
  Measured on a cold 149k-file `~/git` scan: 1→2→4 threads scaled well
  (15s→9.9s→7.8s), 8 was the knee (7.3s), and 16/32 gave no wall-clock gain
  while `sys` time exploded (16s→23s→46s). It's btrfs metadata b-tree lock
  contention (`btrfs_search_slot` / `_raw_spin_lock`), not I/O latency, so more
  threads just burn cores. The default cap (8) is right.
- On a cold walk the cost is fundamental btrfs metadata I/O
  (`statx → btrfs_iget → btree reads`); SQLite is <2%, so parallelizing the
  consumer would not help that workload.

## io-threads auto-tuning (storage detection + --autotune)

`--io-threads` sizes three pools (walkers, the csum/read pool, the dedupe
pool). Its default is no longer a flat `min(nproc, 8)`; two mechanisms refine
it, both **only when the user didn't pass `--io-threads`** (tracked by
`options.io_threads_set`), and both resolved on the main thread *after* the
roots are known (`auto_tune_io_threads()` in `oans.c`, called before
`print_header`/`scan_files`) — the fs isn't known at the old pre-parse default
site.

- **`src/storage.{c,h}` — heuristic from device type.** `storage_detect()`
  reports rotational-ness + device count. btrfs pools have an anonymous
  `st_dev` with no `/sys/block` entry, so members are enumerated via
  `BTRFS_IOC_FS_INFO` (`num_devices`, `max_id`) + `BTRFS_IOC_DEV_INFO` (device
  path → `st_rdev` → `/sys/dev/block/MAJ:MIN/queue/rotational`, with a
  `../queue` parent fallback for partitions). Single-device fs reads the file's
  `st_dev` directly. `storage_recommend_io_threads()` is **pure and
  unit-tested** (`test_storage_recommend_io_threads`): SSD/unknown keep
  `min(nproc,8)` (the previously-validated path, unchanged); a single HDD gets
  ≤4; an HDD pool gets ~2 per device, capped at 8. These HDD constants are
  *educated guesses* — they were never measured on real spinning-disk/RAID
  hardware. Treat `--autotune` as authoritative.
- **`src/autotune.{c,h}` — empirical measurement (`--autotune`).** Re-execs
  `oans` on a bounded sample (fed as a `-` file list, **no hashfile** → pure
  in-memory read+hash) at each candidate thread count, interleaved across
  rounds, keeping each candidate's *fastest* run (per the profiling rules:
  interleave to cancel drift, min-of-N for noise). It drops the page cache
  between trials (`/proc/sys/vm/drop_caches`, needs root) and the scan's own
  `POSIX_FADV_DONTNEED` keeps the sample cold for free. Sample/round bounds:
  `DUPEREMOVE_AUTOTUNE_{MAX_FILES,MAX_BYTES,ROUNDS}`. With `--hashfile` it
  stores the winner under config key `autotune_io_threads`
  (`dbfile_{set,get}_config_int`); a later plain run reads it back and it wins
  over the storage heuristic (but an explicit `--io-threads` still overrides
  everything). Pinned by `test_autotune.py`.
- **Measuring on the dev box is misleading:** the sandbox/CI fs is often ext,
  which `is_fs_supported()` rejects, so autotune trials read 0 bytes and the
  throughput numbers are pure process-startup noise. The *mechanism* still
  runs; real numbers need a btrfs/xfs target.

## dedupe_seq (incremental dedup)

Scan assigns `seq = config+1`, bumped every `--batchsize`/`-B` files (default
1024). `process_duplicates` loops `for i=dedupe_seq; i<max` over generations;
each group is deduped exactly once regardless of batch size (verified — a
no-change rerun nets 0).

The group is never *re-deduped*, but until the cross-pass fix the loaders did
re-load and re-fiemap-check every already-deduped member of a group each pass
it appeared in — wasted work, not a correctness bug — and
`pick_least_fragmented_target` picked a fresh target per pass, so a group
spanning many passes converged to one cluster *per pass* instead of a single
extent. The `GET_DUPLICATE_*` loaders now load only the members new in a pass
plus one stable representative (min id/rowid) as the target, and mark that
dext `de_anchored` to pin the target. Force many small passes with
`DUPEREMOVE_FILES_PER_PASS` to exercise it (see `test_cross_pass.py`). Don't
reintroduce loading all `dedupe_seq <= ?2` members.

## Valgrind

Run with the suppressions file, which filters the one class of library
false-positive:

```sh
valgrind --leak-check=full --track-origins=yes \
    --suppressions=tests/valgrind.supp ./oans -rd --hashfile=/tmp/h.db <tree>
```

The suppressed noise is GLib/glibc **thread TLS** ("possibly lost" under
`pthread_create` -> `_dl_allocate_tls`): GLib caches idle pool threads instead
of joining them all at exit. Not an oans leak. Everything else must be clean —
`definitely lost` and any uninitialised-value / invalid-access errors are real.
(One such was fixed: `__dbfile_get_config` read an unterminated UUID buffer;
config string buffers from `get_config_text` are raw `memcpy`, so anything
`strlen`'d must be zero-initialised.)

## Hashfile identity (oans vs duperemove)

The hashfile schema version is `DB_FILE_MAJOR.MINOR` in `dbfile.h` (5.0; oans
forked at upstream duperemove's 4.1 and jumped to 5.0 as a deliberate clean
break). Every oans hashfile is also stamped with SQLite `PRAGMA application_id =
OANS_APP_ID` ("oans" in ASCII) and `dbfile_check()` **strictly** refuses any file
that doesn't carry the brand — foreign, or a pre-brand/duperemove file. A
brand-new empty file (`dbfile_config_empty()`) is stamped *before* the check, so
a fresh scan doesn't recreate-loop; existing files must already carry the brand.
A failed check unlinks and recreates the hashfile (it's only a cache). Bump
`DB_FILE_MINOR` on any schema change.

A from-scratch build (new hashfile or a recreated one) sets `hashfile_rebuilt`,
which makes `dbfile_maybe_vacuum()` force a one-off `VACUUM` at the end: a fresh
build is at insert density (random-key `path_hash`/digest indexes fill ~2/3), so
it lands ~15-20% smaller than the un-vacuumed size. Normal incremental runs
still only VACUUM once ≥25% of the file is free.

## Correctness invariants

- Ctrl+C is safe: `FIDEDUPERANGE` is atomic and the hashfile stays WAL-consistent
  on process kill. Only power loss risks the hashfile (due to `synchronous=OFF`).
- A no-op rescan must net **0** changes and leave row counts identical. Use this
  as a smoke test after any scan-path change.
