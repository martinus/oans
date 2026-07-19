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

oans finds files and extents with identical content and asks the kernel to
share their storage. It is a performance-focused fork of
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
| ⚡ **Fast where it counts** | Re-runs skip everything already hashed *and* everything already shared. Rescanning a deduped 2M-file / 230 GiB tree: **~92 s with oans vs ~11 min with duperemove 0.15.2** (~7×). |
| 🪄 **Zero-config re-runs** | The hashfile remembers your options, paths and excludes. After the first run, `oans --hashfile=FILE` — nothing else — replays it incrementally. |
| ⏰ **Scheduling built in** | `sudo make install-systemd`, then `systemctl enable --now oans@data.timer`. Weekly dedupe at idle I/O priority, no cron scripts. |
| 📊 **Statistics & history** | `--stats` shows what a hashfile holds and how much is reclaimable; `--history` shows space reclaimed over time; `--json` exports metrics for dashboards. |
| 🎯 **Honest reporting** | A clean, colorful live progress display (rate + ETA) and a summary that reports the disk space *actually freed* — not an inflated shared-extents figure. |
| 🔒 **Hardened** | Fixes a data-loss-adjacent upstream bug (hardlinks could silently empty the hashfile), read-only report modes that are safe while a dedupe runs, and an integration suite CI runs against a real btrfs. |
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
sudo dnf install gcc make pkgconf-pkg-config \
    glib2-devel sqlite-devel xxhash-devel \
    libuuid-devel libmount-devel libblkid-devel libbsd-devel

# Debian / Ubuntu
sudo apt install build-essential pkg-config \
    libglib2.0-dev libsqlite3-dev libxxhash-dev \
    uuid-dev libmount-dev libblkid-dev libbsd-dev
```

Requires Linux 3.13+ with a btrfs or XFS filesystem.

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
oans --stats   --hashfile=data.hash   # files, hashes, duplication, reclaimable space
oans --history --hashfile=data.hash   # reclaimed space per run + lifetime total
oans --json    --hashfile=data.hash   # machine-readable metrics for a dashboard
```

These are read-only and safe to run while a scan or dedupe is in progress.
So is Ctrl+C, by the way: hashes are committed every ~10 s, and a restart
resumes where it left off.

> [!TIP]
> On compressed btrfs (e.g. zstd), `Reclaimed` is a **logical** figure — the
> real disk space freed is smaller by roughly your compression ratio, because
> dedupe shares logical extents but disk holds compressed blocks. Compare
> `compsize` *Disk Usage* before/after for ground truth; *Referenced* staying
> constant is the proof nothing was lost.

## What the fork changes, measured

Benchmarked on real btrfs data (2.07M files, ~230 GiB); your mileage depends
on your data and filesystem. The exact tree, commands and comparison binary are
documented in the **[benchmark methodology](docs/benchmarks.md)**.

| Change | Effect |
|---|---|
| Skip already-shared files up front | Warm re-run **~11 min → ~92 s** vs upstream (best case; cold first runs gain less) |
| No cross-generation reprocessing of duplicate groups | Dedupe phase **~294 s → ~188 s (~36 %)**, kernel dedupe traffic halved, accurate accounting |
| Batched SQLite transactions (~10 s cadence) | **~24 % faster rescans**; hundreds of thousands of file-lock syscalls collapsed to a few hundred |
| Parallel directory walk (`--io-threads`) | Faster listing on large trees, capped where btrfs metadata contention plateaus |
| Compact 64-bit path-hash index | Smaller hashfile, faster path lookups |
| Skip post-dedupe extent measurement on interactive runs | An open + 2 `FIEMAP` ioctls saved per group member |

Full reference — every option, FAQ, examples: **[oans man page](docs/man/oans.md)** (`man 8 oans` once installed).

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
— see [What the fork changes](#what-the-fork-changes-measured) above, and the
attribution below.

## Relationship to duperemove

oans (Austrian dialect for *"one"* — as in one copy) builds on
[duperemove](https://github.com/markfasheh/duperemove); all of the original
design and code is the work of **Mark Fasheh** and the upstream contributors,
and none of this exists without them. The fork's improvements were developed
with the help of AI tooling; every change was reviewed, tested, and benchmarked
on real btrfs data before landing. By design the tool cannot put your data at
risk regardless: dedupe is performed by the kernel's `FIDEDUPERANGE` ioctl,
which byte-compares every range before sharing it (see the note near the top),
so the worst a bug can do is waste work or miss a dedupe.

The logo — a bauble full of rainbow *ones* — was designed and hand-drawn by my
8-year-old daughter.

Differences to know about:

- **Hashfiles are not interchangeable.** oans brands its hashfile with its own
  SQLite `application_id`; oans and duperemove will each rebuild rather than
  read the other's. Hashfiles are only caches, so nothing is lost.
- **CLI**: a few additions (`--stats`, `--history`, `--json`, `--autotune`,
  `--min-filesize`, `--no-color`, `-q`), a few legacy/testing options removed.
- Scripts that expect the `duperemove` binary keep working via the installed
  compatibility symlink, including the stable
  `net change in shared extents` output line.

## Links

- 🚀 [NAS quick-start](docs/nas-quickstart.md) — scheduled dedupe on a NAS/server, step by step
- 📖 [Man page](docs/man/oans.md) — full option reference, FAQ, examples
- ⏰ [systemd templates](systemd/README.md) — the `oans@` service/timer units
- 🧪 [Test suite](tests/) — Python integration tests (stdlib-only) run by [CI](.github/workflows/ci.yml)
- ⬆️ [Upstream duperemove](https://github.com/markfasheh/duperemove) and its [wiki](https://github.com/markfasheh/duperemove/wiki)

## License

[GPL-2.0](LICENSE), same as upstream.
