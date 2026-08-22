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

## Mutation & property testing (the C unit suite)

Two additions to `src/tests.c`'s side of the house. Neither touches the
integration suite, and the distinction matters for reading either of them.

### `scripts/mutate/mutate.py` — does anything notice when this breaks

Breaks a source file in a throwaway copy of the tree, rebuilds, runs `./test`,
and reports whether anything went red. Coverage says a line ran; this says
something would have noticed it misbehaving.

```sh
scripts/mutate/mutate.py --bugs scripts/mutate/bugs/fiemap.txt
make mutation-replay                                    # every bug file, as CI does
scripts/mutate/mutate.py --file src/glob.c --diff       # only what you changed
scripts/mutate/mutate.py --file src/util.c --dry-run    # how many, and how long
```

- **`--file` is not optional in practice.** oans is not a single-header library,
  so there is no file that is obviously *the* code; the default (`src/csum.c`)
  is a starting point and a run without `--file` is usually the wrong question.
- **`bugs/*.txt` are real oans bugs put back** — #147, #159, #186, #187, #191,
  #202 — one block each, and each file names the source it mutates on its own
  first line (`# file: src/util.c`). That is the everyday mode and the one worth
  adding to: a fix without a block here has no answer to "does its test earn its
  place". A block that stops applying means that part was rewritten and the
  question wants re-deriving, not repairing.
  - **The target belongs in the file, not in its name.** Deriving it from the
    filename works until a bug file is named after a theme rather than a source,
    which `escape.txt` was — and it bought a special case in the CI loop for
    exactly one file. `make mutation-replay` now runs the lot with nothing to
    know, and `make lint` refuses a bug file whose `# file:` line is missing or
    names something that is not there.
- **A `survived` is a narrower claim than it reads.** The suite is `src/tests.c`
  alone: the end-to-end Python suite drives the *binary* and is not in the loop,
  so for the dedupe phase, the scan pipeline and the progress block a survivor
  means nothing at all. The tool says so in its own fingerprint.
- **No sanitizer by default**, so a mutant that reads one slot too far only
  shows up if it corrupts something a test checks. Re-run a surprising survivor
  with `--make-arg SANITIZE=address,undefined --make-arg CC=clang`.
  - **`tests/lsan.supp` was hiding oans's own leaks, and this is how that was
    found.** Its `leak:libglib-2.0` line matches any leak whose stack passes
    through GLib, and oans keeps nearly everything in GLib containers
    (`GPtrArray`, `GHashTable`, `GRegex`, `GString`) — so it suppressed most of
    oans's heap along with the cached-thread residue it was written for.
    Measured: deleting one `g_free()` from `glob_set_free()` leaks 769,200
    bytes in 32,050 allocations, LSan names `glob_set_new()` as the site, and
    the run still exits **0**. Measured over a whole `src/glob.c` sweep, all
    three on one tree: **plain 64 survivors, ASAN with this file 50, ASAN with
    the module-wide pattern 64** — so under the broad file the sanitizer leg was
    worth *nothing at all* over a plain build on this file, and narrowing
    recovers 14, every one a deleted `g_free`/`g_string_free`/`g_regex_unref` or
    an early `return` that skips one. `tests/lsan.supp` now names three
    GLib functions and nothing wider, for **both** legs. A handful of
    deallocation mutants still survive it, on error paths the suite never
    reaches — untested code, not an untested leak.
  - **Narrowing it for the end-to-end leg was measured, not assumed**, which
    took getting around the missing btrfs: force `dedupe_probe_fd()` to answer
    `DEDUPE_SUPPORT_YES` in a throwaway tree (before #232 the same trick was
    stubbing `is_fs_supported()`) and the binary runs its whole scan and dedupe
    pipeline on ext4 — the ioctl fails, but the walkers, the csum pool, the
    dedupe pool, sqlite and the progress block all run. Scan, all three dedupe modes,
    `--io-threads=16`, `--exclude`, `--progress=json`, in-memory, replay,
    `--stats`, `--json`: **zero leaks with no suppressions at all**, and under
    `print_suppressions=1` the three entries never fire. They stay as insurance
    against a GLib that caches more eagerly (measured on 2.80), so one of them
    starting to match is worth looking at rather than assuming.
  - What that could not reach is a *successful* `FIDEDUPERANGE`, which ext4
    refuses — so allocations on the accepted-destination path are unverified,
    and are one CI run on btrfs from being answered.
- **`make test-build` exists for this** and only this: `make test` builds *and
  runs*, so a mutant the tests caught would exit nonzero at the build step and
  be scored `compiler` — the compiler credited with protection the tests
  provided. Don't merge the two targets back together.
- **`mutate_core.py` is vendored** from unordered_dense, where its own hermetic
  suite (`scripts/test_mutate.py`) lives; nanobench holds the same copy. Never
  edit it here — change it there, run that suite, re-copy into all three and
  update each `mutate_core.sha256`. `make lint` fails if this copy has drifted.
  Only `scripts/mutate/mutate.py` (the ~130-line adapter) is oans's.
- A mutant costs one compile of `src/tests.c` (~4.5 s) — every source is
  `#include`d into it, so there is no incremental build and ccache cannot help.
  The `-fsyntax-only` pre-filter rejects the invalid ones at about a tenth of
  that.
- **Read a survivor count by function, never as a total.** The first sweep of
  `src/util.c` came back 213 survivors of 413 and that number says nothing:
  grouped by function it was 5-9% survival everywhere a test existed
  (`ctrl_seq_len` 2/37, `sanitize_ctrl` 4/58) and 85-94% in four pure functions
  that had *no test at all* — `parse_size`, `human_size_snprintf`,
  `human_duration_snprintf`, `num_digits`, together 113 of the 213. Writing
  those took it to 94. So the tool measures absent tests and weak tests with
  the same number, and only the grouping tells them apart.
  - `parse_size` is why it mattered: a ladder of `switch` fallthroughs, so one
    missing `mult *= 1024` makes `--max-filesize=10G` mean ten megabytes on a
    run that otherwise looks right. The integration suite passes `1K` and `1M`,
    which left `g`/`t`/`p`/`e` unexercised anywhere.
- **Triage the residue rather than reporting it.** What was left at 94 is
  ~61 mutants in `setrlimit`/`sysconf`/`clock_gettime`/`backtrace` code no unit
  test can reach, 14 on `parse_size`'s `exit()` paths (a test that reaches them
  takes the suite with it), and a handful that are provably equivalent —
  `memcpy(buf, "\\t", 3)` copies the literal's own NUL into a scratch buffer
  whose third byte is never read; `u < ARRAY_SIZE(units) - 1` cannot differ
  from `<=` because `v >= 1024.0` fails first, UINT64_MAX being 16 EiB. Check
  the equivalents rather than assuming them: three that looked equivalent were
  not, and one of those wrote a byte past a buffer.

### `src/proptest.h` — properties, not tables

An ordinary test names an input and its answer; a property names a relationship
that must hold for *every* input and generates inputs looking for a
counterexample. No dependencies, one header, minunit-compatible.

- **The seed is fixed on purpose** (`make test` is reproducible, CI can trust
  it); `OANS_PROPTEST_SEED=random ./test` goes looking and prints what it chose,
  and every failure names the seed and case number to replay. Each property
  draws from its own stream, mixed from the seed and the test's name, so adding
  one does not renumber the cases in all the others.
- **There is no shrinking**, which is most of what a real property-testing
  library is. The substitute is generators that only produce small inputs, so a
  counterexample is already readable. A property needing a large input to mean
  anything should be an ordinary test with a fixture.
- **Three properties are skipped under valgrind** (`OANS_PROPTEST_NO_JIT=1`,
  which the CI leg sets and which prints what it skipped). They compile a
  `GRegex`, GLib compiles every pattern with `G_REGEX_OPTIMIZE`, and memcheck
  cannot see into the machine code PCRE2 then emits — hundreds of "conditional
  jump depends on uninitialised value" from frames with no symbol and a stack
  address where a return address belongs. **Verified to be the library and not
  oans**: a standalone program containing no oans code is clean under memcheck
  with plain `g_regex_new` and reproduces the identical signature the moment
  `G_REGEX_OPTIMIZE` is added. Don't reach for a suppression instead — the
  frames have no object name, so valgrind's own generated entry is
  `Memcheck:Cond` over `obj:*`, which would hide every uninitialised-value error
  in the process, and that check is what caught the missing memset in
  `start_running_checksum()`.
- **Keep the suite fast, for a reason beyond taste.** The mutation tool derives
  a mutant's hang timeout from how long a green run takes, so a slow suite
  reclassifies slow mutants as `hang` instead of letting a test catch them —
  measured: at 1.2 s two glob mutants moved from `caught`/`survived` to `hang`.
  The glob properties compile a `GRegex` per pattern, which is nearly all of
  what they cost, so they try `PROP_GLOB_PATHS` paths per compile. The whole
  suite is ~0.3 s for ~1M assertions; keep it there.
- **A property phrased in terms of the function under test cannot see that
  function being wrong**, only being inconsistent. The escaping property asserts
  `!has_ctrl(out)` — and `has_ctrl` *is* `ctrl_seq_len`, which is also what the
  escaper calls, so a mutation inside the classifier leaves both sides agreeing.
  Measured: narrowing the C1 test from `>= 0x80` to `> 0x80` was caught by
  nothing there, while U+0080 reached the terminal unescaped. The property now
  also checks the raw bytes, spelled out rather than delegated. Reach for a
  second, independent phrasing wherever the obvious property is a tautology
  waiting to happen.
- **They are not a replacement for the tables, and the lift is uneven.**
  Measured, same 413-mutant sweep of `src/util.c` with the properties removed
  and restored: **213 survivors → 203**, and every one of the ten is a
  `survived → caught` in `sanitize_ctrl`'s buffer arithmetic — the truncation
  boundary, which needs a buffer that runs out at each possible offset and so
  is exactly what a hand-written table does not cover. The same A/B over
  `src/glob.c` (305 mutants) gained **nothing**: two mutants moved, both from
  `caught`/`survived` to `hang`, purely because the suite had got slower. On
  the named-bug files they earn their place differently again — leaving the
  fiemap address set unsorted is caught by nothing else. Expect that
  unevenness rather than a uniform number.

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

## Hash resume for very large files (#159)

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

## SIGINT/SIGTERM flush the batch (#201)

`src/interrupt.{c,h}`. The handler **only sets a flag** (SQLite and GLib are not
async-signal-safe); every loop that owns state polls it and unwinds through its
ordinary exit path, and `filescan_free()` → `scan_writer_close()` already
commits the batch on the way out. So nothing new is made durable — the loops
just have to *reach* it. Measured on a 3000-file tree, SIGINT at 0.5 s:
v1.10.1 persisted **0** files, this persists ~975, in ~70 ms.

- **The flag is a lock-free `_Atomic int`, not `volatile sig_atomic_t`.** The
  readers are on *other* threads (csum workers, walkers), and `volatile` orders
  nothing across threads — ThreadSanitizer flags it, and is right to. C11 lets a
  handler touch a lock-free atomic, which is what makes it both race-free and
  async-signal-safe; a `static_assert` on `ATOMIC_INT_LOCK_FREE` keeps that
  true. Relaxed on both sides: the flag carries no data.
- `sigaction` with **`SA_RESTART`** (a walker blocked in `read()` resumes, so no
  error path has to learn about EINTR) and **`SA_RESETHAND`** — the kernel puts
  the default action back on delivery, so the second Ctrl-C kills with no work
  in the handler. Verified by watching `SigCgt` in `/proc/PID/status` drop the
  SIGINT bit.
- **Four checkpoints, one per loop that would otherwise run to completion:** the
  walkers still call `dirq_finished()` for every directory they skip (leaving
  `walk_dir_pending` non-zero means `WALK_STOP` is never pushed and the consumer
  blocks forever); the consumer stops calling `__scan_file`; the csum workers
  *drop* queued files rather than hash them (the queue holds thousands — working
  through it is not a shutdown); `csum_whole_file` abandons the file it is on
  after writing an off-interval checkpoint, so a 1 TiB file costs one buffer of
  latency instead of hours.
- **The dedupe phase stops admitting batches and nothing else.** No new drain
  points: in-flight batches reap normally, `FIDEDUPERANGE` is atomic, and the
  generation-ordered watermark already guarantees `dedupe_seq` names only
  fully-processed generations.
- **An interrupt during the *scan* skips the dedupe phase outright**
  (`process_duplicates` returns right after the "Hashfile written" line), and
  the run does not `dbfile_maybe_vacuum()` on the way out. The batch-loop check
  above is too late: everything *before* it still ran — the deleted-file prune,
  the find-dupes index build, the ~9 s group analysis — and then a VACUUM that
  rewrites the whole file and cannot itself be interrupted. It deduped nothing
  (the loop broke at once), so the only effect was a Ctrl-C that looked ignored
  for minutes and then printed `Nothing to deduplicate`.
  - **Wiping the progress block is part of it.** The scan hands its block to
    the dedupe phase (`pscan_join(continues=true)`), so `dedupe_live` must also
    test `!interrupted()` — without it the early return strands the worker rows
    on screen for the rest of the session, which is #179 from the other side.
    Pinned by `test_progress_tty.py::…_an_interrupted_scan_leaves_nothing_behind`.
- **An interrupted run is not written to `run_history`** — it would read as a
  complete scan of the tree, skip counters and all. Exit is `128 + signo`, set
  last and only over a success.
- **Test hooks `DUPEREMOVE_INTERRUPT_AFTER=N` / `_AFTER_BATCHES=N` /
  `DUPEREMOVE_INTERRUPT_SIGNAL=TERM`** count files that have **finished
  hashing**, not files queued — so when the signal lands, N files are already in
  the open batch and the durability assertion is exact. Counting queued files
  let a loaded CI runner finish *none* of them, and the test asserted `> 0`
  against 0. Two more traps in that hook: it is ticked from the csum workers, so
  the counter is atomic and only the thread that crosses the limit raises
  (`==`, not `>=` — with `SA_RESETHAND` a second `raise()` kills outright), and
  the env is read once in `interrupt_install()` on the main thread, since a lazy
  first-tick read races between workers. They raise the *real* signal at a named
  point,
  the same way `DUPEREMOVE_CHECKPOINT_STOP` does for #159. A `sleep N; kill`
  race needs a tree too big for the suite (~500 MB/s of scan) and still
  coin-flips on faster storage; `test_signal_flush.py` keeps one real-signal
  test, which skips rather than fails if the scan won the race.

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

## File names are untrusted input (#202)

Anyone who can create a file inside a scanned tree picks bytes oans writes to
the admin's terminal, so **every path oans prints goes through
`sanitize_ctrl()`/`path_for_display()`** (`src/util.c`) — C0 controls, DEL and
the UTF-8 C1 controls become `\t`/`\n`/`\r`/`\xNN`. A `\r` in a name rewrites
the line above it (the summary, a progress row); `ESC[2J` clears the screen.

- **`make lint` enforces it — `scripts/lint-escape.py`.** The rule is otherwise
  prose, and prose rots: it was violated **eleven** times in the commit that
  introduced it, three found by review and eight more by the lint. It flags a
  path-shaped identifier passed to a printf-family macro; waive with
  `escape-ok: <why>` (the only legitimate reason so far is the hashfile path,
  which is oans's own argument rather than something found in a walk). Modelled
  on `lint-longpath.py`, and wired into the same `make lint` CI job.
- **Escape at the print site, never in the stored string.** The path is what
  oans opens, stats and stores; only the display copy is escaped. The idiom is
  `declare_display_path(disp, p);` then print `disp` — a *declaration* macro, in
  the shape of `declare_alloc_tracking()`. It cannot be a statement expression
  (`({ ... })` ends the cleanup scope, so the string is freed before `printf`
  reads it) and it cannot use a fixed thread-local buffer like `pretty_size()`
  does, because a path may exceed `PATH_MAX` (#117) and a truncated one names a
  different file.
- **On the hot path, escape inside the branch that prints, under the same guard
  the print macro has.** `check_file()` and `is_excluded()` run per directory
  entry, so their `-v` skip messages sit inside `if (verbose)` — `vprintf`'s own
  guard is too late, the allocation would already have happened. `probe_fs()`
  runs per root, so it escapes once at the top.
- **One classifier, many renderings.** `ctrl_seq_len()` is the single definition
  of *which* bytes are dangerous; `sanitize_ctrl()` spells them `\xNN` and
  `print_json_str()` spells them `\u00NN`. Adding U+2028 or another C1 encoding
  is then one edit, not two that can drift.
- Every real name takes the `has_ctrl()` fast path — one exact-sized `strdup`
  instead of a 4x over-allocation and a per-byte loop — and the progress row
  skips the copy entirely. Worst-case expansion is `SANITIZE_CTRL_MAX` (4) bytes
  per input byte; `sanitize_ctrl()` truncates at the last *complete* escape.
- Pinned by `tests/integration/test_escape_names.py` (non-tty, asserts no raw
  `\x1b`/`\r`/`\x07`/`\x7f` reaches either stream in any report mode) and
  `test_progress_tty.py::test_a_crafted_name_cannot_inject_ansi_into_the_block`.
  A test that only checks a plain name proves nothing — assert on the *bytes*,
  with `text=False`.

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

## What oans will scan: allowlist, then probe (#224)

btrfs and XFS are an **allowlist fast path**, not the definition of supported.
Anything else is asked directly whether it implements `FIDEDUPERANGE`, so a
filesystem that gains the ioctl needs no code change. `is_fs_allowlisted()`
names the two; `dedupe_probe_fd()` asks everything else.

- **The probe is a real block-sized request, and the answer is read from BOTH
  `rc`/`errno` and `info[0].status`.** A zero-length probe reading only errno
  looks obviously safer and is *wrong*: measured on a container's overlayfs
  over ext4, zero-length returns `rc=0` (overlayfs answers for itself, as a
  pass-through, without consulting the storage underneath) while a 4K request
  returns `rc=0, status=-EINVAL`. Plain ext4 returns `rc=-1/EOPNOTSUPP` for
  every shape. So errno-only accepts every containerised run on an overlay
  root, which then hashes the whole tree and fails at dedupe time — precisely
  what refusing unsupported filesystems upfront exists to prevent. Pinned by
  the `dedupe_classify_probe` unit test; reproduce with an `overlay` mount over
  ext4 and `./oans -rq`.
  The length comes from `dedupe_shareable_len()`/`cached_blocksize()`, the
  helpers the dedupe path already uses: `dest_offset` must be block-aligned or
  a filesystem that *does* support the ioctl answers `EINVAL`, which reads as
  UNKNOWN and quietly defeats the probe. Don't hardcode 4096 — 64K blocks exist
  and the codebase can query the real value.
- **The trade the real request buys that with:** on a filesystem that *does*
  support dedupe, the probe's two ranges may turn out identical and get shared.
  That is a genuine, byte-verified dedupe of one block within one file — it
  cannot change contents, size or mtime, and it happens only when the answer is
  yes, i.e. when oans is about to deduplicate the tree anyway.
  `test_fs_probe.py::…_leaves_the_file_alone` pins the file being untouched.
- **The taxonomy is the whole feature, so it is a pure function**
  (`dedupe_classify_probe`, unit-tested against every errno and status that
  matters). Only `EOPNOTSUPP`/`ENOTTY` — from either errno or a negative status
  — mean *no*; everything else, `EINVAL` above all (the man page documents it
  both for "the filesystem does not support deduplicating the ranges" and for a
  dozen per-file conditions), is **UNKNOWN**, and the next file is asked, up to
  `FS_PROBE_MAX_TRIES`. Never widen *no*: a wrong *no* silently skips someone's
  whole tree.
- **Probed lazily, at the first regular file, not at seed time.** The ioctl
  needs a writable regular file and a root is normally a directory — measured,
  a directory fd gives `EISDIR`, not `EOPNOTSUPP`. Creating a scratch file
  instead is a non-starter (read-only mounts; read-only subvolumes are scanned
  by default since #182). So `check_file()` locks the root provisionally and
  `__scan_file()` — the single consumer, hence no locking — settles it before
  anything is hashed. A definite no returns nonzero, which stops the consume
  loop — **and sets the walk-abort flag the walker threads poll**, mirroring
  `interrupted()`. Without that the walkers readdir/statx the whole tree after
  the error is already on screen: measured on a 20k-file tree, 402 `getdents64`
  became 16. An UNKNOWN file is hashed normally, so an unrecognised filesystem
  costs at most `FS_PROBE_MAX_TRIES` files before it is refused.
- **Three ways to end up refusing, and they say different things.** A definite
  no; `FS_PROBE_MAX_TRIES` files that all declined; and a walk that ended with
  the question never asked (`filescan_fs_probe_unsettled()` — an empty tree, or
  everything filtered by `--exclude` or size). The last one matters most: it is
  the case where exiting 0 would look like a clean run.
- **Not routed through `seed_fs_lock_failed`.** That reports roots which could
  never be locked at all and only fires when *none* was seeded; here a root was
  seeded and the probe has already said precisely what is wrong.
- **One place words the refusal** (`report_fs_unusable()`), because which of the
  three cases it is decides the advice, and `file_scan` owns the state — the
  same shape as `filescan_report_excludes()`. It also skips the report on an
  interrupted run, whose exit status belongs to the signal.
- **`DUPEREMOVE_FORCE_FS_PROBE=1` drops the fast path.** Without it the accept
  branch is unreachable in tests: the only filesystems known to answer *yes*
  are the two that never reach the probe. `test_fs_probe.py` uses it to run the
  probe against the real scratch btrfs/XFS, which is the only way this ships
  having been seen to say *yes* and not just *no*.
- The refusal paths are testable anywhere: ext4 and tmpfs answer `EOPNOTSUPP`,
  and an `overlay` mount over either reproduces the stacking case.

## Snapshot-aware scan: copy hashes for an identical layout (#206)

Hashing N snapshots of a subvolume read N× the data. Two files whose fiemaps
describe **the same records** (`fe_logical`, `fe_physical`, `fe_length`,
`fe_flags`, over the same size) are backed by the same stored bytes, so the
second one's digest, extent hashes and block hashes are **copied** instead of
computed. Measured on a 4-snapshot 7.5 GiB tree, cold: **2.36 s → 0.65 s**
(3.6×, against a ceiling of 4×). Flat on `realistic`/`many`/`big` — same
medians, same RSS.

- **Misses are free, false hits are catastrophic.** A miss costs one redundant
  hash; a wrong copy stores a digest of bytes the file never had, and nothing
  downstream could tell that from a file with no duplicate. Hence three
  independent bars, all of which must pass: `fiemap_layout_key()` refuses any
  record whose address it cannot vouch for (DATA_INLINE, UNKNOWN, DELALLOC,
  DATA_ENCRYPTED, or no extents at all), the key is a 128-bit digest of *every*
  record, and `dbfile_layout_matches()` then re-checks the donor's stored
  `(loff, poff, len)` triples one at a time.
  - **Never normalize or merge records to chase more hits.** The same storage
    described with different record boundaries (#186) must read as a *miss*.
  - SHARED and LAST are masked out of the key: the first changes when an
    unrelated file is deduped, the second is positional. Neither says anything
    about content. COMPRESSED/ENCODED is fine — this only ever compares
    `fe_physical` for equality, never does arithmetic on it.
- **The re-check needs no extra state: the donor's `extents` rows *are* its
  fiemap record array.** So the in-memory map holds only a key and a file id,
  and a donor whose rows vanished (aborted batch, hardlink cascade, only_whole_
  files) verifies as a miss by construction rather than needing a guard.
- **Donors are this run's files only.** Block size, `--skip-zeroes` and the rest
  of the hashing options are then the same by construction; an older hashfile's
  rows might have been produced under different ones.
- **No visibility hazard, because every scan write goes through the one shared
  `scan_writer` connection.** The re-check and the copy run on that connection
  inside the open batch, so a donor hashed seconds ago is visible before any
  commit. (The issue anticipated needing a fallback for this; a different
  connection *would* need one.)
- **`LAYOUT_COPY_MIN_SIZE = READ_BUF_LEN` (1 MiB), checked *before* the key is
  built.** The map is one entry per candidate donor, so on a multi-million-file
  tree it would be tens of MB of RSS for nothing (see #208); below one read
  buffer the file is a single I/O and the bookkeeping costs more than it saves.
  Gating after the key — as the first cut did — meant every small file, and
  every file at all under the kill switch, paid for a hash that could never
  match, since the size is part of the key.
- **The donor table is open-addressed with inline slots, like `seen_inodes`** —
  a `GHashTable` node plus a malloc per entry is ~50 B where the entry is 24,
  and there is one per large file. Same reason that set stopped using one.
- **Nothing gates this on `--hashfile`.** A run without one keeps its rows in an
  in-memory database that the copy reads and writes identically; measured, the
  dedupe outcome is the same with and without. Only `DUPEREMOVE_NO_LAYOUT_COPY`
  turns it off.
- **A hit needs the donor to have *finished*.** With N workers, a file and its
  snapshot are often hashed side by side and neither can donate — measured 74-82%
  hit rate on small trees, and it only improves with tree size. Fine in
  production; tests pin it with `--io-threads=1`.
- `DUPEREMOVE_NO_LAYOUT_COPY=1` is the kill switch. **The gate is byte-identical
  output against a run with it set** (`test_layout_copy.py`), not "the counter
  went up" — that is the only definition of correct here. Off automatically
  without `--hashfile`.

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
- **Partial mode drains — *before* `dedupe_begin_batch()`, not mid-load (#227).**
  `find_additional_dedupe()` walks the **global** filerec list, so with
  `--dedupe-options=partial` the producer calls `dedupe_drain()` — earlier
  batches reaped, filerecs freed — at the cost of cross-batch pipelining in that
  mode. Default mode keeps the full overlap; don't add other drain points.
  - **A reap may never land while a batch is open.** A batch takes its filerec
    refs in `push_results()`, so between a load and its push the open batch's
    groups point at filerecs kept alive only by the refs the reap drops — and a
    group spanning two windows is exactly that shape, since the anchor member
    (min id) is reloaded by every later window. The drain used to sit between
    the extent load and the push, which freed that anchor and made
    `push_results()` walk into it: a heap UAF that only showed up under
    `--dedupe-options=partial`, and only when the previous batch was still in
    flight. `free_batch()` now asserts `open_batch == NULL`, so putting a reap
    back in the middle of a load aborts at the violation instead of corrupting
    the heap.
  - `DUPEREMOVE_DEDUPE_DELAY_MS` holds each dedupe worker back so a batch is
    still in flight while the next one loads; on a test-sized tree the window
    never opens without it (`test_partial_cross_window.py`). Sibling of
    `DUPEREMOVE_SEARCH_DELAY_MS` below.
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
- **`fiemap_maps_share()` compares coverage, not extent records.** The
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
- **Routing honors the caller's stream (#203).** `print_above_block()` takes the
  `FILE *` and writes the message there, so an `eprintf` reaches stderr whether
  or not a block was up (it used to always land on stdout, and `2>errors.log`
  lost exactly the errors raised mid-run). The block itself is still stdout-only,
  so the erase must be `fflush`ed before the message and the message flushed
  before the redraw — otherwise the two streams' buffers interleave and the
  message lands inside the block. Don't drop those flushes.
  - Consequence for `--progress=json`: the JSON stream is **stderr**, so
    diagnostics share it. That was already true for anything printed outside the
    printer's lifetime; a consumer must skip lines that don't parse.

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
- **Never credit the kernel's `bytes_deduped` as space freed (#187).**
  `FIDEDUPERANGE` reports the whole compared length whether or not the range
  already shared the target's storage, so summing it counted work as savings: a
  destination half of which was already shared credited twice what it freed, and
  before #186 a run that freed *nothing* claimed 6.6 GiB. Each destination is
  measured before submission — `fiemap_unshared_bytes()` over the target's
  sorted physical addresses — and only that part is credited, capped by what the
  kernel got through and only for destinations it accepted. Approximate by at
  most one extent either way (a partial reference at a different offset into an
  uncompressed extent reads as unshared; any part of a compressed extent reads
  as shared), which is why it must never gate a *decision*, only the figure.
  Pinned by `test_reclaimed_excludes_what_was_already_shared` and the
  `test_fiemap_unshared_bytes` unit test.
- **Check a reported figure against `physical_footprint()`, not against
  sharing.** `tests/integration/test_convergence.py` asserts the two properties
  a summary cannot show — a second run frees nothing, and the reported figure
  equals the drop in distinct physical storage the tree references (summed from
  fiemap in the harness, independently of anything oans computes). #186 and
  #187 both shipped because the suite checked what oans *said* and which
  extents it *shared*, and never either of these. Its layout zoo lives in one
  tree on purpose: #186 only reproduced at that scale.
  - **A whole-file group is not a substitute for an extent group.** Whole-file
    groups load with `poff = 0` and skip `clean_deduped()` entirely, so only an
    extent-pass layout (same tail, different heads) reproduces #186 — a
    four-identical-file version of the same physical shape does not.
  - **The accounted-for set accumulates across a group's destinations (#191).**
    Seeded from the target, then grown by `fiemap_unshared_bytes()` as each
    destination is measured — because two destinations can already share an
    extent with *each other*, and crediting both in full claimed twice what
    releasing that one extent frees. Merged per destination, never inserted per
    address: the fragmented whole-file groups the sorted array exists for make
    the latter hopeless. Still counts one address repeated *within* a single
    destination twice; bounded, and noted at the definition.
