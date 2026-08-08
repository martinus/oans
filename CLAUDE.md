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
  - **No pandoc? `make pandoc`** fetches a pinned prebuilt one into `.pandoc/`
    from PyPI (`pypandoc_binary`) — for sandboxes where GitHub is blocked but
    PyPI works; `make doc` picks it up. It's the sandbox counterpart to devenv's
    `OANS_PANDOC_BIN`. That pandoc (3.9) is newer than the committed `.8`'s, so a
    full `make doc` reformats the whole file — expected, not a bug.
  - **Multi-paragraph def-list gotcha:** pandoc ≥3.9 collapses *tight* def-list
    items (e.g. `--hashfile`) into one blob; a blank line after the term makes it
    *loose* and restores the breaks (a no-op under older pandoc). Keep the
    guarding `<!-- -->`; repeat for any new multi-paragraph option.
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
commit log if no `NOTES` file). Publishing the release fires
`.github/workflows/release.yml`, which builds a prebuilt x86_64 tarball
(`oans-X.Y.Z-linux-x86_64.tar.gz` + `.sha256`, built on Ubuntu 22.04 for a low
glibc floor) and attaches it; `workflow_dispatch` with a `tag` input backfills
assets for an existing release.

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

- **The suite runs in parallel by default** (`make integration TEST_JOBS=…`,
  `tests/run.py -j`; the why is in that file's docstring). 26.4→4.3s on 4 cores;
  the real payoff is the sanitizer builds, which re-run all of it. **A new test
  must not share state outside `setUp`'s per-test `mkdtemp` + hashfile** or it
  will flake in parallel; `TEST_JOBS=1` is the sequential fallback for pinning
  such a flake down. The suite is I/O-bound, so `auto` deliberately
  over-subscribes (`2 × nproc`, capped) — don't "fix" it back to `nproc`.
- **A test asserting on the *physical* extent layout must set `serial = True`**
  (`DuperemoveTest.serial`), which holds it back to a one-at-a-time pass after
  the pool drains. Per-test scratch isolation doesn't help here: the
  fsync-forced-extent-boundary trick and fiemap counts depend on btrfs
  writeback, which concurrent I/O perturbs — CI caught exactly this on btrfs
  (`test_extent_order_independent`, `test_streaming_dedupe`) while xfs passed.
  The four `fsync`-boundary files are already marked.

- **Never scan/benchmark out of `/tmp` — it's tmpfs**, not reflink-capable and
  rejected by `is_fs_supported()`, so a scan there stores **0 files silently**
  and dedupe is a no-op. Use real btrfs/xfs and verify a non-zero file count
  before trusting any before/after. (The `~/git` tree, ~174k files on btrfs, is
  the usual benchmark target.)

## Profiling & measurement — read before optimizing

- **`scripts/perf-profile.sh`** runs an oans command under `perf` and prints the
  self/leaf + caller/stack views, `perf stat`, and a syscall summary, e.g.
  `scripts/perf-profile.sh --cold -- -dr --hashfile=/tmp/prof.db ~/git`.
- **`scripts/bench.py` is THE benchmark harness — don't reinvent it or add new
  bench-*.sh.** One Python tool that generates reproducible trees (reusing
  `scripts/demo/gen.py`), runs a matrix of *binaries × io-threads × walk-threads ×
  env-variants* over declarative workload **profiles**, cold or warm, interleaved
  across rounds, and prints wall/user/sys (+`--rss` peak RSS) as
  median/mean/min/max. It subsumes the old `bench-scan.sh`, `bench-scan-cold.sh`
  and `bench-ram.sh` (all removed). `scripts/demo/*` stays — that's the GIF, not
  benchmarking. `median` is the headline column (robust to the cold/swap
  outliers a `drop_caches` box throws).
  - Profiles live in the `PROFILES` dict — add a key, nothing else changes:
    `realistic` (default; ~65k files + dup groups → hashing + find_dupes, the
    everyday regression check), `mixed` (pure hashing, bandwidth-bound), `bigfile`
    (one huge file → largest-first idle-tail; pair with `--io-threads 2`), `many`
    (~250k tiny dup-heavy → find_dupes pool + walk), `big` (few large → read
    buffers; pair with `--rss`), `fragmented` (8 x 512 MiB shredded to ~32k
    extents each → per-file extent-list cost; run `--warm` and pair with
    `--io-threads 2`), and `git` (an *existing* real tree, default `~/git`, via
    `--external`). Synthetic sizes target ~10 s cold on NVMe.
    - **`fragmented` is the only profile with a non-trivial extent count.** Every
      other one generates ~1 extent/file, as does a real source tree (`~/git/linux`
      measures 1.17 mean, p99 2) — so none of them can see per-file extent-list
      cost, and none would have caught the O(extents²) `get_extent` scan that was
      **49% of all CPU** until #134. That is the shape a long-lived dedupe target
      degrades into, since dedupe fragments what it shares.
    - Generating it needs a `sync` *before* the rewrites (`_fragment`): rewriting
      still-dirty pages replaces them in place, no CoW happens, and writeback lays
      the file out contiguously — the tree comes back at ~6k extents instead of
      ~32k and the profile silently measures nothing.
  - Examples: `scripts/bench.py -p mixed --bin base=/tmp/oans-master --bin new=./oans`
    (A/B two builds); `scripts/bench.py -p git --walk-threads 4,8,16,32` (thread
    sweep); `scripts/bench.py -p git --variant a: --variant b:DUPEREMOVE_FOO=1`
    (env-gated code experiment). It confirms a reflink fs and needs **btrfs/xfs,
    not tmpfs**; a scan uses **`-rq`** (so only `-r`, non-destructive, repeatable).
  - **The dedupe phase has its own harness: `scripts/bench-dedupe.py`** — the one
    sanctioned exception to "no new bench-*", because `bench.py` structurally
    can't measure dedupe (it only scans `-rq`; dedupe *mutates* the tree, so each
    round needs a fresh unshared copy, and its cost only shows larger-than-RAM).
    It builds N `--reflink=never` copies of a source tree, times the hash and
    hash+dedupe phases cold inside a `MemoryMax`-capped `systemd-run --user
    --scope`, restores the copies between rounds, and with `--verify` checks
    byte-identical sharing via `btrfs filesystem du -s`. Reuses `bench.py`'s
    `drop_caches`/reflink helpers. Canonical A/B (produces `docs/benchmarks.md`'s
    larger-than-RAM numbers): `scripts/bench-dedupe.py --baseline build:897a222
    --source ~/git/linux --cap 4G --rounds 10 --verify` (`build:897a222` builds the
    fork's pure-upstream base in a cached worktree — the only valid duperemove
    baseline; `../duperemove` and `../dm-backports` already carry fork commits).
  - **Cold runs work now:** the dev box enables `sudo tee /proc/sys/vm/drop_caches`
    via sudoers, so `bench.py` drops the page cache (metadata + data) before every
    timed run (default; `--warm` opts out). Plain `sudo -n true` still needs a
    password — only the `drop_caches` tee is passwordless.
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
- Cold-walk cost is fundamental btrfs metadata I/O (`statx→btrfs_iget→btree`);
  SQLite is <2%, so parallelizing the consumer wouldn't help.

## io-threads default (storage heuristic)

`--io-threads` sizes three pools (walkers, csum/read, dedupe). One mechanism
refines its default, **only when the user didn't pass `--io-threads`** (the
sentinel is `options.io_threads == 0`), resolved on the main thread after the
roots are known (`apply_storage_defaults()` in `oans.c`).

- **`src/storage.{c,h}` — heuristic from device type.** `storage_detect()`
  reports rotational-ness + device count (btrfs pools enumerated via
  `BTRFS_IOC_FS_INFO`/`DEV_INFO` → `/sys/.../queue/rotational`).
  `storage_recommend_io_threads()` is pure and unit-tested: SSD/unknown keep
  `min(nproc,8)` (the validated path); single HDD ≤4; HDD pool ~2/device capped
  at 8.
- **The HDD/pool constants are unmeasured guesses** and there is now no in-tree
  way to validate them — we have no spinning-disk target. The SSD/unknown path
  is the measured one (see the walker plateau in *Scan parallelism*). If you get
  access to real rotational storage, measure with `scripts/bench.py
  --walk-threads 4,8,16,32` and fix the constants; don't guess again.
- **`--autotune` was removed** (#153) — it measured warm-cache throughput
  whenever it couldn't drop caches (i.e. without root), recommended and
  *persisted* a thread count above the measured btrfs plateau, and the stored
  value then won over the heuristic on every later run. Don't reintroduce a
  self-measuring mode that writes to the hashfile. `scripts/bench.py` answers
  the same question properly, as a dev tool.
- The same `storage_detect()` call also feeds the scan-ETA rotational weight
  (`pscan_set_storage_rotational()`), which must be set **even when io-threads
  is fixed** by an explicit flag — hence the ordering in that function.

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
  reuses the progress counters, so a later read is 0. (Not `files_examined`,
  which counts every file the walk *visited*, up-to-date ones included, not just
  those hashed.) Pinned by `test_history_metrics.py`.
- **Report modes** `-L`/`-R`/`--stats`/`--history`/`--json` are mutually
  exclusive (one `report_count` check in `parse_options`): `--stats` = hashfile
  report, `-L` lists files, `-R` removes paths. All but `-R` open the hashfile
  read-only.
- **Scheduling.** `systemd/oans@.{service,timer}` (via `make install-systemd`,
  kept out of `make install`) run `oans --hashfile=/var/cache/oans/%i.hash` on a
  timer, replaying the stored config. Guide: `docs/nas-quickstart.md`.
- The `fdupes` mode was **removed** — don't reintroduce it.

## --exclude matching (src/glob.{c,h})

`.gitignore` syntax — the one git/ripgrep/fd use — not POSIX `fnmatch`. Bare
name = basename at any depth, leading `/` = absolute anchor, interior `/` = any
depth, trailing `/` = directories only, `*` stops at `/`, `**` does not.
Negation (`!`) is deliberately unsupported.

- **Breaking change in 1.6.0.** Patterns used to be `fnmatch`'d against the full
  path with relative ones resolved against the cwd, so `--exclude node_modules`
  matched at most one literal directory — usually nothing, silently (#147). Old
  patterns stored in a hashfile are replayed under the *new* semantics; that was
  a deliberate call (no syntax versioning) so there is only ever one meaning.
- Patterns compile to PCRE2 fragments joined into **one** combined `GRegex`, so
  match cost is independent of pattern count. `GRegex` is PCRE2 and GLib is
  already linked — no new dependency. Exact absolute paths skip the regex via a
  hash lookup.
- **Compile once in `filescan_init()`**, on the main thread before any walker
  exists; the set is read-only afterwards so walkers need no lock. Don't move
  the compile later or make it lazy.
- The hashfile and its `-wal`/`-shm` sidecars are added with
  `add_exclude_path()` (literal), not as globs — a `*` or `[` in the hashfile's
  own path must not become a wildcard.
- Per-pattern match counts back the "matched nothing" warning; `glob_set_stat()`
  enumerates **user** patterns only, skipping the internal ones.

## Read-only subvolumes are scanned by default (#182)

**The kernel deduplicates *into* a read-only btrfs subvolume.** Only a read-only
*mount* is refused (`vfs_dedupe_file_range_one()` → `mnt_want_write_file()`), and
a probe on 7.1.3 accepted a read-only-snapshot destination and rewrote its extent
map. Don't reintroduce "the kernel refuses read-only destinations" anywhere — it
is the false premise this took two PRs to unwind:

- #156/#164 skipped snapshots by default under `-d`, reasoning the work was
  *provably* wasted. #171/#172 disproved that but only fixed the dedupe side
  (a read-only member is *preferred as the source*, never refused), leaving the
  default keyed to the dead premise until #182 flipped it.
- The option survives as `--skip-readonly-subvols` (off by default, no longer
  tri-state/`-d`-derived) because the **cost** argument is real and workload-
  dependent: snapper/Timeshift snapshots already share the live subvolume's
  extents, so hashing 20 of them reads ~20 TiB per TiB for nothing; an
  rsync-then-snapshot backup target holds independent copies and is exactly what
  offline dedupe is for. One default can't serve both.
- Trade the default accepts: a writable file deduped against a snapshot is
  rewritten to point at *the snapshot's* extents, so deleting that snapshot then
  frees less than its size suggests.
- 1.7.x hashfiles store `opt_skip_readonly_subvols = -1` (the old "auto"). That
  means "the user never asked", so `dbfile_load_scan_config` coerces `< 0` to 0 —
  a replay adopts the new default; only an explicit `1` survives.

## Measured dead-ends — don't re-attempt without new evidence

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

## Streaming dedupe pipeline (Stage 2)

The dedupe phase (`-d`) runs **one persistent thread pool** for the whole phase;
the main thread is a **bounded producer** that loads generation batch *i+1* while
batch *i* dedupes. No pool drain between batches or between the whole-file and
extent passes. Lives in `run_dedupe.c` (`dedupe_phase_begin/end`,
`dedupe_begin_batch`/`dedupe_push`/`dedupe_seal_batch`) driven by
`stream_duplicates()` in `oans.c`. The report path (no `-d`) stays sequential
(`report_duplicates()`).

- **At most `DEDUPE_MAX_INFLIGHT` (2) batches in flight** — the RAM double
  buffer. `dedupe_await_slot()` blocks the producer until a slot frees.
- **Generation-ordered watermark.** Batches are reaped strictly in FIFO
  (generation) order; `dedupe_seq` only advances to a batch's `seq_hi` once that
  batch *and all earlier ones* are complete (`dedupe_advance_seq` callback, under
  `dbfile_lock`). This preserves the **Ctrl+C invariant**: on kill, `dedupe_seq`
  reflects only fully-processed generations. Empty windows still advance it.
- **Filerec lifetime = batch-held refs (NOT per-extent).** `struct filerec.refs`
  is a lifetime count separate from `fd_refs`. Each batch holds one ref per
  filerec it loaded (taken in `push_results`, released in `free_batch` at reap).
  **All get/put/new/find happen on the single producer thread**, so the filerec
  registry (`filerec_by_fileid` tree / `filerec_head`) needs **no lock** and
  workers never touch it — they only read `filerec` fields and mutate their
  batch's own results tree (under the global `mutex`). A cross-window anchor is
  referenced by two in-flight batches; each holds a ref, so it survives until
  both reap. **Never free filerecs while a batch referencing them is live**
  (`free_all_filerecs()` is teardown-only now; it's gone from between batches).
  This is a deliberate, safer alternative to the plan's per-extent/worker-release
  refcount — same lifetime guarantee, far less lock surface.
- **Producer DB handle.** The producer loads on a **separate read connection**
  (`dbfile_open_handle`) so it doesn't share a sqlite handle with the workers
  writing on the global handle (WAL: readers don't block the writer). The
  in-memory shared-cache db (no `--hashfile`) has no WAL, so there the producer
  reuses the global handle and serializes loads with `dbfile_lock()` (`inmem`).
- **Order-independence (Stage 2.1).** `GET_DUPLICATE_EXTENTS` excludes whole-file
  dup members *statically* (the `FILEDUP_MEMBER` predicate) instead of relying on
  the whole-file pass having deleted their extent rows first — the prerequisite
  for loading both passes without a barrier. As a bonus the two per-batch results
  trees reference **disjoint** filerec sets. Pinned by
  `test_extent_order_independent.py`; streaming by `test_streaming_dedupe.py`.
  **Keep it a correlated probe, not a materialized set:** a `group by digest,
  size` CTE here cost ~6 s per batch load on a 3.5M-file hashfile (the producer
  stalls at "loading duplicate extents" while the pool idles; measured 21.7 s →
  0.2 s per load after the switch).
- **A pushed dext belongs to the worker.** The moment `g_thread_pool_push()`
  hands a group to the pool, a worker can free it (an already-shared group is
  cleaned in microseconds) — the producer must capture anything it needs
  (`dext_work()` etc.) **before** the push. Reading after once fed
  `len * (0 - 1)` from a freed dext into the pushed-work total (frozen bar,
  multi-thousand-year ETA); `push_results` now asserts sane per-group work.
- **Partial mode drains.** `find_additional_dedupe()` walks the **global**
  filerec list, so with `--dedupe-options=partial` the producer calls
  `dedupe_drain()` before the block-hash search — earlier batches are reaped
  (filerecs freed) first, at the cost of cross-batch pipelining in that mode.
  Default mode keeps the full overlap; don't add other drain points.
- **The search must outlive nothing.** `find_additional_dedupe()` waits for
  every worker it pushed (its own counter/cond in `find_dupes.c`) before
  returning, because the caller reaps batches — freeing filerecs — right after.
  Don't route that wait through `psearch_join()`: that's a *progress* concern
  and no-ops during the dedupe phase, which was #123 (a UAF that reproduced in
  ~5% of memcheck runs). `free_batch()` asserts `extents_search_idle()`, and
  `DUPEREMOVE_SEARCH_DELAY_MS` makes the race deterministic for the regression
  test (`test_partial_search_waits_for_its_workers`).
- **Valgrind is the gate** (`make integration-valgrind`): this is the UAF-prone
  area (see the "Valgrind" section / PR #105). Run it before any PR here — but
  note it did **not** catch the pushed-dext race above (valgrind's serialization
  makes a fast worker win too rarely); producer-vs-worker lifetime rules need
  review, not just the suite.

## Dedupe must converge — the already-shared skip (#186)

**A second `-d` run over an unchanged tree must reclaim 0.** Nothing in the
output tells you when it doesn't: `FIDEDUPERANGE` returns `bytes_deduped` for a
range that already shares storage, so a run that frees nothing still reports
gigabytes. On a 415 GiB home, `oans -dr ~/` claimed **6.6 GiB every single run**
with a measured free-space change of **0**. Check convergence by *repeating the
command*, not by reading the summary.

Three separate causes, each worth a fix; the numbers are successive stages on
that same tree (6.6 GiB → 1.4 GiB → 131 MiB → 21.3 MiB → **0 B**):

- **`clean_deduped()` culls against the target, not pairwise.** It used to
  collapse a group to one survivor per distinct physical offset. Only that
  survivor is repointed by the ioctl, so the rest of its class stayed on the old
  extent and that extent was never freed — one copy moved per run, nothing
  reclaimed until the last one landed. **Whole-file groups load with `poff = 0`**
  (`GET_DUPLICATE_FILES` has no fiemap to draw on) so they never hit this, which
  is why `--dedupe-options=only_whole_files` converged where the default did not.
  That asymmetry is the diagnostic — if only_whole_files converges, suspect the
  extent path.
- **`fiemap_range_shared_with()` compares coverage, not extent records.** The
  same shared storage is described with different record boundaries in each
  file: a dedupe stops on a block boundary and splits the destination's tail
  where the target has one record. Record-for-record equality reads that as "not
  shared" and resubmits the whole file forever. **Never do `fe_physical + delta`
  arithmetic** — on a compressed extent `fe_physical` addresses the compressed
  extent as a whole, so an offset into it is meaningless; compare `fe_physical`
  for equality only, at points where both sides sit the same distance into their
  record. Holes count too: fiemap omits them, matching holes on both sides *are*
  shared (a browser cache is mostly hole), and a hole facing data is not.
- **Compare whole blocks only.** The kernel rounds a dedupe length down to a
  filesystem block, so a trailing partial block can never be shared and
  comparing it makes the check permanently unsatisfiable. Trim with
  `dedupe_blocksize()` — but **fall back to the untrimmed length below one
  block**, or the skip is silently disabled for every sub-block file (that alone
  was the last 21.3 MiB).

Diagnosing this class: **bisect by scope.** Every subdirectory and every *pair*
of subdirectories converged while their union did not — that pattern means the
group got bigger, not that some directory is cursed.

Pinned by `tests/integration/test_dedupe_idempotent.py`,
`test_extent_dedupe.py::test_group_on_two_physical_extents_converges_in_one_run`
and the `test_fiemap_maps_share` unit test (the map walk is pure and worth
testing directly — synthetic fiemap records beat coaxing btrfs into a layout).

Two traps when writing tests here, both of which make a test pass while testing
nothing:

- **coreutils `cp` and `cat` reflink.** They go through `copy_file_range()`,
  which btrfs implements as a clone — `cp --reflink=never` does *not* give you
  an independent copy. Use `dd`, or write the bytes from python.
- **A file below btrfs `max_inline` (2048 by default) is stored inline**, has no
  physical location, and the kernel declines to dedupe it. Any "reclaimed 0"
  assertion on such a file holds no matter what the code does.

## Dedupe progress is byte-weighted (not group-counted)

The dedupe bar tracks the kernel **byte-verify volume**, not a fuzzy group
count, so it moves smoothly 0→100% (like hashing) even through one giant group.

- **Work of a group = `de_len * (de_num_dupes - 1)`** — the bytes the kernel
  byte-compares, same figure `cmp_dext_work()` sorts by. The exact phase total
  is summed up front by `dbfile_count_dupe_bytes()` (whole-file + extent, minus
  extents owned by whole-file dup members, since the whole-file pass deletes
  those rows first). Passed `seq_lo = first_seq`, so it matches what the
  per-pass loaders hand the workers regardless of how generations split — the
  sum is batch-invariant (`test_progress_bytes.py::…_stable_across_passes`).
- **Settlement contract:** every group credits *exactly* `W0` bytes to
  `work_done_bytes` by the time its worker returns — ticked smoothly per ≤32 MiB
  ioctl round (via `ctxt->progress_fn`), plus a settle-up lump in
  `dedupe_worker()` for whatever was skipped (clean_deduped, already-shared,
  changed-since-scan, ENOENT/EINVAL, the DEDUPE_EXTENTS_CLEANED early return).
  **Don't try to credit each skip path exactly** — capture `w0` before the work
  (the worker can free `dext`) and settle the shortfall once. Over-ticking is
  fine: the 99% cap + monotone display clamp + exact upfront total absorb it.
- Block-hash-discovered groups (`--dedupe-options=partial`, off by default)
  aren't in the upfront total; each is added via `pdedupe_add_pushed_work()` at
  push time and the renderer clamps `total = max(upfront, pushed)`.
- `--progress=json` emits `work_done_bytes`/`work_total_bytes` **raw** (no
  monotone clamp — machine consumers want truth); `pdedupe_end()` emits one
  final dedupe record so the last line shows the settled `done == total`.
- **The pre-analysis scales with the new work, not the hashfile (#184).** Both
  figures come from one `dbfile_count_dupe_work()` call — one query per pass
  yielding count *and* sum, since they group over the identical row set — and
  `FILES_GROUP_IS_NEW`/`EXTENTS_GROUP_IS_NEW` restrict each to groups with a
  member newer than `first_seq`, as a `WHERE` on the **group key** (index seek)
  rather than a `having` verdict reachable only after grouping everything. On
  2M files/2M extents: 1% new 8.95 s → 0.10 s. **The trade:** the scoped form
  loses the index-ordered group-by, so at ~100% new (a first scan, or any run
  without `--hashfile`) it is *slower* — 8.98 s → 12.5 s, crossing over near
  50% new. Right side of the trade: a first scan spends far longer hashing,
  while the incremental case is every scheduled run.
- The group estimate is therefore **per-run**, not lifetime, which it always
  should have been — `pdd.done` counts groups deduped *this* run, so
  `max(estimate, queued)` was comparing a lifetime figure to a per-run one and
  the bar sat low (one new pair among four deduped read "1 / ~5 groups").
- `process_duplicates()` also skips the call entirely when `passes == 0`. That
  is **insurance, not the fix** — with the macros the no-op case already costs
  ~0.5 ms. It matters only if the search indexes are missing, since
  `dbfile_create_search_indexes()` is best-effort.

## Nothing prints past the live block (#179)

The block is redrawn by moving the cursor up `drawn_lines`, so **every byte
written while one is on screen must go through `progress_printf()`** — i.e. the
`dprintf`/`vprintf`/`qprintf`/`eprintf` macros, never a bare `printf`. The
mechanism and the failure mode are documented at the definition in `progress.c`;
what belongs here is when it bites:

- **"A block is on screen" ≠ "a printer thread is running."** The scan hands its
  block straight to the dedupe phase (`pscan_join(continues=true)` …
  `pdedupe_begin()`) with no printer alive across the gap, so `progress_printf()`
  routes on `printer || block_live()`. Keying it on the printer alone was #179:
  one stranded worker row per unrouted line, *and* the scan-skip report silently
  erased on every run that had one.
- **Route whole lines.** One call is one erase/print/redraw cycle, so a message
  assembled from several calls redraws the block between the pieces —
  `report_scan_skips()` builds its whole report in a `GString` first.
- Non-tty output is untouched (`block_live()` is gated on `tty`), which is also
  why the plain integration suite can't see any of this. The regression test
  drives a real pty and replays the stream through a small ANSI emulator:
  `tests/integration/test_progress_tty.py`. Its invariant — *no worker-slot row
  may survive the end of a run* — catches the whole class, not just this site.
- **Known gap, not yet fixed:** a routed `eprintf` lands on **stdout**, because
  `print_above_block()` only knows the stream the block lives on. So an error's
  destination depends on whether a block happened to be up. Pre-existing; fixing
  it means flushing across the two streams mid-redraw.

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

- **`make integration-valgrind`** runs the *whole* end-to-end suite under
  memcheck — each `oans` invocation via `tests/valgrind-wrap.sh` (set
  `DUPEREMOVE=` to it, `OANS_VG_LOGDIR=` for per-pid logs). Findings land in
  `.vglogs/`; a non-empty log fails the target. ~7× slower than plain
  `integration` (so opt-in, not in `check`), but it catches use-after-free /
  leaks the plain suite reads straight past — it found the `dbfile_prepare`
  recreate-path UAF (a rejected hashfile reopened into a by-value local, so the
  caller kept the closed handle; fixed by passing `sqlite3 **`). The CI
  `valgrind` job runs it (btrfs only — memory behaviour is fs-independent).

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
  the extras. Self-describing and run-history were both added this way, left at
  `5.0`. Removing a key is equally safe: `--autotune`'s orphaned
  `autotune_io_threads` row is simply never read again (no bump, no migration).
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
