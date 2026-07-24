# Benchmarks

The headline numbers in the [README](../README.md) come from the measurements
below. They are one person's results on one machine and two datasets; **your
mileage depends heavily on your data, filesystem and hardware.** Everything here
is reproducible — the point is that you can re-run it on your own tree rather
than trust a number.

There are two scenarios, because oans optimizes two different situations:

1. **[Warm re-runs on a large tree](#warm-re-runs-on-a-large-tree)** — the
   everyday workflow: re-scan an already-hashed, already-deduped tree and skip
   the work that is already done.
2. **[Larger-than-RAM dedupe](#larger-than-ram-dedupe)** — a first-run dedupe of
   a tree that does not fit in the page cache, where the kernel's dedupe re-read
   goes cold and oans prefetches it.

Both scenarios compare oans against upstream
[duperemove](https://github.com/markfasheh/duperemove) **0.15.2** (the fork's
base), on the same tree and hardware, with runs interleaved to cancel drift.

---

## Warm re-runs on a large tree

### Dataset

- A real btrfs tree: **~2.07M files, ~230 GiB**, mostly stable between runs
  (the shape oans is built for: a NAS / backup / build tree re-scanned
  regularly).
- Filesystem: **btrfs** on the test machine. (Never benchmark on `/tmp` — it is
  tmpfs, not reflink-capable, and a scan there stores zero files silently.)

### What "cold" vs "warm" means

- **Cold first run** — empty hashfile: every file is read and hashed, then
  deduped. This is dominated by btrfs metadata I/O and raw hashing; oans gains
  the least here. (For the cold-dedupe *phase* specifically, see the
  [larger-than-RAM benchmark](#larger-than-ram-dedupe) below.)
- **Warm re-run** — an existing hashfile from a previous run: only changed files
  are re-hashed, and files whose extents are already shared are skipped up
  front. This is the workflow oans optimizes, and where the large speedups show.

### Commands

```sh
# Cold: fresh hashfile
sudo oans -dr --hashfile=/var/cache/oans/bench.hash /srv/data

# Warm: re-run against the populated hashfile (no other args needed —
# oans replays the stored paths/options)
sudo oans --hashfile=/var/cache/oans/bench.hash
```

Compared against **duperemove 0.15.2** on the same tree and hardware, runs
interleaved to cancel drift (A/B/A/B), keeping each candidate's fastest run.

### Results

| Scenario | duperemove 0.15.2 | oans | Speedup |
|---|---|---|---|
| Warm re-run of an already-deduped tree | ~11 min | ~92 s | ~7× |
| Dedupe phase (no cross-generation reprocessing) | ~294 s | ~188 s | ~1.6× |
| Batched SQLite transactions (rescan) | baseline | ~24 % faster | — |

Cold first runs gain much less: the win is in *not repeating work* on re-runs,
not in the initial hash-everything pass.

---

## Larger-than-RAM dedupe

This scenario isolates the workload oans's dedupe phase is built for: a tree
**larger than the available page cache**. It is where the largest single win
lives.

### TL;DR

On a dataset that does not fit in the page cache, oans:

| Metric (median of 10) | oans | duperemove 0.15.2 | oans advantage |
|---|---:|---:|---:|
| **Hash + dedupe** (`-dr`) | **13.8 s** | 179.7 s | **~13× faster** |
| Hash only (`-r`, in-memory) | 7.8 s | 9.8 s | ~1.3× faster |
| Peak RSS (dedupe) | **121 MiB** | 243 MiB | ~2× lower |
| Peak RSS (hash) | **120 MiB** | 333 MiB | ~2.8× lower |
| Hashfile size | **39.7 MiB** | 70.9 MiB | ~1.8× smaller |

Both tools read the **same 16.7 GiB** and perform **byte-for-byte identical
dedupe**: after each run the tree is `Total 10.49 GiB / Exclusive 0.00 B /
Set-shared 4.75 GiB` (`btrfs filesystem du -s`) — the exact same physical
layout. The ~13× gap is purely *how* the dedupe phase reads the data back.

### Why limit RAM? (the whole point of this benchmark)

Deduplication goes through the kernel's `FIDEDUPERANGE` ioctl, which
**byte-compares the source range against every destination range before sharing
them**. That comparison reads the data the scanner just finished hashing.

- If that data is still in the **page cache**, the compare is a RAM read and is
  nearly free.
- If it has been **evicted**, the kernel re-reads it from disk — and btrfs'
  in-kernel dedupe read path does *not* issue readahead, so cold it crawls
  (~50 MiB/s in our measurements, vs GiB/s for a normal sequential read).

Whether the hashed data survives to the dedupe phase depends entirely on whether
the working set **fits in RAM**:

- **Fits in RAM** (small tree, big-RAM box): the data stays cached, the re-read
  is free, and there is nothing to optimize. This is the *easy* case — and the
  misleading one, because it hides the problem.
- **Larger than RAM** (the real target: a NAS / backup / build tree of hundreds
  of GiB on a box with modest RAM): the earliest-hashed files are evicted long
  before dedupe reaches them, so dedupe pays the cold re-read. **This is the case
  that matters in production, and the one oans optimizes.**

The development machine has **62 GiB of RAM**, so any tree we could reasonably
generate fits entirely in cache and the larger-than-RAM behaviour never appears —
you would need a >62 GiB dataset to see it naturally. Instead of building a
100+ GiB tree, we **cap the page cache available to the process** with a cgroup
v2 memory limit set *below the dataset size*. That faithfully and cheaply
reproduces the larger-than-RAM regime: the process can cache at most `cap` bytes,
so a dataset bigger than `cap` is partially evicted during the run exactly as it
would be on a RAM-constrained server. Swap is disabled for the run
(`MemorySwapMax=0`) so eviction is real, not merely pushed to swap.

Concretely: dataset ≈ **10.5 GiB**, cap = **4 GiB**. During the dedupe phase the
tree cannot stay resident, so the just-hashed data is evicted and must be
re-read — the production scenario, on a dataset small enough to run 10× in a few
minutes.

### How oans handles it

Two changes (merged in [#107](https://github.com/martinus/oans/pull/107)) address
the cold re-read:

1. **Keep hashed data warm** — the scanner no longer drops each file from the
   page cache (`POSIX_FADV_DONTNEED`) when a dedupe will follow. This alone fixes
   the case where the working set *fits* in RAM.
2. **Prefetch each dedupe round** — right before every `FIDEDUPERANGE` call, oans
   reads that round's source and destination ranges (≤ 32 MiB each) back into the
   page cache with a plain sequential read, so the kernel's compare hits RAM
   instead of the cold path. This covers the larger-than-RAM case, where #1 can't
   keep everything warm. It is chunked to the dedupe round, so a file far larger
   than RAM is warmed a piece at a time.

Upstream duperemove has neither, so under memory pressure its dedupe phase takes
the cold in-kernel read path for the evicted data. The dedupe phase also now runs
a **streaming pipeline** (a persistent worker pool with a double-buffered
producer) that keeps peak RSS low and flat regardless of hashfile size.

### Setup and parameters

| | |
|---|---|
| **oans** | `master` @ `c20c50a` (`v1.3.0-26-gc20c`: byte-weighted dedupe progress + streaming pool) |
| **duperemove** | `v0.15.2-2-g897a222` (built from upstream, the fork's base) |
| **CPU** | AMD Ryzen 9 7950X (16 cores / 32 threads) |
| **RAM** | 62 GiB |
| **Storage** | Corsair MP400 NVMe, **btrfs** |
| **OS** | Fedora Linux 44 |
| **Kernel** | 7.1.3-200.fc44.x86_64 (cgroup v2) |
| **Dataset** | two non-reflinked copies of a Linux kernel git tree: **189,546 files, ~10.5 GiB** (~8.4 GiB unique data hashed), fully duplicated (copy2 == copy1) |
| **Memory cap** | **4 GiB** per run, via `systemd-run --user --scope -p MemoryMax=4G -p MemorySwapMax=0` |
| **Rounds** | 10 per configuration |

Methodology:

- **Cold caches:** `sync; echo 3 > /proc/sys/vm/drop_caches` before every timed
  run, so each run reads the tree from disk.
- **Interleaved:** the two binaries alternate in a randomized order each round, so
  drift and thermal effects hit both roughly equally.
- **Non-reflinked copies:** the tree is built with `cp --reflink=never`, so the
  two copies are independent physical data and dedupe has real work (it reclaims
  copy2 against copy1).
- **Between dedupe runs** the shared state is undone by removing and re-copying
  `copy2` (`rm copy2 && sync && cp --reflink=never … && sync`); `copy1` stays put
  as the dedupe target, so every run starts from the same fully-unshared tree
  with an empty hashfile.
- **Timing** via `/usr/bin/time`: `%e` wall, `%U` user, `%S` sys, `%M` peak RSS,
  `%I` filesystem inputs (disk reads).

The two phases:

- **Phase 1 — hash only:** `<bin> -rq <tree>` (in-memory, no hashfile, no
  dedupe). The tree is never modified, so only `drop_caches` is needed between
  runs. This is a control: both tools read and hash the same ~8.4 GiB.
- **Phase 2 — hash + dedupe:** `<bin> -dr --hashfile=<hf> <tree>`. This is the
  real workload; the dedupe re-read is where the larger-than-RAM penalty lands.

### Results

#### Phase 1 — hash only (in-memory), cap 4 GiB, 10 rounds

| binary | wall median | wall avg | min | max | user | sys | peak RSS | disk read |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| **oans** | **7.83 s** | 7.95 | 7.78 | 8.68 | 3.37 | 10.95 | **120 MiB** | 8.4 GiB |
| duperemove | 9.83 s | 9.87 | 9.76 | 10.27 | 5.93 | 12.64 | 333 MiB | 8.4 GiB |

Both read and hash the same ~8.4 GiB. oans is ~1.3× faster on the wall (the cold
read is I/O-bound, so the gap is modest here) at ~2.8× lower peak RSS and roughly
half the user CPU (3.4 s vs 5.9 s) — the parallel walk and lower per-file
overhead show up as CPU headroom.

#### Phase 2 — hash + dedupe, cap 4 GiB, 10 rounds

| binary | wall median | wall avg | min | max | user | sys | peak RSS | disk read | hashfile |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| **oans** | **13.84 s** | 16.54 | 13.56 | 26.99 | 5.17 | 24.76 | **121 MiB** | 16.7 GiB | **39.7 MiB** |
| duperemove | 179.66 s | 184.43 | 169.85 | 213.95 | 13.29 | 47.84 | 243 MiB | 16.7 GiB | 70.9 MiB |

oans's dedupe is **~13× faster** (median 13.8 s vs 179.7 s). Both read the same
16.7 GiB (~8.4 GiB to hash + ~8.3 GiB re-read for the dedupe compare) and reach
the identical shared-extent layout — the difference is that oans prefetches the
re-read as fast sequential I/O while duperemove takes the kernel's slow cold path.

Notes:

- oans's median (13.8 s) is clean; its **average** (16.5 s) is pulled up by 2 of
  10 rounds spiking to ~27 s — background btrfs/NVMe activity hitting the cold
  hash read, not a property of the tool. duperemove is steady at ~170–214 s.
- oans's dedupe peak RSS (**121 MiB**) is now *lower* than its own hash-phase
  figure and ~2× below duperemove's, thanks to the streaming pipeline — it no
  longer grows with the number of duplicate groups in flight.
- oans writes a **39.7 MiB** hashfile vs duperemove's **70.9 MiB** (~1.8×
  smaller), from oans's compact 64-bit path-hash index.

### Verifying identical dedupe

Both tools reclaim the same space and leave the tree in the same physical state.
After deduping a freshly-unshared copy with each:

```
$ btrfs filesystem du -s tree      # after oans
  10.49GiB       0.00B     4.75GiB  tree
$ btrfs filesystem du -s tree      # after duperemove
  10.49GiB       0.00B     4.75GiB  tree
```

Identical `Exclusive` (0 B — nothing unique left in copy2) and `Set-shared`
(4.75 GiB). oans reports `Reclaimed 5.1 GiB across 45626 groups`.

### Reproduce it

The whole benchmark above is one command — [`scripts/bench-dedupe.py`](../scripts/bench-dedupe.py):

```sh
scripts/bench-dedupe.py --baseline build:897a222 --source ~/git/linux \
    --copies 2 --cap 4G --rounds 10 --verify
```

`build:897a222` builds the fork's pure-upstream base (duperemove 0.15.2) in a
cached worktree; pass `--baseline /path/to/duperemove` to use a binary you
already have, or omit `--baseline` to benchmark oans alone. `--verify` runs the
byte-identical-sharing check (`btrfs filesystem du -s`). The dataset is a
stand-in — use any `--source` tree, as long as its `--copies` total is larger
than `--cap`.

Under the hood, for each round and interleaving the binaries, it:

1. builds `--copies` non-reflinked copies of the source (dedupe reclaims the
   later copies against `copy0`);
2. restores the unshared copies (`rm` + `cp --reflink=never`) and removes the
   hashfile before each dedupe run;
3. drops the page cache (`sync; echo 3 > /proc/sys/vm/drop_caches`) and runs each
   binary under a capped scope, recording `/usr/bin/time` metrics:
   ```sh
   systemd-run --user --scope -p MemoryMax=4G -p MemorySwapMax=0 \
     /usr/bin/time -f '%e %U %S %M %I' \
     <bin> -dr --hashfile=/path/bench.hash /path/tree
   ```
4. reports the median across rounds (robust to the occasional cold-cache spike).

## Caveats worth stating up front

- **`Reclaimed` is a logical figure.** On compressed btrfs (e.g. zstd) the real
  disk freed is smaller by roughly your compression ratio, because dedupe shares
  logical extents while disk holds compressed blocks. For ground truth compare
  `compsize` **Disk Usage** before/after; `Referenced` staying constant proves
  nothing was lost.
- **Walker parallelism plateaus.** On btrfs, scaling `--io-threads` past ~8 gives
  no wall-clock gain — it is metadata b-tree lock contention, not I/O. Use
  `--autotune` to pick the fastest count for your disks.
- **One machine, one filesystem.** A fast NVMe here — on a slow HDD (a typical
  NAS) the cold dedupe re-read is far more expensive, so the larger-than-RAM gap
  would be *larger*, not smaller. The cgroup cap models RAM pressure; a real
  larger-than-RAM tree on a smaller-RAM box behaves the same way but is more
  expensive to run repeatedly.
- **Numbers are not portable across machines.** As always: **re-run it on your
  own data** before quoting them.
