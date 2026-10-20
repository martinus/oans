---
paths:
  - "src/fiemap.{c,h}"
  - "src/storage.{c,h}"
  - "src/btrfs-util.{c,h}"
  - "tests/unit/test_fiemap.c"
  - "tests/unit/test_storage.c"
  - "tests/unit/fixtures.h"
---

# `fiemap.c` and `storage.c` tests

- **`fm_mapped_extents` never exceeds the buffer (#288).** With too few slots
  (none, if the count found none or failed) the kernel only counts; the file
  can change between calls.
  - **Never return a short map.** A filled buffer looks complete and readers
    take the rest for a hole, so two maps cut at one index compare unknown
    tails as shared. `fiemap_map()` asks about the remainder when the buffer
    is full and the last record is neither `LAST` nor at the range end,
    retries with more slots, then returns NULL.
  - **A retry doubles the slots; never recount.** btrfs skips its shared check
    when counting (`fi_extents_max == 0`), so adjacent extents with one shared
    count as one record and map as two (any deduped file). A recount came back
    short every run. Pinned by `test_fiemap_map_outgrows_a_short_count`.
- `struct fm_rec` fixtures suit `fiemap_maps_share()` and the layout key;
  `do_fiemap()` (buffer sizing, range arithmetic) needs a **real file**.
- **Probe the filesystem.** tmpfs (`/tmp`) has no `->fiemap` (`EOPNOTSUPP`).
  The fixture prefers `DUPEREMOVE_TEST_DIR`, probes `FS_IOC_FIEMAP` and skips
  by name (at `/dev/shm`: three named skips, suite green).
  - Make holes with `fallocate(FALLOC_FL_PUNCH_HOLE)` + `ftruncate`, never by
    seeking: XFS preallocation (`xfs_iomap_prealloc_size`) reports a seeked
    gap as `DELALLOC`.
  - **A negative assertion needs the map to exist**: `fiemap_count_shared()`
    returns early on NULL, so "shares nothing" passed with
    `fiemap_count_extents()` stubbed to `0`. Assert the precondition first.
- **Prove equivalent mutants one at a time.** `storage_recommend_io_threads()`
  had nine survivors, eight equivalent (`<` → `<=` at
  `ncpus < AUTO_THREADS_CAP` differs only where both answer `CAP`;
  `num_devices <= 1` → `<= 2` is identical, since a two-device pool's `2 * 2`
  equals the single-disk clamp of 4). The two that were not:
  - `<= 1` → `== 1`: a zeroed profile reaches the pool arm and recommends
    `2 * 0`, no reader threads for three pools. `storage_detect()` seeds
    `num_devices = 1`; the test holds the defensive code anyway.
  - A clamp of three agrees with `base < 4 ? base : 4` at `base >= 4` and
    `base <= 2`; a three-core case separates them.
- `storage_describe()` is pure; untested, its device-count mutants (`>= 1`,
  `> 0`, `> 2`) swapped single disk and pool in the line a run prints.
- **`btrfs-util.c` gets no tests.** All 28 survivors are in three
  `BTRFS_IOC_*` wrappers, like most of `storage_detect`/`read_rotational`/
  `detect_btrfs`. Stop there; the next step is a fault-injection seam.
