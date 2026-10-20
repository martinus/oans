---
paths:
  - "scripts/bench*.py"
  - "scripts/perf-profile.sh"
  - "docs/benchmarks.md"
---

# Profiling & measurement — read before optimizing

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
  - **SQL has its own harness: `scripts/bench-queries.py`** — the second
    sanctioned exception, because neither tool above can isolate one query.
    It builds large synthetic hashfiles (profiles `blocks`, `fragmented`,
    `snapshots`, `manyfiles`, cached in `--workdir`), takes each dedupe-phase
    query from `src/dbfile.c` of `--base` (default `origin/master`) and of the
    working tree through `cc -E`, and prints time and temp writes for both,
    plus whether the results are the same (exit 1 if not). Run it for any change
    to a `GET_DUPLICATE_*` or `COUNT_*` statement: `scripts/bench-queries.py -q
    count-extents -p fragmented`. The temp-write column is the one that found
    #260, #265 and #270; on a fast disk the time alone hides it.
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
