---
title: oans
section: 8
header: System Manager’s Manual
footer: oans 1.4.0
date: July 2026
---

# NAME

`oans` — find duplicate data in files and share it with the kernel's
deduplication ioctl.

# SYNOPSIS

| **oans** \[*options*] *file*...
| **oans** **-d** \[**-r**] **\--hashfile**=*FILE* *file*...
| **oans** **\--hashfile**=*FILE*
| **oans** {**\--stats** | **\--history** | **\--json** | **-L**} **\--hashfile**=*FILE*
| **oans** **-R** **\--hashfile**=*FILE* *file*...
| **oans** **\--autotune** *file*...

# DESCRIPTION

`oans` finds regions of identical data across a set of files and, on request,
asks the kernel to make them share storage — reclaiming the duplicated space
without changing what any file contains. It works on **btrfs** and **xfs**, the
Linux filesystems that support reflink/dedupe.

Deduplication is performed with the kernel's **`FIDEDUPERANGE`** ioctl, which
**byte-for-byte compares** every candidate range and locks the files while it
does so. Because the kernel — not `oans` — moves the data, a bug in `oans` can
only waste work or miss a dedupe; it can never corrupt your files.

Given a list of files and directories, `oans` reads them, hashes their contents
on an extent basis, and groups extents whose hashes match. Without **-d** it
only *reports* what it would dedupe (a dry run); with **-d** it submits the
matches to the kernel and prints how much space was reclaimed.

`oans` scales to large trees by keeping its hashes in a **hashfile** (see
**\--hashfile**), an SQLite database it updates incrementally: on a re-run it
re-hashes only the files whose size or mtime changed, and skips extents that are
already shared. The hashfile is **self-describing** — it records the options,
paths, and excludes of each run — so a scheduled job need only name it (see
**\--hashfile** and the *EXAMPLES*).

`oans` is a fork of `duperemove`; `make install` also installs a `duperemove`
symlink so existing scripts keep working. Its hashfiles are branded and are not
interchangeable with upstream duperemove's (each rebuilds its own; they are only
caches, so nothing is lost).

# MODES OF OPERATION

**Read-only (default).**
Without **-d**, `oans` reads, hashes, and prints the duplicate extents it found
— useful for previewing what a dedupe run would do. Nothing on disk is changed.
It does not concern itself with how an extent is stored (compressed, already
shared, mid-I/O); in dedupe mode the kernel handles those details.

**Deduplicating (-d).**
The same read-hash-compare step, but matching extents are submitted to the
kernel for sharing. Extents that are already shared are detected up front and
skipped, so repeated runs over a mostly-stable tree are cheap. A summary of the
space reclaimed is printed at the end (see *OUTPUT*).

**Report and maintenance modes.**
**\--stats**, **\--history**, **\--json**, **-L**, **-R**, and **\--autotune**
each inspect or maintain a hashfile and then exit without scanning for dupes.
The first four open the hashfile **read-only**; see *NOTES* for running them
alongside an active `oans`.

# OPTIONS

The non-option arguments are the *files* and *directories* to scan, or a single
hyphen (**-**) to read the list from standard input, one path per line. Naming a
directory scans the regular files directly inside it; add **-r** to recurse.

## Operation

**-d**
  ~ Deduplicate: submit matching extents to the kernel (**btrfs**/**xfs** only).
    Without **-d**, `oans` performs a read-only dry run and only reports.

**-r**
  ~ Recurse into subdirectories of any directory argument.

<!-- Keep the blank line between this term and its `~` body below: without it
     pandoc >=3.9 collapses this multi-paragraph definition into a single block. -->
**\--hashfile**=*FILE*

  ~ Store hashes in *FILE* (an SQLite database) instead of memory. This is the
    recommended way to run `oans` on anything larger than a handful of files: it
    keeps the memory footprint modest and makes runs **incremental** and
    **resumable**.

    If *FILE* does not exist it is created. If it exists, `oans` compares the
    stored size/mtime of each recorded path and re-hashes only what changed.
    Paths that no longer exist on disk are pruned automatically (by existence,
    so files merely outside the paths scanned this run are kept); the database
    is compacted once enough of it is free.

    New files are added to the hashfile only when they are reachable from the
    *file* arguments, so normally you pass the same paths and **-r** each run.
    Discovery is efficient and visits each file once, even ones already stored.

    Because the hashfile also records the run's options, paths, and **\--exclude**
    patterns, you need not repeat them: **`oans --hashfile=FILE`** with no *file*
    arguments **replays the last run** (any other options on that command line
    are ignored). If none of the stored paths still exist `oans` refuses rather
    than prune every entry — guarding against, e.g., an unmounted drive — while
    individually missing paths are skipped with a warning.

    When deduping from a hashfile, files that have not changed since they were
    last deduped are skipped.

## Scan tuning

**-b** *SIZE*
  ~ Block size used when reading and hashing file data. Default **128K**;
    accepted range **4K**–**1M** (suffixes `K`/`M` accepted). Smaller blocks can
    surface more sub-extent matches when partial matching is enabled
    (**\--dedupe-options=partial**) but cost more CPU and a larger hashfile;
    larger blocks fragment less. Most users never need to change this.

**-B** *N*, **\--batchsize**=*N*
  ~ Run the dedupe phase after every *N* newly scanned files instead of once at
    the end. This caps memory use on large data sets and when partial matching
    is enabled, at a small cost to multithreading efficiency. Default **1024**,
    a good value for extent-based dedupe; drop it toward **1** when working on
    very large files (VM images, backups).

**-m** *SIZE*, **\--min-filesize**=*SIZE*
  ~ Skip regular files smaller than *SIZE* bytes (suffixes `K`/`M`/`G`
    accepted). Trees full of tiny files scan much faster this way, since such
    files rarely dedupe usefully. Default **1**, which skips only empty files.

**\--skip-zeroes**
  ~ Detect and skip all-zero blocks while reading. Speeds up scanning of sparse
    or zero-filled data, at the cost of not deduplicating runs of zeroes.

**\--exclude**=*PATTERN*
  ~ Exclude matching files and directories from the scan — handy for skipping
    snapshot mounts or caches. A *PATTERN* without a leading `/` is matched
    relative to the current directory. Because the shell also expands globs,
    quote the pattern, e.g. `oans --exclude "/srv/media/tmp/*" /srv/media`. May
    be given multiple times.

**\--dedupe-options**=*opt*\[,*opt*...]
  ~ Comma-separated switches that alter *what* gets deduplicated. Prefix an
    option with **no** to turn it off.

    **[no]same**
      ~ Allow deduplicating extents that live within the *same* file. Default
        **on**.

    **[no]partial**
      ~ Also compare *portions* of extents (block-level matching), finding
        dedupe that pure extent matching misses. Powerful but CPU-intensive and
        larger on disk; pair it with **\--batchsize** to bound memory. Default
        **off**. (This path is under active development and its semantics may
        change.)

    **[no]only_whole_files**
      ~ Restrict dedupe to entire, byte-identical files, disabling both extent-
        and block-level matching. The hashfile is smaller and some steps are
        faster, but less duplication is found. Default **off**.

## Threads

**\--io-threads**=*N*
  ~ Number of threads for the I/O-bound stages (directory walk, file hashing,
    dedupe). By default `oans` inspects the scan target's backing storage and
    chooses automatically: the host CPU count capped at **8** for
    non-rotational disks (SSD/NVMe) or unknown media; fewer for a single
    spinning disk (seek-bound); and roughly two per device for a multi-device
    btrfs pool, still capped at 8. Run with **-v** to see the detected storage
    and the value chosen. A stored **\--autotune** result, if present, is used
    in preference to the heuristic. Passing *N* explicitly disables all of this
    and uses *N* verbatim.

**\--cpu-threads**=*N*
  ~ Number of threads for the CPU-bound duplicate-extent-finding stage. Default
    is the host CPU count capped at **8**; an explicit *N* overrides the cap.

## Reporting and maintenance (each exits after running)

**\--stats**
  ~ Print a summary of the hashfile and exit: its format and identity, the
    stored scan configuration a bare replay would use, how many files and hashes
    it holds, the total logical data tracked, and the whole-file duplication it
    records (duplicate groups and their logical duplicate volume). That volume is
    a logical figure — it is already shared on disk if you have run a dedupe, so
    check **\--history** for bytes actually freed and `compsize`(8) for on-disk
    usage. Requires **\--hashfile**.

**\--history**
  ~ Print the run history recorded in the hashfile and exit: the number of runs,
    the total space reclaimed over their lifetime, and a timeline of recent runs
    (date, reclaimed bytes, elapsed time, files hashed, and whether each was a
    dedupe or a scan). One row is appended automatically per run. Requires
    **\--hashfile**.

**\--json**
  ~ Print the hashfile's current metrics as a flat JSON object — files, hashes,
    logical bytes, duplicate groups, reclaimable bytes, plus lifetime
    run-history totals — then exit. Intended for scripting and dashboards (pipe
    to `jq`, Telegraf, a node_exporter textfile). `reclaimable_logical_bytes` is
    a logical upper bound; real disk freed is smaller on a compressed
    filesystem. Requires **\--hashfile**.

**-L**
  ~ List every file tracked in the hashfile and exit; **-v** adds per-file
    detail. Requires **\--hashfile**.

**-R** *file*...
  ~ Remove the named paths from the hashfile and exit; a single **-** reads the
    list from standard input. Requires **\--hashfile**. (Deleted files are also
    pruned automatically on the next scan; **-R** is for removing paths that
    still exist.)

**\--autotune**
  ~ Measure the fastest **\--io-threads** for this machine and exit. `oans`
    reads and hashes a bounded sample of the given tree at several thread
    counts, dropping the page cache between trials (run as **root** for
    meaningful cold-read numbers on spinning disks), and prints a throughput
    table with the winner. Directories are always sampled recursively — the
    sample is about the storage, not the **-r** flag. With **\--hashfile** the
    winning value *and* the scan configuration are stored, so `--autotune`
    doubles as one-time setup: a later bare `oans --hashfile=FILE` replays it,
    using the tuned thread count unless you pass an explicit **\--io-threads**.
    This is the hardware-measured counterpart to the storage heuristic and the
    reliable way to tune a NAS or disk array. Sample bounds are controlled by
    the `DUPEREMOVE_AUTOTUNE_*` variables (see *ENVIRONMENT*).

## Output and information

**-q**, **\--quiet**
  ~ Quiet: print only errors and a one-line dedupe summary. Suppresses the live
    progress display and the human summary block.

**-v**
  ~ Verbose: restore the per-group dedupe listing that is hidden by default in
    favor of the progress bar, and show the detected storage / chosen thread
    count.

**\--progress=json**

  ~ Stream machine-readable progress instead of the interactive display, for
    monitoring scheduled or non-interactive runs. One newline-delimited JSON
    object is written to **standard error** per phase (and about once a second),
    ending with a `{"event":"done", ...}` line; standard output is unchanged (so
    the `net change in shared extents` line and the human summary still appear
    there). Works whether or not standard output is a terminal.

    Each line is a self-contained object. During scanning:
    `{"phase":"scanning","elapsed_sec":2.1,"files_examined":48120,"files_to_hash":12340}`;
    during hashing: `"phase":"hashing"` with `files`, `files_total`, `bytes`,
    `bytes_total`, and, once measurable, `bytes_per_sec` and `eta_sec`; during
    dedupe: `"phase":"dedupe"` with `groups`, `groups_total`, `work_done_bytes`,
    `work_total_bytes`, `reclaimed_bytes`, the current `activity`, and `eta_sec`.
    `work_total_bytes` is the total kernel byte-verify volume the phase will do
    (computed exactly up front) and `work_done_bytes` counts up to it; these
    drive the smooth dedupe progress bar and are emitted raw (never clamped
    backwards), so `work_done_bytes` is non-decreasing and reaches
    `work_total_bytes` on the last dedupe line. The final line is
    `{"event":"done","elapsed_sec":...,"files_scanned":...,"groups_deduped":...,"reclaimed_bytes":...}`.
    Fields that are not yet known (an ETA, a rate) are omitted; a consumer
    reading a line at a time can ignore any line that is not valid JSON.

    Combine with **-q** to silence the human stdout too, e.g.
    `oans -qd --progress=json --hashfile=FILE /srv/data 2>progress.jsonl`.

**\--no-color**
  ~ Disable colored output. Color is also disabled automatically when standard
    output is not a terminal or when the `NO_COLOR` environment variable is set.

**\--debug**
  ~ Print debug messages; implies **-v**.

**\--version**
  ~ Print the `oans` version and exit.

**-h**, **\--help**
  ~ Print a short usage summary and exit. (For the full reference, read this
    manual page: **man 8 oans**.)

# OUTPUT

A dedupe run ends with a summary such as:

```
Summary
  Reclaimed      133.1 GiB across 164872 groups
  Elapsed        92.1s
  Already shared 350708 files skipped (no work needed)
```

**Reclaimed** is the disk space actually freed: for a group of *N* identical
copies, `oans` keeps one physical copy and frees the other *N*−1, so this is the
honest bytes-freed figure, not an inflated count. **Already shared** counts
files skipped because their storage was already shared. If some destinations
could not be deduped (their data changed since the scan, or the kernel refused),
a **Not deduped** line reports the counts; re-run with **-v** for detail.

When standard output is not a terminal, or under **-q**, `oans` also prints a
stable machine-readable line:

```
Comparison of extent info shows a net change in shared extents of: N
```

This *N* is a lower-level fiemap diagnostic — the change in bytes the filesystem
reports as shared — and counts the surviving copy as shared too, so for pairs it
is about twice **Reclaimed**. Scripts that parsed this line from upstream
duperemove continue to work.

On a **compressed** btrfs, **Reclaimed** is a *logical* figure: dedupe shares
logical extents but frees compressed blocks, so the real on-disk saving is
smaller, roughly by your compression ratio. For ground truth compare `compsize`
**Disk Usage** before and after a run (its **Referenced** total should not
change — proof that no data was lost).

# EXAMPLES

Dedupe a small directory tree in place (no hashfile; fine for a few files):

```
oans -dr /foo
```

Dedupe a large tree, storing hashes so future runs are incremental. Re-running
the same command only re-hashes and re-dedupes what changed:

```
oans -dr --hashfile=foo.hash foo/
```

Replay the last run — with no *file* arguments `oans` reuses the options, paths,
and excludes saved in the hashfile. Ideal for cron:

```
oans --hashfile=foo.hash
```

Add another directory to the set (this also becomes the new stored config):

```
oans -dr --hashfile=foo.hash foo/ bar/
```

Preview what a run would dedupe, without changing anything (omit **-d**):

```
oans -r foo/
```

Skip tiny files and a snapshot directory on a media server:

```
oans -dr --hashfile=media.hash --min-filesize=1M \
     --exclude "/srv/media/.snapshots/*" /srv/media
```

Inspect a hashfile, watch savings over time, and export metrics:

```
oans --stats   --hashfile=foo.hash
oans --history --hashfile=foo.hash
oans --json    --hashfile=foo.hash | jq .reclaimed_total_bytes
```

Measure the best thread count for a NAS array (as root), storing it for later
runs:

```
sudo oans --autotune --hashfile=media.hash /srv/media
```

## Scheduled deduplication

Because the hashfile is self-describing, a scheduled run only needs to name it.
`oans` ships systemd template units (`oans@.service` / `oans@.timer`, installed
by `make install-systemd`). Set a job up once, then enable its timer — the
instance name is the hashfile's basename under `/var/cache/oans`:

```
sudo oans -dr --hashfile=/var/cache/oans/media.hash /srv/media
sudo systemctl enable --now oans@media.timer
```

The timer then re-deduplicates `/srv/media` weekly, hashing only what changed.
See `docs/nas-quickstart.md` in the source tree for the full walkthrough.

# EXIT STATUS

`oans` exits **0** on success. A non-zero status indicates a fatal error, such
as invalid options or a hashfile that could not be opened. Note that individual
per-file dedupe failures (e.g. a file that changed between scan and dedupe) are
reported in the summary but do **not** by themselves change the exit status;
check the *Not deduped* line, or run with **-v**, to detect them.

# ENVIRONMENT

**NO_COLOR**
  ~ If set (to any value), disables colored output, as **\--no-color** does.

**DUPEREMOVE_AUTOTUNE_MAX_FILES**, **DUPEREMOVE_AUTOTUNE_MAX_BYTES**, **DUPEREMOVE_AUTOTUNE_ROUNDS**
  ~ Bound the sample **\--autotune** reads (file count, total bytes, and number
    of interleaved measurement rounds).

# FILES

*hashfile*
  ~ The SQLite database named by **\--hashfile**. Alongside it, SQLite may keep
    `hashfile-wal` and `hashfile-shm` write-ahead-log sidecars; these are normal
    and should not be deleted by hand.

*/var/cache/oans/*
  ~ Conventional location for scheduled-job hashfiles used by the systemd units
    (see *Scheduled deduplication*).

# NOTES

Deduplication is supported only on **btrfs** and **xfs**.

On **xfs**, `oans` identifies the filesystem by its UUID. This works unprivileged
on Linux 6.4 and newer; on older kernels the UUID lookup needs root, so run
`oans` with **sudo** there (a scheduled root job is unaffected). If the
filesystem cannot be identified, `oans` now stops with an error instead of
silently reporting nothing to do. **btrfs** is unaffected.

The read-only report modes (**\--stats**, **\--history**, **\--json**, **-L**)
open the hashfile read-only and are safe to run while another `oans` process is
scanning or deduping the same hashfile: they show a consistent snapshot and
never write. Running two *writing* invocations at once (two **-d** runs, or a
**-d** and an **-R**) against the same hashfile is **not** supported and may
corrupt it.

Interrupting a run with Ctrl-C is safe: the kernel owns the data, and the
hashfile is a transactional database, so you can stop and re-run without
corrupting either. Only a power loss can, in principle, damage a hashfile — and
never your data.

Two logically identical files are not always deduped: `oans` works on extent
boundaries, so files with the same content but a different on-disk extent layout
may not match unless block-level matching is enabled with
**\--dedupe-options=partial**.

`oans` is offline, batch deduplication: you point it at specific trees and run it
on demand or on a timer. This differs from `bees`, an always-on daemon that
deduplicates the whole filesystem continuously, and from inline filesystem
dedupe such as ZFS, which maintains a permanent in-RAM dedupe table. `oans` keeps
no such table and imposes no background cost between runs.

# SEE ALSO

`btrfs`(8), `xfs`(8), `filesystems`(5), `ioctl_fideduprange`(2), `compsize`(8)

The `oans` project page: <https://github.com/martinus/oans>. `oans` is a fork of
`duperemove` <https://github.com/markfasheh/duperemove>.
