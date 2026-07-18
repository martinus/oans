# oans on a NAS — quick start

A practical, copy-pasteable path to running oans as a scheduled deduplication
job on a NAS or home server. It uses two features that make this painless: the
**self-describing hashfile** (a run remembers its own options and paths) and
**`--autotune`** (measures the fastest thread count for your disks).

Throughout, replace `/srv/media` with your data directory and `media` with a
short name for the job.

## Step 0 — Check your filesystem (the dealbreaker)

Deduplication only works on **btrfs** or **xfs**:

```sh
findmnt -no FSTYPE /srv/media      # or: stat -f -c %T /srv/media
```

- **btrfs / xfs** → you're good. (Most Synology volumes are btrfs.)
- **zfs** → stop. oans cannot dedupe ZFS; use ZFS's own deduplication instead.
  (This rules out a stock TrueNAS SCALE pool.)
- **ext4 / other** → oans will scan and report, but cannot deduplicate.

## Step 1 — Build and install

There is no binary package yet, so build from source. Install the build
dependencies first:

```sh
# Fedora / RHEL
sudo dnf install gcc make pkgconf-pkg-config \
    glib2-devel sqlite-devel xxhash-devel \
    libuuid-devel libmount-devel libblkid-devel libbsd-devel
```

```sh
# Debian / Ubuntu
sudo apt install build-essential pkg-config \
    libglib2.0-dev libsqlite3-dev libxxhash-dev \
    uuid-dev libmount-dev libblkid-dev libbsd-dev
```

Then build and install:

```sh
make
sudo make install            # installs the oans binary (+ duperemove symlink)
sudo make install-systemd    # installs the oans@ timer/service templates
```

## Step 2 — Autotune the thread count for your disks

Give the job a name and tune into the hashfile the timer will later use
(`/var/cache/oans/<name>.hash`), so the measured value is reused by every run:

```sh
sudo install -d -m 0755 /var/cache/oans
sudo oans --autotune --hashfile=/var/cache/oans/media.hash /srv/media
```

Run it **as root** — autotune drops the page cache between trials to get honest
cold-read numbers, which matters a lot on spinning disks and RAID. It reads only
a bounded sample (quick, not a full scan), prints a throughput-vs-threads table,
and stores the winning `--io-threads` value in the hashfile. This is the
reliable way to size threads for a NAS; without it, oans falls back to a
storage-type heuristic whose HDD/RAID numbers are only educated guesses.

## Step 3 — The first run (the slow one)

```sh
sudo oans -dr --hashfile=/var/cache/oans/media.hash /srv/media
```

This is the expensive pass: it hashes everything and deduplicates. It

- reuses the thread count autotune stored in Step 2,
- **records its options and paths** in the hashfile (this is what makes the
  scheduled runs need no arguments), and
- is safe to interrupt — the kernel does each dedupe atomically and
  byte-verified, so Ctrl+C can only waste work, never corrupt data.

Add `-v` once if you want to see the detected storage and the chosen thread
count. Check the result:

```sh
oans --stats --hashfile=/var/cache/oans/media.hash
```

## Step 4 — Schedule it

```sh
sudo systemctl enable --now oans@media.timer
```

Done. Weekly from here, oans re-scans `/srv/media`, hashes only what changed,
and deduplicates — with no arguments, because the hashfile remembers everything.
To change the frequency, override `OnCalendar=`:

```sh
sudo systemctl edit oans@media.timer     # e.g. OnCalendar=daily
```

## Step 5 — Monitor

```sh
systemctl list-timers 'oans@*'                          # when it next runs
journalctl -u oans@media.service                        # what the last run did
oans --history --hashfile=/var/cache/oans/media.hash    # reclaimed over time
oans --json    --hashfile=/var/cache/oans/media.hash    # metrics for a dashboard
```

## Notes for NAS users

- **Run as root** so oans can read and re-extent every file in the tree.
- **Multiple datasets:** repeat Steps 2–4 with different names (`oans@photos`,
  `oans@backups`, …). Each is an independent timer you can schedule separately.
- **Report-only mode:** set the job up with `-r` instead of `-dr` and the
  scheduled runs will only refresh hashes and report, never change data.
- **The hashfile is just a cache** (under `/var/cache/oans`). Deleting it only
  forces the next run to re-hash from scratch; it can live on your system SSD,
  separate from the data pool.
- **Compressed btrfs:** the reported "Deduplicated" figure is *logical* — the
  real disk space freed is smaller (roughly the compression ratio times that).
  Compare `compsize` before and after for the true reclaimed amount.
- **First run is slow, later runs are fast:** only changed/new files are
  re-hashed, and files whose extents are already shared are skipped.
