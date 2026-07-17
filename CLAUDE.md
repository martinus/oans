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
make -j$(nproc)                         # builds oans + helpers
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

## Correctness invariants

- Ctrl+C is safe: `FIDEDUPERANGE` is atomic and the hashfile stays WAL-consistent
  on process kill. Only power loss risks the hashfile (due to `synchronous=OFF`).
- A no-op rescan must net **0** changes and leave row counts identical. Use this
  as a smoke test after any scan-path change.
