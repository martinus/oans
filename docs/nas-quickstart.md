# oans on a NAS — quick start

A practical, copy-pasteable path to running oans as a scheduled deduplication
job on a NAS or home server. It leans on the **self-describing hashfile**: a
run remembers its own options and paths, so everything after the first run
needs no arguments.

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

## Step 2 — The first run (the slow one)

Give the job a name — the hashfile the timer will later use is
`/var/cache/oans/<name>.hash`:

```sh
sudo install -d -m 0755 /var/cache/oans
sudo oans -dr --hashfile=/var/cache/oans/media.hash /srv/media
```

This is the expensive pass: it hashes everything and deduplicates. It

- records the options and paths, so later runs need no arguments,
- sizes its I/O threads from the detected backing storage (run with **-v** to
  see what it picked; pass `--io-threads=N` to override), and
- is safe to interrupt — the kernel does each dedupe atomically and
  byte-verified, so Ctrl+C can only waste work, never corrupt data.

Check the result:

```sh
oans --stats --hashfile=/var/cache/oans/media.hash
```

## Step 3 — Schedule it

```sh
sudo systemctl enable --now oans@media.timer
```

The name after `@` is the **basename of your hashfile** in `/var/cache/oans/`:
`oans@media` runs `--hashfile=/var/cache/oans/media.hash`. So match it to the
hashfile you created in Step 2 — if yours is
`/var/cache/oans/data.hash`, enable `oans@data.timer` instead. (The unit skips
cleanly until that hashfile exists, so set it up first.)

Done. Weekly from here, oans re-scans `/srv/media`, hashes only what changed,
and deduplicates — with no arguments, because the hashfile remembers everything.
To change the frequency, override `OnCalendar=`:

```sh
sudo systemctl edit oans@media.timer     # e.g. OnCalendar=daily
```

## Step 4 — Monitor

```sh
systemctl list-timers 'oans@*'                          # when it next runs
journalctl -u oans@media.service                        # what the last run did
oans --history --hashfile=/var/cache/oans/media.hash    # reclaimed over time
oans --json    --hashfile=/var/cache/oans/media.hash    # metrics for a dashboard
```

For **live** progress of a run in flight — to feed a dashboard or a health
check rather than watch the terminal — run it with `--progress=json`. oans then
streams one JSON object per phase (about once a second) to **stderr**, ending
with a `{"event":"done", ...}` line, and leaves stdout alone:

```sh
oans -qd --progress=json --hashfile=/var/cache/oans/media.hash \
    /srv/media 2>>/var/log/oans-media.jsonl
```

To capture that from the scheduled job, add `--progress=json` to the unit's
command (`sudo systemctl edit oans@.service`); the JSON lines then land in the
journal, where `journalctl -u oans@media.service -o cat` gives you a clean
stream to parse.

## Notes for NAS users

- **Run as root** so oans can read and re-extent every file in the tree.
- **Multiple datasets:** repeat Steps 2–3 with different names (`oans@photos`,
  `oans@backups`, …). Each is an independent timer you can schedule separately.
- **Report-only mode:** set the job up with `-r` instead of `-dr` and the
  scheduled runs will only refresh hashes and report, never change data.
- **Skipping snapshots and NAS metadata.** `--exclude` takes `.gitignore`-style
  patterns, so a bare name matches at any depth — no path juggling:

  ```sh
  sudo oans -dr --hashfile=/var/cache/oans/media.hash \
      --exclude '.snapshots' --exclude '@eaDir' /srv/media
  ```

  The patterns are stored with the rest of the run, so later scheduled runs
  reuse them. A pattern that matches nothing is reported as a warning.
- **The hashfile is just a cache** (under `/var/cache/oans`). Deleting it only
  forces the next run to re-hash from scratch; it can live on your system SSD,
  separate from the data pool.
- **Compressed btrfs:** the reported "Reclaimed" figure is *logical* — the
  real disk space freed is smaller (roughly the compression ratio times that).
  Compare `compsize` before and after for the true reclaimed amount.
- **First run is slow, later runs are fast:** only changed/new files are
  re-hashed, and files whose extents are already shared are skipped.
- **Querying while a run is in progress is safe.** The report commands
  (`--stats`, `--history`, `--json`, `-L`) open the hashfile read-only, so you
  can run them against a hashfile that a scheduled or manual run is actively
  using — they show a consistent point-in-time snapshot and can't disturb the
  run or the file. What you should *not* do is start a second *writing* run
  (another `oans -dr`, or `-R`) on the same hashfile at the same time; the
  single-writer design assumes one writer. Note the systemd timer won't start a
  second `oans@<name>.service` while one is active, but a manual `oans` run is
  invisible to that — so avoid kicking off a manual dedupe when the timer might
  fire on the same hashfile.
