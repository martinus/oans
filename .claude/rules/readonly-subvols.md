---
paths:
  - "src/file_scan.c"
  - "src/btrfs-util.{c,h}"
  - "tests/integration/test_readonly_*.py"
---

# Read-only subvolumes are scanned by default (#182)

**The kernel deduplicates *into* a read-only btrfs subvolume.** Only a read-only
*mount* is refused (`vfs_dedupe_file_range_one()` → `mnt_want_write_file()`), and
a probe on 7.1.3 accepted a read-only-snapshot destination and rewrote its extent
map. Don't reintroduce "the kernel refuses read-only destinations" anywhere — it
is the false premise this took two PRs to unwind:

- #156/#164 skipped snapshots by default under `-d`, reasoning the work was
  *provably* wasted. #171/#172 disproved that but only fixed the dedupe side
  (a read-only member is *preferred as the source*, never refused), leaving the
  default keyed to the dead premise until #182 flipped it.
- The option survives as `--skip-readonly-subvols` (off by default, no longer
  tri-state/`-d`-derived) because the **cost** argument is real and workload-
  dependent: snapper/Timeshift snapshots already share the live subvolume's
  extents, so hashing 20 of them reads ~20 TiB per TiB for nothing; an
  rsync-then-snapshot backup target holds independent copies and is exactly what
  offline dedupe is for. One default can't serve both.
- Trade the default accepts: a writable file deduped against a snapshot is
  rewritten to point at *the snapshot's* extents, so deleting that snapshot then
  frees less than its size suggests.
- 1.7.x hashfiles store `opt_skip_readonly_subvols = -1` (the old "auto"). That
  means "the user never asked", so `dbfile_load_scan_config` coerces `< 0` to 0 —
  a replay adopts the new default; only an explicit `1` survives.
