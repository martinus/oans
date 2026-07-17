# oans

> **A friendly fork of [markfasheh/duperemove](https://github.com/markfasheh/duperemove).**
> All of the original design and code is the work of **Mark Fasheh** and the
> upstream contributors (SUSE, Oracle, and many others). This fork adds a set of
> performance, correctness, and usability improvements on top of that
> foundation, and is renamed **oans** (Austrian dialect for "one") to avoid
> confusion with the upstream project. The improvements were developed with the
> help of AI tooling, and every change was reviewed, tested, and benchmarked on
> real btrfs data before landing. Huge thanks to the upstream authors — none of
> this exists without their work.
>
> The `oans` binary installs a `duperemove` compatibility symlink, so existing
> scripts and habits keep working.

oans is a simple tool for finding duplicated extents and submitting them for
deduplication. Given a list of files it hashes their contents on an extent
basis and compares those hashes, finding and categorizing extents that match.
With the `-d` option it submits matching extents to the kernel for
deduplication via the `FIDEDUPERANGE` ioctl — an atomic, byte-verified
operation, so a bug can only waste work or miss a dedupe, never corrupt your
data.

oans can store the hashes it computes in a **hashfile**. Given an existing
hashfile it only re-hashes files that changed since the last run, so you can
run it repeatedly on your data as it changes without re-checksumming
everything. The hashfile format is unchanged from upstream duperemove.

See [the upstream duperemove man page](http://markfasheh.github.io/duperemove/duperemove.html)
for the full reference (options, FAQ, and examples) — this fork keeps the same
command-line interface.

---

## What's different in this fork

Everything below is additive: the CLI, hashfile format, and behavior stay
compatible with upstream. The theme is **doing less redundant work** and
**telling you more clearly what happened**.

### Performance

- **Skip already-shared files.** Files whose extents are already shared are
  detected up front and skipped instead of being re-read and re-compared. This
  is the big win for the common "re-run periodically on a mostly-stable tree"
  workflow.
- **No cross-generation reprocessing.** The dedupe phase processes files in
  generation passes; a group whose copies span many passes used to be reloaded
  and re-checked in every pass. It now loads only what's new per pass plus one
  stable target, so large duplicate groups are handled once. This also fixes an
  inflated "Deduplicated" figure and keeps copies converging onto a single
  physical extent.
- **Batched hashfile transactions.** Per-file SQLite read/write transactions
  are batched on a ~10s cadence, collapsing a lock storm (hundreds of thousands
  of `F_SETLK` calls on a large rescan down to a few hundred).
- **Parallel directory walk.** The `opendir`/`readdir`/`statx` walk runs on a
  pool of walker threads (`--io-threads`), with a default cap tuned to where
  btrfs metadata contention plateaus.
- **Compact path-hash index.** Files are looked up in the hashfile by a 64-bit
  hash of their path (`csum_path`) rather than a full-path string index, for
  faster path lookups on large trees.

#### Measured

Real numbers, with honest caveats — your mileage depends heavily on your data
and filesystem.

- **Re-running on an already-deduped tree** (2.07M files, ~230 GiB on btrfs):
  **~92s vs ~11m for upstream 0.15.2** (~7× faster). This is the best case for
  the already-shared skip — a cold first run does the real work and the gap is
  smaller.
- **Eliminating cross-generation reprocessing** (same tree, our own
  before/after): **~294s → ~188s (~36%)**, with kernel dedupe traffic cut
  roughly in half and accurate dedupe accounting.
- **Batched read transactions**: ~24% faster rescans on a large hashfile.

### Correctness

- **Hardlink safety fix.** `INSERT OR REPLACE` on the inode key could
  cascade-delete rows for other hardlinks to the same inode and, in a bad
  batch, silently empty the hashfile while still exiting 0. Guarded, with a
  regression test.
- Fixes for a number of upstream issues surfaced along the way.

### Usability

- A **human-readable, colorful summary** (respecting `NO_COLOR` and non-TTY
  output) instead of a wall of per-extent text.
- A **live dedupe progress bar** with throughput rate and ETA.

### Testing

- A **Python `unittest` integration suite** (stdlib only) that drives the built
  binary against a scratch tree and asserts on the hashfile and on-disk
  sharing, plus **GitHub Actions CI**.

---

## A note on compressed filesystems

If your btrfs uses compression (e.g. zstd), read the `Deduplicated` figure as a
**logical/uncompressed** amount — the actual disk space you get back is
smaller, roughly the compression ratio times that number, because dedupe
operates on logical extents but frees compressed blocks. To see the true
reclaimed space, compare `compsize` **Disk Usage** before and after a run;
`Referenced` should stay constant (that's the proof nothing was lost).

---

## Requirements & building

- Linux kernel 3.13 or later
- GNU make, pkg-config
- glib2, sqlite3
- libxxhash 0.8.0 or later (`libxxhash-dev` on Debian)
- util-linux (libuuid, libmount, libblkid)
- libbsd (`libbsd-dev` on Debian)

```sh
make            # build oans + helpers
make check      # C unit tests + Python integration suite
sudo make install   # installs oans, a duperemove compat symlink, and helpers
```

Sources live under `src/`; man page sources under `docs/man/`; the integration
suite under `tests/`.

## Usage

oans takes a list of files and directories. A directory scans all regular files
within it; `-r` recurses. `-d` performs the deduplication (without it, oans
only reports what it would dedupe).

```sh
# Dedupe two files:
oans -dh file1 file2

# Add a directory (its files) to the set:
oans -dh file1 file2 dir1

# Recurse into dir1 as well:
oans -dhr file1 file2 dir1/

# Recursively dedupe a tree, reusing a hashfile across runs:
oans -dr --hashfile=/path/to/hashfile /my/data
```

A run ends with a summary like:

```
Summary
  Deduplicated   134.8 GiB across 164872 groups
  Kernel scanned 994.7 MiB
  Elapsed        92.1s
  Already shared 350708 files skipped (no work needed)
```

For the complete set of options and examples (including hashfile and `fdupes`
input), see the
[upstream man page](http://markfasheh.github.io/duperemove/duperemove.html).

## Credits & license

Original author: **Mark Fasheh** and the upstream duperemove contributors.
Upstream project: <https://github.com/markfasheh/duperemove>.

Licensed under the **GNU General Public License, version 2** — see
[`LICENSE`](LICENSE). This fork remains GPLv2, same as upstream.

## Links

- [Upstream duperemove](https://github.com/markfasheh/duperemove) — the original project
- [Upstream man page](http://markfasheh.github.io/duperemove/duperemove.html) — full reference
- [Upstream wiki](https://github.com/markfasheh/duperemove/wiki) — design & performance notes
