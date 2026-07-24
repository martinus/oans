# Benchmark: dedupe on a dataset larger than RAM (oans vs duperemove)

This benchmark isolates the workload oans's dedupe phase is built for: a tree
**larger than the available page cache**, deduplicated against an upstream
[duperemove](https://github.com/markfasheh/duperemove). It is a companion to the
general **[benchmark methodology](benchmarks.md)**; everything here is
reproducible with the harness described at the bottom.

## TL;DR

On a dataset that does not fit in the page cache, oans:

| Metric (median of 10) | oans | duperemove 0.15.2 | oans advantage |
|---|---:|---:|---:|
| **Hash + dedupe** (`-dr`) | **14.0 s** | 155.0 s | **~11× faster** |
| Hash only (`-r`, in-memory) | 6.0 s | 9.3 s | ~1.6× faster |
| Peak RSS (dedupe) | 162 MiB | 254 MiB | ~1.6× lower |
| Peak RSS (hash) | 117 MiB | 337 MiB | ~2.9× lower |
| Hashfile size | 41 MiB | 73 MiB | ~1.8× smaller |

Both tools read the **same 17.9 GiB** and perform **byte-for-byte identical
dedupe** (same extents shared) — the ~11× gap is purely *how* the dedupe phase
reads the data back.

## Why limit RAM? (the whole point of this benchmark)

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
would be on a RAM-constrained server. Swap is disabled so eviction is real, not
merely pushed to swap.

Concretely: dataset ≈ **11 GiB**, cap = **4 GiB**. During the dedupe phase the
tree cannot stay resident, so the just-hashed data is evicted and must be
re-read — the production scenario, on a dataset small enough to run 10× in a few
minutes.

## How oans handles it

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
the cold in-kernel read path for the evicted data.

## Setup and parameters

| | |
|---|---|
| **oans** | `master` @ `4d1c10a` (with #107) |
| **duperemove** | `v0.15.2-2-g897a` (built from upstream, the fork's base) |
| **CPU** | AMD Ryzen 9 7950X (16 cores / 32 threads) |
| **RAM** | 62 GiB |
| **Storage** | Corsair MP400 NVMe, **btrfs** |
| **Kernel** | 7.1.3 (cgroup v2) |
| **Dataset** | two non-reflinked copies of a Linux kernel git tree: **189,546 files, ~11 GiB**, fully duplicated (copy2 == copy1) |
| **Memory cap** | **4 GiB** per run, via `systemd-run --user --scope -p MemoryMax=4G -p MemorySwapMax=0` |
| **Rounds** | 10 per configuration |

Methodology (matching the general methodology doc):

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
  runs. This is a control: both tools read and hash the same 9.1 GiB.
- **Phase 2 — hash + dedupe:** `<bin> -dr --hashfile=<hf> <tree>`. This is the
  real workload; the dedupe re-read is where the larger-than-RAM penalty lands.

## Results

### Phase 1 — hash only (in-memory), cap 4 GiB, 10 rounds

| binary | wall median | wall avg | min | max | user | sys | peak RSS | disk read |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| **oans** | **5.97 s** | 5.98 | 5.95 | 6.02 | 3.65 | 10.94 | **117 MiB** | 9.1 GiB |
| duperemove | 9.31 s | 9.34 | 9.20 | 9.70 | 6.08 | 12.59 | 337 MiB | 9.1 GiB |

Both read and hash the same 9.1 GiB. oans is ~1.6× faster (parallel directory
walk + lower per-file overhead) at ~2.9× lower peak RSS.

### Phase 2 — hash + dedupe, cap 4 GiB, 10 rounds

| binary | wall median | wall avg | min | max | user | sys | peak RSS | disk read | hashfile |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| **oans** | **14.01 s** | 18.73 | 13.94 | 36.37 | 4.81 | 24.58 | **162 MiB** | 17.9 GiB | **41 MiB** |
| duperemove | 155.03 s | 157.71 | 153.35 | 165.80 | 13.45 | 47.22 | 254 MiB | 17.9 GiB | 73 MiB |

oans's dedupe is **~11× faster** (median 14.0 s vs 155.0 s). Both read the same
17.9 GiB (9.1 GiB to hash + ~8.8 GiB re-read for the dedupe compare) and share
the same extents — the difference is that oans prefetches the re-read as fast
sequential I/O while duperemove takes the kernel's slow cold path.

Notes:

- oans's median (14.0 s) is clean; its **average** (18.7 s) is pulled up by 3 of
  10 rounds spiking to 26–36 s — background btrfs/NVMe activity hitting the cold
  hash read, not a property of the tool. duperemove is steady at ~155 s.
- oans writes a **41 MiB** hashfile vs duperemove's **73 MiB** (~1.8× smaller),
  from oans's compact 64-bit path-hash index.
- Peak RSS is lower for oans in both phases.

## Reproduce it

The dataset is a stand-in — use any tree you like, as long as the total data is
larger than the cap. The harness:

1. Build the tree with two non-reflinked copies of a source directory.
2. For each round, interleaving the two binaries:
   - restore the unshared tree (`rm copy2 && sync && cp --reflink=never … && sync`)
     and remove the hashfile;
   - `sync; echo 3 > /proc/sys/vm/drop_caches`;
   - run under a capped scope and record `/usr/bin/time` metrics:
     ```sh
     systemd-run --user --scope -p MemoryMax=4G -p MemorySwapMax=0 \
       /usr/bin/time -f '%e %U %S %M %I' \
       <bin> -dr --hashfile=/path/bench.hash /path/tree
     ```
3. Report the median across rounds (robust to the occasional cold-cache spike).

**Caveats.** One machine, one dataset, one filesystem; a fast NVMe here — on a
slow HDD (a typical NAS) the cold re-read is far more expensive, so the gap would
be *larger*, not smaller. The cgroup cap models RAM pressure; a real
larger-than-RAM tree on a smaller-RAM box behaves the same way but is more
expensive to run repeatedly. As always: **re-run it on your own data.**
