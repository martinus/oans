---
title: oans
section: 8
header: System Manager’s Manual
footer: oans 1.1.1
date: July 2026
---
# NAME
`oans` - Find duplicate regions in files and submit them for deduplication

# SYNOPSIS
**oans** *[options]* *files...*

# DESCRIPTION
`oans` is a simple tool for finding duplicated regions in files and
submitting them for deduplication. When given a list of files it will
hash their contents and compare those hashes to each
other, finding and categorizing regions that match each other. When
given the `-d` option, `oans` will submit those regions
for deduplication using the Linux kernel FIDEDUPERANGE ioctl.

`oans` computes hashes for each files extents as well as for
the whole file's content. Optionally, per-block hashes can be computed.

`oans` can store the hashes it computes in a `hashfile`. If
given an existing hashfile, `oans` will only compute hashes
for those files which have changed since the last run. Thus you can run
`oans` repeatedly on your data as it changes, without having to
re-checksum unchanged data. For more on hashfiles see the
`--hashfile` option below as well as the `Examples` section.

The hashfile is also **self-describing**: each run records its scan-shaping
options (`-d`, `-r`, `--skip-zeroes`, `--min-filesize`, `--dedupe-options`),
its paths, and its `--exclude` patterns. Running `oans --hashfile=FILE` with
no file arguments replays that stored configuration, so a periodic job only
needs to name the hashfile. See the `Examples` section.

# GENERAL
oans has two major modes of operation, one of which is a subset
of the other.

## Readonly / Non-deduplicating Mode

When run without `-d` (the default) oans will print out one or
more tables of matching extents it has determined would be ideal
candidates for deduplication. As a result, readonly mode is useful for
seeing what oans might do when run with `-d`.

Generally, oans does not concern itself with the underlying
representation of the extents it processes. Some of them could be
compressed, undergoing I/O, or even have already been deduplicated. In
dedupe mode, the kernel handles those details and therefore we try not
to replicate that work.

## Deduping Mode

This functions similarly to readonly mode with the exception that the
duplicated extents found in our "read, hash, and compare" step will
actually be submitted for deduplication. Extents that have already
been deduped will be skipped. An estimate of the total data
deduplicated will be printed after the operation is complete. This
estimate is calculated by comparing the total amount of shared bytes
in each file before and after the dedupe.

# OPTIONS

## Common options

`files` can refer to a list of regular files and directories or be
a hyphen (-) to read them from standard input.
If a directory is specified, all regular files within it will also be
scanned. oans can also be told to recursively scan directories with
the `-r` switch.

**-r**
  ~ Enable recursive dir traversal.

**-d**
  ~ De-dupe the results - only works on `btrfs` and `xfs`.

**\--hashfile**=`hashfile`
  ~ Use a file for storage of hashes instead of memory. This option drastically
reduces the memory footprint of oans and is recommended when your data
set is more than a few files large. `Hashfiles` are also reusable,
allowing you to further reduce the amount of hashing done on subsequent
dedupe runs.

    If `hashfile` does not exist it will be created. If it exists,
`oans` will check the file paths stored inside of it for changes.
Files which have changed will be rescanned and their updated hashes will be
written to the `hashfile`. Entries for files that were deleted from disk are
pruned automatically on the next scan (the check is by existence, so files
merely outside the paths scanned this run are kept); the hashfile is compacted
when enough of it becomes free. `-R` still removes specific paths explicitly.

    New files are only added to the `hashfile` if they are discoverable via
the `files` argument. For that reason you probably want to provide the
same `files` list and `-r` arguments on each run of
`oans`.  The file discovery algorithm is efficient and will only
visit each file once, even if it is already in the `hashfile`.

    You need not repeat them, though: each run records its scan-shaping
options, paths and `--exclude` patterns in the hashfile, so running
`oans --hashfile=FILE` with no file arguments replays the last such run
(any other options given on that command line are ignored). If none of the
stored paths still exist, oans refuses to run rather than prune every entry
(guarding against, for example, an unmounted drive); paths that are
individually missing are skipped with a warning.

    Adding a new path to a hashfile is as simple as adding it to the `files`
argument.

    When deduping from a hashfile, oans will avoid deduping files which
have not changed since the last dedupe.

**-B** `N`, **\--batchsize**=`N`
  ~ Run the deduplication phase every `N` files newly scanned. This greatly reduces
memory usage for large dataset, or when you are doing partial extents lookup,
but reduces multithreading efficiency.

    Because of that small overhead, its value shall be selected based
on the average file size and `blocksize`.

    The default of 1024 is a sane value for extents-only lookups, while you
can go as low as `1` if you are running `oans` on very large files (like
virtual machines etc).

**-m** `N`, **\--min-filesize**=`N`
  ~ Skip all regular files smaller than `N` bytes (suffixes like `K`, `M`, `G`
are accepted). Trees with many tiny files can be scanned much faster this way,
since such files rarely dedupe usefully. The default of `1` only skips empty
files.

**-h**
  ~ Print numbers in human-readable format.

**-q**
  ~ Quiet mode. oans will only print errors and a short summary of
any dedupe.

**-v**
  ~ Be verbose. Restores the per-group dedupe listing that is hidden by
default in favor of a progress bar and summary.

**\--no-color**
  ~ Disable colored output. Colors are also disabled automatically when
standard output is not a terminal or when the `NO_COLOR` environment
variable is set.

**\--help**
  ~ Prints help text.

## Advanced options

**-L**
  ~ Print all files in the hashfile and exit. Requires the `--hashfile` option.
Will print additional information about each file when run with `-v`.

**\--stats**
  ~ Print a summary of the hashfile and exit: its format and identity, the
stored scan configuration if any (the options, paths and excludes a bare
`oans --hashfile=FILE` would replay), how many files and hashes it holds, the
total logical data tracked, and how much whole-file duplication it records
(duplicate groups and reclaimable bytes). Requires the `--hashfile` option.
(Replaces the standalone `hashstats` tool.)

**\--history**
  ~ Print the run history recorded in the hashfile and exit: how many runs,
the total space reclaimed over their lifetime, and a timeline of the most
recent runs (date, reclaimed bytes, elapsed time, files hashed, and whether it
was a dedupe or scan-only run). Every run appends one row automatically.
Requires the `--hashfile` option.

**\--json**
  ~ Print a machine-readable JSON object of the hashfile's current metrics
(files, hashes, logical bytes, duplicate groups and reclaimable bytes) plus the
lifetime run-history totals, then exit. Intended for scripting and dashboards
(e.g. piping to `jq`, Telegraf, or a node\_exporter textfile). The
`reclaimable_logical_bytes` field is a logical upper bound; the real disk space
freed is smaller on a compressed filesystem. Requires the `--hashfile` option.

**-R** `files ..`
  ~ Remove file from the db and exit. oans will read the list from
standard input if a hyphen (-) is provided. Requires the `--hashfile` option.

    `Note:` If you are piping filenames from another oans instance it
is advisable to do so into a temporary file first as running oans
simultaneously on the same hashfile may corrupt that hashfile.

**\--skip-zeroes**
  ~ Read data blocks and skip any zeroed blocks, useful for speedup oans,
but can prevent deduplication of zeroed files.

**-b** `size`
  ~ Use the specified block size for reading file extents. Defaults to 128K.

**\--io-threads**=`N`
  ~ Use N threads for I/O. This is used by the file hashing and dedupe
stages. The default is the number of host cpus, capped at 8 - beyond that,
more threads mostly add filesystem lock contention instead of speed. An
explicit `N` overrides the cap.

**\--cpu-threads**=`N`
  ~ Use N threads for CPU bound tasks. This is used by the duplicate
extent finding stage. The default is the number of host cpus, capped at 8;
an explicit `N` overrides the cap.

**\--dedupe-options**=`options`
  ~ Comma separated list of options which alter how we dedupe. Prepend 'no' to an
option in order to turn it off.

    **[no]partial**
    ~ oans can often find more dedupe by comparing portions of extents
to each other. This can be a lengthy, CPU intensive task so it is
turned off by default. Using `--batchsize` is recommended to
limit the negative effects of this option.

        The code behind this option is under active development and as a
result the semantics of the `partial` argument may change.

    **[no]same**
    ~ Defaults to `on`. Allow dedupe of extents within the same
file.

    **[no]only_whole_files**
    ~ Defaults to `off`. oans will only work on full file. Both
extent-based and block-based deduplication will be disabled. The hashfile will
be smaller, some operations will be faster, but the deduplication efficiency
will indeed be reduced.

**\--debug**
  ~ Print debug messages, forces `-v` if selected.

**\--exclude**=`PATTERN`
  ~ You can exclude certain files and folders from the deduplication process. This
might be benefical for skipping subvolume snapshot mounts, for instance. Unless you
provide a full path for exclusion, the exclude will be relative to the current working
directory. Another thing to keep in mind is that
shells usually expand glob pattern so the passed in pattern ought to also be
quoted. Taking everything into consideration the correct way to pass an exclusion
pattern is `oans --exclude "/path/to/dir/file*" /path/to/dir`

# EXAMPLES

## Simple Usage

Dedupe the files in directory /foo, recurse into all subdirectories. You only want to use this for small data sets:

	oans -dr /foo

## Using Hashfiles
oans can optionally store the hashes it calculates in a
hashfile. Hashfiles have two primary advantages - memory usage and
re-usability. When using a hashfile, oans will stream computed
hashes to it, instead of main memory.

If oans is run with an existing hashfile, it will only scan
those files which have changed since the last time the hashfile was
updated. The `files` argument controls which directories
oans will scan for newly added files. In the simplest usage, you
rerun oans with the same parameters and it will only scan
changed or newly added files - see the first example below.

Dedupe the files in directory foo, storing hashes in foo.hash. We can
run this command multiple times and oans will only checksum and
dedupe changed or newly added files:

	oans -dr --hashfile=foo.hash foo/

Replay the last run: with no file arguments, oans reuses the options,
paths and excludes saved in the hashfile. Handy for a cron job, which then
only needs to name the hashfile:

	oans --hashfile=foo.hash

Add directory bar to our hashfile and discover any files that were
recently added to foo (this also becomes the new stored configuration):

	oans -dr --hashfile=foo.hash foo/ bar/

List the files tracked by foo.hash:

	oans -L --hashfile=foo.hash

Show how much space has been reclaimed over time, and export current metrics
for a dashboard:

	oans --history --hashfile=foo.hash
	oans --json --hashfile=foo.hash | jq .reclaimed_total_bytes

# FAQ

## Is oans safe for my data?

Yes. To be specific, oans does not deduplicate the data itself.
It simply finds candidates for dedupe and submits them to the Linux
kernel FIDEDUPERANGE ioctl. In order to ensure data integrity, the
kernel locks out other access to the file and does a byte-by-byte
compare before proceeding with the dedupe.

## Is it safe to interrupt the program (Ctrl-C)?

Yes. The Linux kernel deals with the actual data. On oans' side,
a transactional database engine is used. The result is that you
should be able to ctrl-c the program at any point and re-run without
experiencing corruption of your hashfile. In case of a bug, your hashfile
may be broken, but your data never will.

## I got two identical files, why are they not deduped?

oans by default works on extent granularity. What this means is if there
are two files which are logically identical (have the same content) but are
laid out on disk with different extent structure they won't be deduped. For
example if 2 files are 128k each and their content are identical but one of
them consists of a single 128k extent and the other of 2 * 64k extents then
they won't be deduped. This behavior is dependent on the current implementation
and is subject to change as oans is being improved.

## What is the cost of deduplication?

Deduplication will lead to increased fragmentation. The blocksize
chosen can have an effect on this. Larger blocksizes will fragment
less but may not save you as much space. Conversely, smaller block
sizes may save more space at the cost of increased fragmentation.

## How can I find out my space savings after a dedupe?

oans will print out an estimate of the saved space after a
dedupe operation for you.

You can get a more accurate picture by running 'btrfs fi df' before
and after each oans run.

Be careful about using the 'df' tool on btrfs - it is common for space
reporting to be 'behind' while delayed updates get processed, so an
immediate df after deduping might not show any savings.

## Why is the total deduped data report an estimate?

At the moment oans can detect that some underlying extents are
shared with other files, but it can not resolve which files those
extents are shared with.

Imagine oans is examining a series of files and it notes a shared
data region in one of them. That data could be shared with a file
outside of the series. Since oans can't resolve that information
it will account the shared data against our dedupe operation while in
reality, the kernel might deduplicate it further for us.

## Why are my files showing dedupe but my disk space is not shrinking?

This is a little complicated, but it comes down to a feature in Btrfs
called _bookending_. The [Btrfs wiki](http://en.wikipedia.org/wiki/Btrfs#Extents)
explains this in detail.

Essentially though, the underlying representation of an extent in
Btrfs can not be split (with small exception). So sometimes we can end
up in a situation where a file extent gets partially deduped (and the
extents marked as shared) but the underlying extent item is not freed
or truncated.

## Is there an upper limit to the amount of data oans can process?

oans is fast at reading and cataloging data. Dedupe runs will be
memory limited unless the `--hashfile` option is used. `--hashfile` allows
oans to temporarily store duplicated hashes to disk, thus removing the
large memory overhead and allowing for a far larger amount of data to be
scanned and deduped. Realistically though you will be limited by the speed of
your disks and cpu. In those situations where resources are limited you may
have success by breaking up the input data set into smaller pieces.

When using a hashfile, oans will only store duplicate hashes in
memory, so even very large data sets can be processed with a modest
memory footprint.

## How large of a hashfile will oans create?

Hashfiles are sqlite3 database files; their size scales with the number
of files and extents tracked, and grows with file fragmentation. To
inspect an existing hashfile - its size on disk, contents, and how much
duplication it records - run:

	oans --stats --hashfile=foo.hash

# NOTES
Deduplication is currently only supported by the `btrfs` and `xfs` filesystem.

The oans project page can be found on [github](https://github.com/martinus/oans).
oans is a fork of [duperemove](https://github.com/markfasheh/duperemove).

# SEE ALSO
* `filesystems(5)`
* `btrfs(8)`
* `xfs(8)`
* `ioctl_fideduprange(2)`
