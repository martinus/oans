# oans

**Fast, safe, set-and-forget deduplication for btrfs and XFS.**

[![CI](https://github.com/martinus/oans/actions/workflows/ci.yml/badge.svg)](https://github.com/martinus/oans/actions/workflows/ci.yml)
[![License: GPL v2](https://img.shields.io/badge/License-GPL_v2-blue.svg)](LICENSE)

<!-- assets/demo.gif was generated with:
     DEMO_UNIQUE=9765 DEMO_DUP_GROUPS=273 DEMO_COPIES=4 DEMO_MEAN_KB=1234 DEMO_LOSSY=80 \
       scripts/demo/record.sh -->
<p align="center">
  <img src="assets/demo.gif" width="900"
       alt="oans scanning a large tree, deduplicating it, and reporting the space reclaimed">
</p>

<img src="assets/logo.png" align="left" width="180"
     alt="The oans logo, designed and drawn by my 8-year-old daughter">

oans (Austrian dialect for "one", logo by my daughter) finds files and extents
with identical content and asks the kernel to share their storage. It is a
performance-focused fork of
[duperemove](https://github.com/markfasheh/duperemove) by Mark Fasheh — same
proven engine, rebuilt for the workflow that actually matters: **re-running
regularly on a big, mostly-stable tree** (a NAS, a backup target, a build
server) and getting out of the way.

```console
$ sudo oans -dr --hashfile=/var/cache/oans/data.hash /srv/data
...
Summary
  Reclaimed      133.1 GiB across 164872 groups
  Elapsed        92.1s
  Already shared 350708 files skipped (no work needed)
```

> [!NOTE]
> Deduplication happens through the kernel's `FIDEDUPERANGE` ioctl, which
> **byte-compares every range before sharing it**. A bug can waste work or miss
> a dedupe — it cannot corrupt your data.

## Why oans?

| Feature | What you get |
|---|---|
| ⚡ **Fast where it counts** | Re-runs skip everything already hashed *and* already shared — rescanning a deduped 2M-file / 230 GiB tree takes **~92 s vs ~11 min for duperemove 0.15.2** (~7×). And when the tree is **larger than RAM** (the usual NAS/backup case), even a first-run dedupe is **~11× faster** (14 s vs 155 s): oans prefetches the data the kernel re-reads cold — [benchmark](docs/benchmarks-low-memory.md). |
| 🪄 **Zero-config re-runs** | The hashfile remembers your options, paths and excludes. After the first run, `oans --hashfile=FILE` — nothing else — replays it incrementally. |
| ⏰ **Scheduling built in** | `sudo make install-systemd`, then `systemctl enable --now oans@data.timer`. Weekly dedupe at idle I/O priority, no cron scripts. |
| 📊 **Observability** | `--stats` shows what a hashfile holds and how much is duplicated; `--history` shows space actually reclaimed over time; `--json` exports metrics for dashboards; `--progress=json` streams live per-phase progress for monitoring scheduled runs. |
| 🎯 **Honest reporting** | A clean, colorful live progress display (rate + ETA) and a summary that reports the disk space *actually freed* — not an inflated shared-extents figure. |
| 🔒 **Hardened** | Fixes a data-loss-adjacent upstream bug (hardlinks could silently empty the hashfile), read-only report modes that are safe while a dedupe runs, and an integration suite CI runs against a real btrfs and XFS. |
| 🤝 **Drop-in compatible** | The CLI is a close superset of duperemove's, and `make install` provides a `duperemove` compatibility symlink. |

## Quick start

> **Prefer a prebuilt binary?** Grab the latest
> `oans-*-linux-x86_64.tar.gz` from the
> [releases page](https://github.com/martinus/oans/releases) — no build step;
> the tarball's `INSTALL.txt` lists the handful of runtime libraries. Otherwise,
> build from source:

<details>
<summary><b>Install build dependencies</b> (Fedora, Debian/Ubuntu)</summary>

```sh
# Fedora / RHEL
sudo dnf install gcc make libatomic pkgconf-pkg-config \
    glib2-devel sqlite-devel xxhash-devel \
    libuuid-devel libmount-devel libblkid-devel libbsd-devel

# Debian / Ubuntu
sudo apt install build-essential pkg-config \
    libglib2.0-dev libsqlite3-dev libxxhash-dev \
    uuid-dev libmount-dev libblkid-dev libbsd-dev
```

Requires Linux 3.13+ with a btrfs or XFS filesystem. On XFS, filesystem
identification is unprivileged on Linux 6.4+; on older kernels run oans as root
(a scheduled root job is unaffected).

</details>

```sh
make && sudo make install
```

> **Arch Linux:** `PKGBUILD`s live in [`packaging/aur/`](packaging/aur) (`oans`
> for releases, `oans-git` for `master`).

```sh
# 1. First run: hash the tree and deduplicate it (-r recurse, -d dedupe)
sudo oans -dr --hashfile=/var/cache/oans/data.hash /srv/data

# 2. Every run after that: only changed files are re-hashed,
#    already-shared files are skipped. No arguments needed.
sudo oans --hashfile=/var/cache/oans/data.hash

# 3. Optional: make it automatic (weekly, idle-priority)
sudo make install-systemd
sudo systemctl enable --now oans@data.timer
```

📖 **Setting this up on a NAS or home server?** Follow the
**[NAS quick-start guide](docs/nas-quickstart.md)** — a complete walkthrough
from first scan to scheduled, monitored dedupe (including `--autotune` to pick
the fastest I/O settings for your disks).

## Watching it work

```sh
oans --stats   --hashfile=data.hash   # files, hashes, logical duplication (freed if deduped)
oans --history --hashfile=data.hash   # reclaimed space per run + lifetime total
oans --json    --hashfile=data.hash   # machine-readable metrics for a dashboard
```

These are read-only and safe to run while a scan or dedupe is in progress.
So is Ctrl+C, by the way: hashes are committed every ~10 s, and a restart
resumes where it left off.

For **live** progress of a running scan/dedupe — feeding a dashboard or a status
check instead of the interactive display — add `--progress=json`. It streams one
JSON object per phase (about once a second) to stderr, ending with a `done`
event, and leaves stdout untouched:

```sh
oans -qd --progress=json --hashfile=data.hash /srv/data 2>progress.jsonl
```

> [!TIP]
> On compressed btrfs (e.g. zstd), `Reclaimed` is a **logical** figure — the
> real disk space freed is smaller by roughly your compression ratio, because
> dedupe shares logical extents but disk holds compressed blocks. Compare
> `compsize` *Disk Usage* before/after for ground truth; *Referenced* staying
> constant is the proof nothing was lost.

## What the fork changes

Same engine as [duperemove](https://github.com/markfasheh/duperemove), retuned
end-to-end for **re-running regularly on a big, mostly-stable tree**. The short
version: warm re-runs skip everything already hashed *and* already shared, the
dedupe phase stops reprocessing groups across passes and stays fast even when the
tree is larger than RAM, an upstream data-loss-adjacent hashfile bug is fixed, and
you get real observability (`--stats`, `--history`, `--json`, `--progress=json`)
plus set-and-forget scheduling.

The headline speedups, benchmarked on real btrfs data (2.07M files, ~230 GiB);
your mileage depends on your data and filesystem, and the exact tree, commands
and comparison binary are documented in the
**[benchmark methodology](docs/benchmarks.md)**:

| Change | Effect |
|---|---|
| Skip already-shared files up front | Warm re-run **~11 min → ~92 s** vs upstream (best case; cold first runs gain less) |
| No cross-generation reprocessing of duplicate groups | Dedupe phase **~294 s → ~188 s (~36 %)**, kernel dedupe traffic halved, accurate accounting |
| Batched SQLite transactions (~10 s cadence) | **~24 % faster rescans**; hundreds of thousands of file-lock syscalls collapsed to a few hundred |
| Parallel directory walk (`--io-threads`) | Faster listing on large trees, capped where btrfs metadata contention plateaus |
| Compact 64-bit path-hash index | Smaller hashfile (**41 vs 73 MiB** on the benchmark tree), faster path lookups |
| Skip post-dedupe extent measurement on interactive runs | An open + 2 `FIEMAP` ioctls saved per group member |

<details>
<summary><b>Everything the fork changes</b>, grouped — correctness, performance, observability, UI, packaging</summary>

#### 🔒 Correctness & hardening

- **Hardlink hashfile-emptying hazard fixed.** `INSERT OR REPLACE` on
  `UNIQUE(ino, subvol)` could cascade-delete rows for other links to an inode and
  silently empty the hashfile while exiting 0; an in-memory `seen_inodes` guard
  and a regression test pin it.
- **Use-after-free & leak fixed** when a rejected hashfile is recreated (a closed
  handle was handed back to the caller); found by running the whole suite under
  valgrind, which is now a CI job.
- **Dedupe robustness:** stop spinning when a round makes no progress
  (upstream #396/#407), clamp the source range to file size to avoid `EINVAL`,
  fix the infinite loop on NoCOW files (#376), and skip members whose size
  changed since the scan.
- **Sparse-file fixes:** handle files with a trailing hole (#374); hash the
  actual block, not a stale zero check.
- **Uninitialised-memory fix** in UUID config load, with a committed valgrind
  suppressions file for the one library false-positive.
- **In-memory (no `--hashfile`) “database table is locked” fixed** (shared-cache
  read/write connection split).
- **Robust output:** 32-bit-correct counters, sanitized filenames in the status
  line, no `-nan%` bar.
- **`busy_timeout`** so transient lock contention retries instead of failing.
- **Report modes open the hashfile read-only** — `--stats`/`--history`/`--json`
  are safe to run while a scan or dedupe is in progress.
- **Refuse unsupported-fs roots up front** instead of silently storing 0 files;
  hint at permissions when a hashfile can't be opened; report a version even when
  built outside a git checkout (#387).

#### ⚡ Performance & memory

- **Skip already-shared files up front** (#331) — the headline warm-rescan win.
- **No cross-generation reprocessing** of duplicate groups that span passes
  (halves kernel dedupe traffic, fixes accounting).
- **Fast dedupe on trees larger than RAM:** keep just-hashed data in the page
  cache for the dedupe phase and prefetch each `FIDEDUPERANGE` round, so the
  kernel byte-compares from RAM instead of a slow cold re-read — **~11× faster**
  than upstream under memory pressure ([benchmark](docs/benchmarks-low-memory.md)).
- **Batched SQLite transactions** (~10 s cadence) for both the change-detection
  reads and writes — hundreds of thousands of per-file lock syscalls collapse to
  a few hundred (~24 % faster rescans).
- **Parallel directory walk** (`--io-threads`), capped where btrfs metadata
  contention plateaus (~8 walkers).
- **Compact 64-bit path-hash index** instead of the full-path index.
- **Largest-files-first (LPT) hashing** to shorten the idle tail.
- **fiemap streamlining:** ranged queries in the dedupe phase, one shared core,
  a single ioctl for the rescan extent, and a resume cursor that kills an
  O(n²) rescan.
- **Skip all-hole blocks** in sparse files; grow the scan's hash arrays
  geometrically, not one element at a time.
- **Fewer redundant lookups:** cache per-device fs verification and btrfs
  subvolume lookups; build find-dupes indexes after the scan; drop indexes that
  duplicate a UNIQUE prefix.
- **Lower peak RSS:** SQLite cache 256 MB → 64 MB per connection, per-role cache
  budgets, smaller read buffers, and an open-addressing `seen_inodes` set instead
  of a `GHashTable`.

#### 📊 Observability

- **`--stats`** — a full hashfile report: file/hash counts, logical duplication
  ratio, file-size summary, free space a VACUUM would reclaim, and the top
  duplicate groups (size × copies, with an example path).
- **`--history`** — a per-run timeline of space actually reclaimed, plus lifetime
  totals, from an appended `run_history` table.
- **`--json`** — a flat metrics object for jq / Telegraf / dashboards.
- **`--progress=json`** — one JSON object per phase (~1/s) to stderr, ending with
  a `done` event, for monitoring scheduled runs; stdout untouched.
- **Scan diagnostics** (`DUPEREMOVE_SCAN_STATS`) — queue starvation and
  write-lock contention counters.

#### 🎯 UI & output

- **Unified live progress UI** across scanning / hashing / dedupe / done — one
  stable, colorful status line with rate, percentage, runtime and a weighted-ETA,
  a `mapping:` pre-read phase, and a summary block.
- **Honest reporting:** the summary reports disk space *actually freed*
  (one physical copy kept per group), not an inflated shared-extents figure.
- **Progress polish:** ellipsize the path middle so the numbers stay visible,
  always-human sizes, no bar pinned at 100 %, no live-block drift, files-examined
  shown during listing, quieter per-file errors, and “Skipping dedupe” no longer
  collides with the status bar.
- **Self-contained `--help`/usage** instead of shelling out to `man`.

#### ⏰ Operations, packaging & compatibility

- **Self-describing hashfile:** each run stores its options, roots and excludes,
  so a bare `oans --hashfile=FILE` replays the last run incrementally — the basis
  for zero-config scheduling.
- **systemd `oans@` service/timer templates** (`make install-systemd`) for
  weekly, idle-priority dedupe, with a **[NAS quick-start guide](docs/nas-quickstart.md)**.
- **`--autotune`** empirically picks the fastest `--io-threads` for the backing
  storage (and a device-type heuristic sizes the default for HDD/RAID pools).
- **`--min-filesize`**, **`--no-color`** and **`-q`** added; the legacy
  `--fdupes` mode and other dead/testing options removed.
- **Automatic housekeeping:** prune deleted files from the hashfile after a scan
  (stat-based), then VACUUM when a build or prune left it worth it.
- **Branded hashfile** (SQLite `application_id`, format 5.0) so oans and
  duperemove never misread each other's caches.
- **Drop-in compatibility:** `make install` adds a `duperemove` symlink and keeps
  the stable `net change in shared extents` output line.
- **Packaging & release:** prebuilt x86_64 tarball attached to each GitHub
  release (`scripts/release.sh`), AUR `PKGBUILD`s, and a hand-drawn logo +
  scripted demo GIF.
- **Build & CI hardening:** `src/` layout, `-Wextra`/hardened flags, reproducible
  builds, and CI running the unit + integration suites (and a valgrind pass)
  against real btrfs *and* XFS.

</details>

Full reference — every option, FAQ, examples: **[oans man page](docs/man/oans.md)** (`man 8 oans` once installed).

> [!NOTE]
> **On a dataset larger than RAM** — the normal case for a NAS / backup / build
> tree — oans deduplicates **~11× faster than upstream duperemove** (median
> **14 s vs 155 s** on an ~11 GiB tree with the page cache capped to 4 GiB),
> hashes ~1.6× faster, uses less than half the peak RSS, and writes a ~1.8×
> smaller hashfile — doing byte-for-byte identical dedupe. This is where the
> dedupe-phase design pays off: when the working set doesn't fit in cache, the
> kernel's `FIDEDUPERANGE` re-read is slow cold, and oans prefetches it while
> upstream doesn't. Full methodology, parameters and per-round tables:
> **[larger-than-RAM benchmark](docs/benchmarks-low-memory.md)**.

## How it compares

**vs. [bees](https://github.com/Zygo/bees):** bees is an always-on daemon doing
continuous, block-level dedupe across the *whole* filesystem. oans is the other
trade-off — an *offline, batch* tool you point at specific trees and run on a
schedule (or a systemd timer): no resident daemon, no constant background I/O,
plus per-run stats, history and honest accounting. Pick bees for always-on
whole-fs dedupe; pick oans for scheduled, observable, targeted runs.

**vs. ZFS deduplication:** ZFS dedupes inline and keeps a large dedupe table
permanently in RAM (on the order of 1–5 GiB per TiB of data), and it's hard to
undo. oans needs no dedupe table and no permanent RAM cost — you run it when you
want, on the btrfs/XFS you already have.

**vs. upstream duperemove:** the same engine, tuned for *re-running regularly*
and for trees **larger than RAM** — where its dedupe phase measures
[~11× faster](docs/benchmarks-low-memory.md). See
[What the fork changes](#what-the-fork-changes) above, and the
attribution below.

## Relationship to duperemove

oans builds on
[duperemove](https://github.com/markfasheh/duperemove); all of the original
design and code is the work of **Mark Fasheh** and the upstream contributors,
and none of this exists without them. The fork's improvements were developed
with the help of AI tooling; every change was reviewed, tested, and benchmarked
on real btrfs data before landing. By design the tool cannot put your data at
risk regardless: dedupe is performed by the kernel's `FIDEDUPERANGE` ioctl,
which byte-compares every range before sharing it (see the note near the top),
so the worst a bug can do is waste work or miss a dedupe.

Differences to know about:

- **Hashfiles are not interchangeable.** oans brands its hashfile with its own
  SQLite `application_id`; oans and duperemove will each rebuild rather than
  read the other's. Hashfiles are only caches, so nothing is lost.
- **CLI**: a few additions (`--stats`, `--history`, `--json`, `--progress=json`,
  `--autotune`, `--min-filesize`, `--no-color`, `-q`), a few legacy/testing
  options removed.
- Scripts that expect the `duperemove` binary keep working via the installed
  compatibility symlink, including the stable
  `net change in shared extents` output line.

## Links

- 🚀 [NAS quick-start](docs/nas-quickstart.md) — scheduled dedupe on a NAS/server, step by step
- 📖 [Man page](docs/man/oans.md) — full option reference, FAQ, examples
- 📊 [Benchmark methodology](docs/benchmarks.md) and [larger-than-RAM benchmark](docs/benchmarks-low-memory.md) — how the numbers were measured
- ⏰ [systemd templates](systemd/README.md) — the `oans@` service/timer units
- 🧪 [Test suite](tests/) — Python integration tests (stdlib-only) run by [CI](.github/workflows/ci.yml)
- ⬆️ [Upstream duperemove](https://github.com/markfasheh/duperemove) and its [wiki](https://github.com/markfasheh/duperemove/wiki)

## License

[GPL-2.0](LICENSE), same as upstream.
