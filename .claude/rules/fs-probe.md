---
paths:
  - "src/file_scan.c"
  - "src/dedupe.{c,h}"
  - "tests/integration/test_fs_probe.py"
  - "tests/integration/test_unsupported_fs.py"
---

# What oans will scan: allowlist, then probe (#224)

btrfs and XFS are an **allowlist fast path**, not the definition of supported.
Anything else is asked directly whether it implements `FIDEDUPERANGE`, so a
filesystem that gains the ioctl needs no code change. `is_fs_allowlisted()`
names the two; `dedupe_probe_fd()` asks everything else.

- **The probe is a real block-sized request, and the answer is read from BOTH
  `rc`/`errno` and `info[0].status`.** A zero-length probe reading only errno
  looks obviously safer and is *wrong*: measured on a container's overlayfs
  over ext4, zero-length returns `rc=0` (overlayfs answers for itself, as a
  pass-through, without consulting the storage underneath) while a 4K request
  returns `rc=0, status=-EINVAL`. Plain ext4 returns `rc=-1/EOPNOTSUPP` for
  every shape. So errno-only accepts every containerised run on an overlay
  root, which then hashes the whole tree and fails at dedupe time — precisely
  what refusing unsupported filesystems upfront exists to prevent. Pinned by
  the `dedupe_classify_probe` unit test; reproduce with an `overlay` mount over
  ext4 and `./oans -rq`.
  The length comes from `dedupe_shareable_len()`/`cached_blocksize()`, the
  helpers the dedupe path already uses: `dest_offset` must be block-aligned or
  a filesystem that *does* support the ioctl answers `EINVAL`, which reads as
  UNKNOWN and quietly defeats the probe. Don't hardcode 4096 — 64K blocks exist
  and the codebase can query the real value.
- **The trade the real request buys that with:** on a filesystem that *does*
  support dedupe, the probe's two ranges may turn out identical and get shared.
  That is a genuine, byte-verified dedupe of one block within one file — it
  cannot change contents, size or mtime, and it happens only when the answer is
  yes, i.e. when oans is about to deduplicate the tree anyway.
  `test_fs_probe.py::…_leaves_the_file_alone` pins the file being untouched.
- **The taxonomy is the whole feature, so it is a pure function**
  (`dedupe_classify_probe`, unit-tested against every errno and status that
  matters). Only `EOPNOTSUPP`/`ENOTTY` — from either errno or a negative status
  — mean *no*; everything else, `EINVAL` above all (the man page documents it
  both for "the filesystem does not support deduplicating the ranges" and for a
  dozen per-file conditions), is **UNKNOWN**, and the next file is asked, up to
  `FS_PROBE_MAX_TRIES`. Never widen *no*: a wrong *no* silently skips someone's
  whole tree.
- **Probed lazily, at the first regular file, not at seed time.** The ioctl
  needs a writable regular file and a root is normally a directory — measured,
  a directory fd gives `EISDIR`, not `EOPNOTSUPP`. Creating a scratch file
  instead is a non-starter (read-only mounts; read-only subvolumes are scanned
  by default since #182). So `check_file()` locks the root provisionally and
  `__scan_file()` — the single consumer, hence no locking — settles it before
  anything is hashed. A definite no returns nonzero, which stops the consume
  loop — **and sets the walk-abort flag the walker threads poll**, mirroring
  `interrupted()`. Without that the walkers readdir/statx the whole tree after
  the error is already on screen: measured on a 20k-file tree, 402 `getdents64`
  became 16. An UNKNOWN file is hashed normally, so an unrecognised filesystem
  costs at most `FS_PROBE_MAX_TRIES` files before it is refused.
- **A file that cannot host the probe is not asked, and costs no try.** One
  under two blocks, or one that will not open read-write, said nothing about
  the filesystem but spent `FS_PROBE_MAX_TRIES`, so a supported tree whose
  first sixteen files were small or 0444 was refused. A file we own is asked
  through a read-only fd, as the dedupe phase opens it (an UNKNOWN that way is
  not counted either: older kernels refuse owner dedupe). `EROFS` still counts,
  since no file of a read-only mount gets past it.
  - **They get a cap of their own, `FS_PROBE_MAX_UNASKED` (1024).** Each one
    is hashed while the question is open, so uncapped, a user who may not
    write a tree on ext4 or NFS hashed all of it before the refusal, where
    the old budget refused after 16 files. Larger than the try budget because
    such files say nothing: a supported filesystem is refused only if 1024 of
    them come before any file that can host the probe.
  - **Both probe opens go through `open_listed()`**, #310's helper with the
    access mode as a parameter. A 0444 FIFO swapped in after the walker's
    statx fails the `O_RDWR` open with `EACCES`, and a plain `O_RDONLY`
    fallback then blocked the whole walk waiting for a writer. A name that is
    no longer a regular file is neither asked nor counted; `__scan_file()`
    skips it as before. Pinned by the unit test
    `test_the_probe_skips_a_fifo_without_blocking`, whose `SIGALRM` without
    `SA_RESTART` turns the hang into an `EINTR` failure.
- **Four ways to end up refusing, and they say different things.** A definite
  no; `FS_PROBE_MAX_TRIES` files that all declined; `FS_PROBE_MAX_UNASKED`
  files that could not be asked; and a walk that ended with the question
  never asked (`filescan_fs_probe_unsettled()` — an empty tree, or everything
  filtered by `--exclude` or size). The last one matters most: it is the case
  where exiting 0 would look like a clean run.
- **Not routed through `seed_fs_lock_failed`.** That reports roots which could
  never be locked at all and only fires when *none* was seeded; here a root was
  seeded and the probe has already said precisely what is wrong.
- **One place words the refusal** (`report_fs_unusable()`), because which of the
  four cases it is decides the advice, and `file_scan` owns the state — the
  same shape as `filescan_report_excludes()`. It also skips the report on an
  interrupted run, whose exit status belongs to the signal.
- **`DUPEREMOVE_FORCE_FS_PROBE=1` drops the fast path.** Without it the accept
  branch is unreachable in tests: the only filesystems known to answer *yes*
  are the two that never reach the probe. `test_fs_probe.py` uses it to run the
  probe against the real scratch btrfs/XFS, which is the only way this ships
  having been seen to say *yes* and not just *no*.
- The refusal paths are testable anywhere: ext4 and tmpfs answer `EOPNOTSUPP`,
  and an `overlay` mount over either reproduces the stacking case.
