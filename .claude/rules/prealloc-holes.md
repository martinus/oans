---
paths:
  - "src/file_scan.c"
  - "src/csum.c"
  - "src/fiemap.c"
  - "src/file_flags.h"
  - "tests/integration/test_prealloc.py"
  - "tests/integration/test_hole_extent.py"
---

# Preallocated extents are read, not faked (#273)

`is_area_ignored()` lets `fill_buffer()` fake a 1 MiB read buffer with zeroes
instead of reading it. It may do that only when **no byte of the area is
data**: every byte lies in a hole or in a `FIEMAP_SKIP_FLAGS` extent. The
upstream code answered true as soon as it reached any preallocated extent, and
on a hole it took the next extent without checking that it starts inside the
area. So 512 KiB of data before a `posix_fallocate` tail was hashed as zeroes,
and two different files got one digest. journald files, torrents, VM images and
databases all have that layout.

- **The right digest is the one of the same bytes written out.** That is what
  `test_prealloc.py` compares against; "the two digests differ" alone would
  pass for a digest of garbage.
- **Old hashfiles are repaired per row, not by a `DB_FILE_MINOR` bump.** An
  unchanged file is never hashed again, so a wrong digest would stay for good.
  Every file this binary hashes or copies gets `FILE_UNWRITTEN_CHECKED`
  (`flags & 4`). An up-to-date row without the bit costs one fiemap: it is
  rehashed if the file has an `UNWRITTEN` extent and just marked otherwise.
  A global "done" key was rejected because one hashfile can cover several
  trees scanned on different days. Old binaries read `flags` only as `& 1` and
  `& 2`, so the bit is safe for them; one that rewrites the row drops it, which
  costs one more fiemap.
- **The extent holding EOF runs past the file size, and is not always LAST.**
  `process_extents()` allowed for the overshoot only on `FIEMAP_EXTENT_LAST`,
  so a data extent followed by a preallocated one past EOF (`fallocate -n`)
  got no row and was never extent-deduped. Repaired the same way, per row:
  `FILE_EOF_EXTENT_CHECKED` (`flags & 8`), checked in the same fiemap as the
  bit above (`scan_row_ok()`); `FILE_SCAN_CHECKED` is both.
- **UNWRITTEN is only zeroes once the data is flushed.** Data written but not
  yet on disk can sit over an UNWRITTEN extent: XFS allocates delalloc as
  unwritten at writeback and converts it when the I/O completes, and a write
  into a fallocated range stays unwritten until flushed. The scan faked that
  data as zeroes, so two identical files scanned mid-writeback got different
  digests (the xfs-only flake of `test_a_crafted_name_...`). `do_fiemap()`
  counts with `FIEMAP_FLAG_SYNC`, which flushes and waits for the conversion;
  free on a clean file (250k tiny files, warm: 10.56 s vs 10.45 s). The
  harness `syncfs()`es before every `dm()` run, which hid this from the suite:
  the regression test passes `settle=False`, and is `serial` because another
  test's `syncfs()` flushes its file just as well.
- **An extent's digest leaves out the hole before it.** The scan skips only
  whole hash blocks of hole, so a hole that does not fill them is read as
  zeroes, and `get_extent()` answers the *next* extent for an offset in it.
  `process_extents()` hashed those zeroes into that extent, whose row still
  said `fe_logical`/`fe_length`: one 64 KiB extent behind 32K, 64K and 128K
  holes got three digests and was never extent-deduped. It steps over the gap
  now; file digest and block hashes are unchanged (checked byte-identical).
  Repaired per row by `FILE_HOLE_EXTENT_CHECKED` (`flags & 16`): rehashed when
  a data extent follows a gap that is unaligned to the hash block or follows a
  preallocated extent - wider than the exact cases, on purpose
  (`hole_read_before()`, shared with the check below).
  - **An older binary's checkpoint is refused if it follows such a gap.** In
    the hole it carries extent state started before its extent (`ext_loff >=
    loff`, refused whoever wrote it). Anywhere *after* the hole the damage is
    already done and looks fine: in the extent state, or in the extent rows
    stored before the checkpoint, which `dbfile_remove_hashes_from()` keeps -
    a checkpoint in a later hole has no extent state at all. The row the
    resumed run finishes then gets the bit, so the digest would stay for good.
    A row flag cannot tell who wrote a checkpoint (a checkpointed row has no
    flags yet), so the saved checksum state does: its magic is `OANSCSM2` now,
    `OANSCSM1` before, same layout, and every checkpoint has a file state.
    `adopt_resume()` refuses a version 1 checkpoint when any data extent before
    its offset is one `hole_read_before()` names, so an upgrade does not
    restart every interrupted huge file. It has to decide at adoption: a
    checkpoint saved again is version 2. Older binaries refuse version 2 and
    rehash.
  - **The tests make an older binary's checkpoints with this one.**
    `DUPEREMOVE_OLD_HOLE_DIGESTS` puts back the old gap hashing and saves
    version 1 states. Checked against a real older binary on the review's
    two-hole layout: the same extent rows and checkpoints, and state bytes
    identical but for XXH3's `extSecret` pointer. Rewriting the magic of a
    current checkpoint, as the first tests did, cannot produce a wrong digest,
    so it could only ever fail on a message. Pinned by
    `test_an_older_binarys_checkpoint_*`, whose refusal case fails on the
    extent fingerprint, in all three positions, without the check.
