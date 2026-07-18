# oans — working notes for Claude

`oans` (a fork of duperemove) finds duplicate extents and deduplicates them via
the kernel `FIDEDUPERANGE` ioctl (atomic, byte-verified). Hashes live in a
SQLite **hashfile** (WAL mode, `synchronous=OFF`, `cache_size=-65536` = 64 MB).

C sources are under `src/` (main `src/oans.c`); man-page sources under
`docs/man/`. The binary is `oans`; `make install` adds a `duperemove` compat
symlink. Some identifiers keep the old name on purpose: the `DUPEREMOVE*` env
vars and the `DuperemoveTest` python base class.

**Local dev box:** `source scripts/devenv.sh` once per shell (see its header) so
`make`, `scripts/verify.sh`, etc. need no env prefixes. This box can't `dnf
install` the `-devel` packages, hence the `/tmp/devroot` pkg-config shim; a
normal machine with the README's deps needs none of it.

## Repo layout & workflow (read first)

- **This checkout is a git *worktree*.** `master` is checked out in a sibling
  worktree (`../duperemove-master`), so `git checkout master` here fails. Branch
  with `git fetch origin && git checkout -b <branch> origin/master`; park the
  worktree afterwards with `git checkout --detach origin/master`.
- **GitHub is a fork:** every `gh` command needs `--repo martinus/oans`.
- **Never merge a PR without the user explicitly saying "merge it".** Rhythm:
  branch → PR → wait. The user often asks for a `/simplify` pass first.
- **`scripts/verify.sh`** is the pre-PR gate: build (warnings = failure),
  `make check`, and a valgrind scan+dedupe+replay smoke.
- **`make doc`** regenerates the man page from `docs/man/oans.md` and needs
  `pandoc` (on `PATH` via `devenv.sh`). roff escapes `-` as `\-`, so grep the
  generated `.8` accordingly.
- Confirm you're testing *this* `./oans`, not a system `duperemove`, before
  diagnosing runtime behaviour.

## Releasing

The version is `git describe --tags` (Makefile) — **a release is just a tag**,
no `VERSION` file. **`scripts/release.sh`** encodes the convention in two phases
(merging the bump PR is a human step):

```sh
scripts/release.sh prepare X.Y.Z          # bump man pages, run verify.sh, open the PR
#   → review & merge the "Bump version to X.Y.Z" PR
scripts/release.sh publish X.Y.Z [NOTES]  # tag the merged master + create the GH release
```

`prepare` refuses on a dirty tree or existing tag; `publish` refuses until the
bump is on `origin/master`, then tags and releases (auto-seeds notes from the
commit log if no `NOTES` file). Releases attach no build artifacts.

- **Incremental semver.** The CLI is a superset of duperemove's and hashfiles
  auto-rebuild, so feature batches are backward-compatible → **minor** bumps
  (`1.1.1`→`1.2.0`); reserve major for a real break. (`1.2.0` was the first
  release carrying the fork's headline features.)
- **`gh pr merge --merge` is blocked by the local permission classifier** — use
  `--squash`.

## Build & test

```sh
make -j$(nproc)                        # build oans
make check                             # C unit tests + Python integration suite
DUPEREMOVE=./oans python3 tests/run.py # integration suite only
```

Integration tests are stdlib `unittest` (no deps); they drive the built binary
against a scratch tree and assert on the hashfile and on-disk sharing. Dedupe
cases need a reflink fs (`DUPEREMOVE_TEST_DIR`, set by `devenv.sh`). Keep tests
in `tests/`; no shell tests.

- **Never scan/benchmark out of `/tmp` — it's tmpfs**, not reflink-capable and
  rejected by `is_fs_supported()`, so a scan there stores **0 files silently**
  and dedupe is a no-op. Use real btrfs/xfs and verify a non-zero file count
  before trusting any before/after. (The `~/git` tree, ~174k files on btrfs, is
  the usual benchmark target.)

## Profiling & measurement — read before optimizing

- **`scripts/perf-profile.sh`** runs an oans command under `perf` and prints the
  self/leaf + caller/stack views, `perf stat`, and a syscall summary, e.g.
  `scripts/perf-profile.sh --cold -- -dr --hashfile=/tmp/prof.db ~/git`.
- **Never trust `strace -c`** — its interception overhead inflates the
  most-called syscall. It once reported `statx` at 66% (really ~7%) and hid the
  real hotspot (per-file SQLite WAL locking). Use `perf record -g --call-graph
  dwarf` for where time goes, `perf stat -e task-clock` for A/B wall-clock.
- **A/B-test two distinctly-named binaries** and confirm they differ before
  trusting numbers — a stale object file once made a build look identical to
  itself and a real ~24% win got dismissed. Interleave runs to cancel drift.
- **A lone surprising result that contradicts prior profiling is probably a
  measurement bug**, not a discovery. Reconcile before acting.

## Hashfile / SQLite gotchas

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
  cadence so the reader snapshot doesn't pin the WAL against checkpointing.
  Reader and writer must be **separate connections** (`db` listing handle vs
  `wdb`/`scan_writer`).
- `.hashfile-wal` / `.hashfile-shm` are SQLite WAL sidecars — don't hand-delete.
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

## Scan parallelism (the walk)

The walk (`opendir`/`readdir`/`statx`) runs on N walker threads
(`--io-threads`); every regular file goes to a **single consumer** (the main
thread) running `__scan_file()`. Deliberate: all delicate per-file state (batched
writer, `dedupe_seq`, `seen_inodes`, batched read txn, `subvol_cache`) stays on
the one consumer, lock-free. Walkers only read `locked_fs` (set before they
spawn) and mutex-guarded `verified_devs`. **Don't move per-file DB state onto the
walkers.**

- **Walker count plateaus at ~8 on btrfs — don't raise the cap.** Cold 149k-file
  `~/git`: 1→2→4 scaled (15→9.9→7.8s), 8 was the knee (7.3s), 16/32 gave no wall
  gain while `sys` exploded (16→23→46s). It's btrfs metadata b-tree lock
  contention (`btrfs_search_slot`/`_raw_spin_lock`), not I/O.
- Cold-walk cost is fundamental btrfs metadata I/O (`statx→btrfs_iget→btree`);
  SQLite is <2%, so parallelizing the consumer wouldn't help.

## io-threads auto-tuning (--autotune)

`--io-threads` sizes three pools (walkers, csum/read, dedupe). Two mechanisms
refine its default, both **only when the user didn't pass `--io-threads`**
(`options.io_threads_set`) and both resolved on the main thread after the roots
are known (`auto_tune_io_threads()` in `oans.c`).

- **`src/storage.{c,h}` — heuristic from device type.** `storage_detect()`
  reports rotational-ness + device count (btrfs pools enumerated via
  `BTRFS_IOC_FS_INFO`/`DEV_INFO` → `/sys/.../queue/rotational`).
  `storage_recommend_io_threads()` is pure and unit-tested: SSD/unknown keep
  `min(nproc,8)` (the validated path, unchanged); single HDD ≤4; HDD pool
  ~2/device capped at 8. **The HDD constants are unmeasured guesses** — treat
  `--autotune` as authoritative.
- **`src/autotune.{c,h}` — empirical (`--autotune`).** Re-execs oans on a bounded
  sample (`-` file list, no hashfile → pure in-memory read+hash) at each thread
  count, interleaved, keeping each candidate's fastest run. Drops the page cache
  between trials (`drop_caches`, needs root). Bounds:
  `DUPEREMOVE_AUTOTUNE_{MAX_FILES,MAX_BYTES,ROUNDS}`. With `--hashfile` it stores
  the winner (config key `autotune_io_threads`); a later plain run reads it back
  (an explicit `--io-threads` still overrides). Pinned by `test_autotune.py`.
- **Measuring autotune on the dev box is misleading:** on an unsupported fs
  (ext) trials read 0 bytes and the numbers are startup noise; needs a btrfs/xfs
  target.

## Self-describing hashfile, history & scheduling (fork features)

- **Self-describing hashfile.** Each run stores its scan-shaping options (`opt_*`
  keys in `config`), roots (realpath'd) and `--exclude` patterns
  (`scan_roots`/`scan_excludes` tables) via
  `dbfile_store_scan_config`/`load_scan_config`. A bare `oans --hashfile=FILE`
  **replays** the last run (`apply_scan_config`+`drop_missing_roots`):
  last-run-wins, other options ignored, a missing root skipped with a warning —
  but if *all* roots are gone oans refuses (so the stat prune can't wipe the
  hashfile, e.g. an unmounted drive); a replay doesn't re-persist. Pinned by
  `test_self_describing.py`.
- **Run history & metrics.** Each run appends to `run_history`
  (`dbfile_record_run`, from `main()` after `process_duplicates`). `--history` =
  human timeline + lifetime totals (`dbfile_get_run_summary`); `--json` = a flat
  metrics object for jq/Telegraf. Gotcha: take the per-run file count from
  `pscan_files_scanned()` (out-param) **inside `scan_files`** — the dedupe phase
  reuses the progress counters and `pscan.files_examined` is cleared ~10×/s by
  the renderer. Pinned by `test_history_metrics.py`.
- **Report modes** `-L`/`-R`/`--stats`/`--history`/`--json`/`--autotune` are
  mutually exclusive (one `report_count` check in `parse_options`): `--stats` =
  hashfile report, `-L` lists files, `-R` removes paths.
- **Scheduling.** `systemd/oans@.{service,timer}` (via `make install-systemd`,
  kept out of `make install`) run `oans --hashfile=/var/cache/oans/%i.hash` on a
  timer, replaying the stored config. Guide: `docs/nas-quickstart.md`.
- The `fdupes` mode was **removed** — don't reintroduce it.

## Measured dead-ends — don't re-attempt without new evidence

- **Warm rescan is single-consumer pipeline-latency-bound**, not CPU/query. A
  no-op rescan of ~174k files is ~2s wall / ~0.9s CPU, invariant to
  `--io-threads` (1..16); the serial consumer (`__scan_file` queue handoff +
  per-file `dbfile_describe_file`, only ~0.12s CPU) sets the floor.
- **Bulk in-memory change-detection cache** (preload `files` to skip the per-file
  `describe_file`) was prototyped and **measured a regression** (2.0→3.1s): the
  query isn't the bottleneck and the preload adds ~1s.
- **statx-relative-to-dir-fd** gave ~5-8% less scan *CPU* but **zero wall**
  change; PR closed.
- **A savings preview / dry-run isn't worth building.** Real reclaimed disk can't
  be predicted without doing the dedupe (compression, extent alignment, the
  kernel declining, snapshot/external refcounts), and dedupe is already safe and
  self-reporting. `--stats` gives the logical upper bound. (A compsize-style
  *physical* estimate needs root — `BTRFS_IOC_LOGICAL_INO` — so it can't back an
  unprivileged preview either.)
- **"Biggest-savings-first" order already exists:** `push_extents` qsorts by
  `cmp_dext_work = de_len*(de_num_dupes-1)` (reclaimable bytes, desc). Only per
  generation-pass, not global — not worth changing.

## dedupe_seq (incremental dedup)

Scan assigns `seq = config+1`, bumped every `--batchsize`/`-B` files (default
1024). `process_duplicates` loops `for i=dedupe_seq; i<max` over generations;
each group is deduped exactly once (a no-change rerun nets 0). The
`GET_DUPLICATE_*` loaders load only the members new in a pass plus one stable
representative (min id) as the target, marked `de_anchored` to pin it — this
fixed both wasted per-pass re-load/re-fiemap of already-deduped members and
per-pass target drift (a group spanning passes used to converge to one cluster
*per pass* instead of a single extent). Exercise with `DUPEREMOVE_FILES_PER_PASS`
(`test_cross_pass.py`); don't reintroduce loading all `dedupe_seq <= ?2` members.

## Valgrind

`verify.sh` runs the smoke. Manual, with the suppressions file (filters the one
library false-positive):

```sh
valgrind --leak-check=full --track-origins=yes \
    --suppressions=tests/valgrind.supp ./oans -rd --hashfile=/tmp/h.db <tree>
```

The suppressed noise is GLib/glibc **thread TLS** ("possibly lost" under
`pthread_create`→`_dl_allocate_tls`: GLib caches idle pool threads). Everything
else must be clean — `definitely lost` and any uninitialised-value /
invalid-access errors are real. (One fixed: `__dbfile_get_config` read an
unterminated UUID buffer; `get_config_text` buffers are raw `memcpy`, so anything
`strlen`'d must be zero-initialised.)

## Hashfile identity & schema version

Schema version is `DB_FILE_MAJOR.MINOR` in `dbfile.h` (5.0; forked at upstream
4.1, jumped to 5.0 as a clean break). Every hashfile is stamped `PRAGMA
application_id = OANS_APP_ID` ("oans"); `dbfile_check()` strictly refuses any file
without the brand (foreign, or pre-brand/duperemove). A brand-new empty file is
stamped *before* the check (so a fresh scan doesn't recreate-loop); a failed
check unlinks and recreates (it's only a cache).

- **Bump `DB_FILE_MINOR` only for a change an old binary could misread.**
  `dbfile_check()` rejects a differing `minor`, discarding and rebuilding the
  file (a full re-scan). Do **not** bump for purely *additive* changes (a new
  `CREATE TABLE IF NOT EXISTS`, or an optional `config` key): `create_tables()`
  runs every open, so additive tables appear on old files and old binaries ignore
  the extras. Self-describing, run-history and autotune were all added this way,
  left at `5.0`.
- A from-scratch build sets `hashfile_rebuilt` → `dbfile_maybe_vacuum()` forces a
  one-off `VACUUM` (a fresh build is at insert density, ~15-20% larger
  un-vacuumed). Incremental runs only VACUUM at ≥25% free.

## Correctness invariants

- **Ctrl+C is safe:** `FIDEDUPERANGE` is atomic and the hashfile stays
  WAL-consistent on kill. Only power loss risks it (`synchronous=OFF`).
- **A no-op rescan must net 0 changes** and leave row counts identical — a smoke
  test after any scan-path change.
- **"Reclaimed" is a logical figure, not disk.** It's the honest bytes freed
  (kernel-deduped = one physical copy kept per group), but on compressed btrfs
  dedupe frees *compressed* blocks while the number is *logical*, so real disk
  freed is ~ratio smaller. Verify with `compsize` **Disk Usage** before/after
  (not `df`); `Referenced` staying constant proves nothing was lost. The piped /
  `-q` "net change in shared extents" line is a separate fiemap diagnostic
  (counts the surviving copy too, ~2× for pairs); `--json`
  `reclaimable_logical_bytes` is a logical upper bound.
