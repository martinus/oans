---
paths:
  - "src/file_scan.c"
  - "src/csum.c"
  - "src/fiemap.c"
  - "src/file_flags.h"
  - "tests/integration/test_prealloc.py"
  - "tests/integration/test_hole_extent.py"
---

# Preallocated extents and holes (#273)

- `is_area_ignored()` may let `fill_buffer()` fake a 1 MiB buffer with zeroes
  **only when no byte of the area is data** (all hole or `FIEMAP_SKIP_FLAGS`
  extent); otherwise data before a `posix_fallocate` tail hashes as zeroes.
- Compare against the digest of the same bytes written out
  (`test_prealloc.py`); "the digests differ" passes for garbage.
- **Repair old hashfiles per row via `flags` bits, never a `DB_FILE_MINOR`
  bump** or a global "done" key: unchanged files are never rehashed, and one
  hashfile covers trees scanned on different days. Set on every file hashed
  or copied. Old binaries read only `& 1`/`& 2`; one that rewrites a row
  drops the bits (one more fiemap).
  - `FILE_UNWRITTEN_CHECKED` (`& 4`): an up-to-date row without it costs one
    fiemap; rehash if it has an `UNWRITTEN` extent, else just mark.
  - `FILE_EOF_EXTENT_CHECKED` (`& 8`), same fiemap (`scan_row_ok()`): the EOF
    extent runs past the size and is not always `FIEMAP_EXTENT_LAST`;
    `process_extents()` must allow the overshoot on any extent
    (`fallocate -n`). `FILE_SCAN_CHECKED` = both.
  - `FILE_HOLE_EXTENT_CHECKED` (`& 16`): rehash when a data extent follows a
    gap unaligned to the hash block or a preallocated extent; deliberately
    wider than exact (`hole_read_before()`).
- **UNWRITTEN is zeroes only once flushed** (XFS delalloc, writes into a
  fallocated range). `do_fiemap()` counts with `FIEMAP_FLAG_SYNC`. The
  harness's `syncfs()` before `dm()` hides this: the test uses `settle=False`
  and `serial` (xfs flake of `test_a_crafted_name_...`).
- **An extent's digest excludes the hole before it.** A partial-block hole is
  read as zeroes and `get_extent()` answers the next extent, whose row says
  `fe_logical`/`fe_length`; `process_extents()` steps over the gap.
- **Refuse an older binary's checkpoint after such a gap.** `ext_loff >= loff`
  is refused from anyone; later damage (extent state, or rows
  `dbfile_remove_hashes_from()` keeps) is invisible. The checksum state magic
  tells the writer: `OANSCSM2` now, `OANSCSM1` before. `adopt_resume()`
  refuses version 1 when a data extent before its offset is one
  `hole_read_before()` names; decide at adoption, a re-save is version 2.
  Older binaries refuse version 2 and rehash.
  - `DUPEREMOVE_OLD_HOLE_DIGESTS` restores old gap hashing and saves version 1
    states; never fake one by rewriting the magic (cannot yield a wrong
    digest). Pinned by `test_an_older_binarys_checkpoint_*`.
