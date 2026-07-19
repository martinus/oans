# Benchmark methodology

The headline numbers in the [README](../README.md) come from the measurements
below. They are one person's results on one real dataset; **your mileage depends
heavily on your data, filesystem and hardware.** Everything here is reproducible
— the point is that you can re-run it on your own tree rather than trust a
number.

## Dataset

- A real btrfs tree: **~2.07M files, ~230 GiB**, mostly stable between runs
  (the shape oans is built for: a NAS / backup / build tree re-scanned
  regularly).
- Filesystem: **btrfs** on the test machine. (Never benchmark on `/tmp` — it is
  tmpfs, not reflink-capable, and a scan there stores zero files silently.)

## What "cold" vs "warm" means

- **Cold first run** — empty hashfile: every file is read and hashed, then
  deduped. This is dominated by btrfs metadata I/O and raw hashing; oans gains
  the least here.
- **Warm re-run** — an existing hashfile from a previous run: only changed files
  are re-hashed, and files whose extents are already shared are skipped up
  front. This is the workflow oans optimizes, and where the large speedups show.

## Commands

```sh
# Cold: fresh hashfile
sudo oans -dr --hashfile=/var/cache/oans/bench.hash /srv/data

# Warm: re-run against the populated hashfile (no other args needed —
# oans replays the stored paths/options)
sudo oans --hashfile=/var/cache/oans/bench.hash
```

Compared against **duperemove 0.15.2** on the same tree and hardware, runs
interleaved to cancel drift (A/B/A/B), keeping each candidate's fastest run.

## Headline results

| Scenario | duperemove 0.15.2 | oans | Speedup |
|---|---|---|---|
| Warm re-run of an already-deduped tree | ~11 min | ~92 s | ~7× |
| Dedupe phase (no cross-generation reprocessing) | ~294 s | ~188 s | ~1.6× |
| Batched SQLite transactions (rescan) | baseline | ~24 % faster | — |

Cold first runs gain much less: the win is in *not repeating work* on re-runs,
not in the initial hash-everything pass.

## Caveats worth stating up front

- **`Reclaimed` is a logical figure.** On compressed btrfs (e.g. zstd) the real
  disk freed is smaller by roughly your compression ratio, because dedupe shares
  logical extents while disk holds compressed blocks. For ground truth compare
  `compsize` **Disk Usage** before/after; `Referenced` staying constant proves
  nothing was lost.
- **Walker parallelism plateaus.** On btrfs, scaling `--io-threads` past ~8 gives
  no wall-clock gain — it is metadata b-tree lock contention, not I/O. Use
  `--autotune` to pick the fastest count for your disks.
- **Numbers are not portable across machines.** Reproduce on your own tree
  before quoting them.
