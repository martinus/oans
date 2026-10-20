---
paths:
  - "src/file_scan.c"
  - "src/dedupe.{c,h}"
  - "tests/integration/test_fs_probe.py"
  - "tests/integration/test_unsupported_fs.py"
---

# What oans will scan: allowlist, then probe (#224)

- btrfs/XFS are a fast path (`is_fs_allowlisted()`), not the definition of
  supported; any other filesystem is asked by `dedupe_probe_fd()` whether it
  implements `FIDEDUPERANGE`.
- **Probe with a real block-sized request and read BOTH `rc`/`errno` and
  `info[0].status`.** Overlayfs over ext4 answers zero-length `rc=0` and 4K
  `rc=0, status=-EINVAL` (plain ext4: `rc=-1/EOPNOTSUPP`), so errno-only
  accepts every overlay root. Reproduce: `overlay` over ext4, `./oans -rq`.
- Length from `dedupe_shareable_len()`/`cached_blocksize()`, never 4096: an
  unaligned `dest_offset` gets `EINVAL` even where dedupe works.
- The probe may really share one block within the file (only when the answer
  is yes); contents, size, mtime unchanged, pinned by
  `test_fs_probe.py::…_leaves_the_file_alone`.
- **`dedupe_classify_probe` (pure, unit-tested): only `EOPNOTSUPP`/`ENOTTY`,
  from errno or a negative status, mean no. Never widen no**; a wrong no skips
  a whole tree. All else (`EINVAL` above all) is UNKNOWN: the file is hashed
  normally and the next one asked, up to `FS_PROBE_MAX_TRIES`.
- **Probe lazily at the first regular file** (a directory fd gives `EISDIR`; no
  scratch file on read-only mounts or subvolumes, #182). `check_file()` locks
  the root provisionally, `__scan_file()` (single consumer) settles it before
  hashing. A definite no stops the consume loop **and sets the walk-abort flag
  the walkers poll** (like `interrupted()`), or they read the whole tree (402
  vs 16 `getdents64` on 20k files).
- **A file that cannot host the probe is not asked and costs no try**: under
  two blocks or not openable read-write (else 16 small or 0444 files refuse a
  supported tree). An owned file is asked via a read-only fd, as dedupe opens
  it; an UNKNOWN that way does not count (older kernels refuse owner dedupe).
  `EROFS` counts.
  - Unaskable files have their own cap, `FS_PROBE_MAX_UNASKED` (1024): they are
    hashed while the question is open.
  - **Both probe opens use `open_listed()`** (#310): a 0444 FIFO fails `O_RDWR`
    with `EACCES` and an `O_RDONLY` fallback blocked the walk. A name no longer
    regular is neither asked nor counted. Pinned by
    `test_the_probe_skips_a_fifo_without_blocking` (`SIGALRM` without
    `SA_RESTART` turns the hang into `EINTR`).
- Four refusals, worded differently: definite no; `FS_PROBE_MAX_TRIES`
  declines; `FS_PROBE_MAX_UNASKED` unaskable; never asked
  (`filescan_fs_probe_unsettled()`: empty tree or all filtered by `--exclude`
  or size), which must not exit 0.
- Not via `seed_fs_lock_failed` (roots never locked, fires only when none was
  seeded). `report_fs_unusable()` alone words the refusal (`file_scan` owns
  the state, like `filescan_report_excludes()`) and skips it on an interrupted
  run, whose exit status belongs to the signal.
- `DUPEREMOVE_FORCE_FS_PROBE=1` drops the fast path so the accept branch is
  testable; `test_fs_probe.py` runs it on the scratch btrfs/XFS. Refusals test
  anywhere: ext4 and tmpfs answer `EOPNOTSUPP`, `overlay` adds stacking.
