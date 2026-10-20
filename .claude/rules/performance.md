---
paths:
  - "scripts/bench*.py"
  - "scripts/perf-profile.sh"
  - "docs/benchmarks.md"
---

# Profiling & measurement

```sh
# perf self/caller views, perf stat, syscall summary
scripts/perf-profile.sh --cold -- -dr --hashfile=/tmp/prof.db ~/git
scripts/bench.py -p mixed --bin base=/tmp/oans-master --bin new=./oans  # A/B
scripts/bench.py -p git --walk-threads 4,8,16,32           # thread sweep
scripts/bench.py -p git --variant a: --variant b:DUPEREMOVE_FOO=1  # env
# canonical dedupe A/B
scripts/bench-dedupe.py --baseline build:897a222 --source ~/git/linux \
    --cap 4G --rounds 10 --verify
scripts/bench-queries.py -q count-extents -p fragmented    # one SQL query
```

- **`scripts/bench.py` is the scan harness; never add a `bench-*.sh`** (it
  replaced `bench-scan.sh`, `bench-scan-cold.sh`, `bench-ram.sh`;
  `scripts/demo/*` is the GIF). Matrix of binaries × io-threads ×
  walk-threads × env-variants on trees from `scripts/demo/gen.py`,
  interleaved rounds; `median` is the headline, `--rss` adds peak RSS. Needs
  btrfs/xfs (checks reflink); scans `-rq`.
  - Profiles are `PROFILES` keys: `realistic` (default regression check),
    `mixed` (bandwidth-bound hashing), `bigfile` (idle tail; `--io-threads 2`),
    `many` (~250k tiny; find_dupes pool + walk), `big` (read buffers;
    `--rss`), `fragmented` (8 × 512 MiB at ~32k extents; `--warm`,
    `--io-threads 2`), `git` (`--external`, default `~/git`). ~10 s cold.
  - **Only `fragmented` has many extents per file** (real trees ~1), so only
    it sees per-file extent-list cost, like the O(extents²) `get_extent` scan
    (49% of CPU until #134). `_fragment` must `sync` before rewriting, or no
    CoW happens and it yields ~6k extents, not ~32k.
  - Cold is default (`--warm` opts out) via passwordless
    `sudo tee /proc/sys/vm/drop_caches`; `sudo -n true` still needs a
    password.
- **`scripts/bench-dedupe.py`** is the one dedupe exception: `bench.py` only
  scans and dedupe mutates the tree, costly only larger-than-RAM. It times
  hash and hash+dedupe of N `--reflink=never` copies cold under a
  `MemoryMax`-capped `systemd-run --user --scope`, restores them between
  rounds, `--verify` checks `btrfs filesystem du -s`, reuses `bench.py`'s
  `drop_caches`/reflink helpers, and produces `docs/benchmarks.md`'s
  larger-than-RAM numbers. `build:897a222` (pure-upstream base) is the only
  valid baseline; `../duperemove`, `../dm-backports` carry fork commits.
- **`scripts/bench-queries.py`** (second exception) runs each dedupe-phase
  query of `src/dbfile.c` at `--base` (default `origin/master`) and in the
  working tree (via `cc -E`) on cached synthetic hashfiles (`blocks`,
  `fragmented`, `snapshots`, `manyfiles`; `--workdir`); exit 1 if results
  differ. Run it for any `GET_DUPLICATE_*`/`COUNT_*` change and read the
  temp-write column: it exposed #260, #265, #270, which time hides on a fast
  disk.
- **Never trust `strace -c`** (showed `statx` 66%, really ~7%). Use
  `perf record -g --call-graph dwarf` for where time goes,
  `perf stat -e task-clock` for A/B wall-clock.
- A/B distinctly-named binaries, confirm they differ (a stale object once hid
  a ~24% win), interleave runs.
- A lone result contradicting prior profiling is probably a measurement bug;
  reconcile before acting.
