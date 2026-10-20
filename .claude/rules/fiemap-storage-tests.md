---
paths:
  - "src/fiemap.{c,h}"
  - "src/storage.{c,h}"
  - "src/btrfs-util.{c,h}"
  - "tests/unit/test_fiemap.c"
  - "tests/unit/test_storage.c"
  - "tests/unit/fixtures.h"
---

# fiemap and the storage heuristic — where the fixture is the filesystem

`fiemap.c` was **177 of 487 surviving** and is now 122; `storage.c`'s two pure
functions were the last untested pure code in the tree. Both are small, and both
taught the same thing from opposite directions.

- **`fm_mapped_extents` never exceeds the buffer (#288).** With fewer slots
  than extents - none, when the count pass found none or failed - the kernel
  only counts and reports the real number, so every reader walked past the
  allocation. The file changed between the calls.
  - **A clamp is not enough: a short map must never be returned.** With
    *some* slots the kernel fills them and stops, which looks exactly like a
    complete map, and readers take running out of records for a hole - so two
    maps cut at the same index compared their unknown tails as shared and the
    destination was skipped. `fiemap_map()` asks about the remainder when the
    buffer is full and the last record is neither `LAST` nor at the range's
    end, retries with more slots, then returns NULL.
  - **The count is a lower bound on btrfs, so a retry doubles the slots and
    never recounts.** btrfs skips its shared check when only counting
    (`fi_extents_max == 0`), so physically adjacent extents of which only one
    is shared merge into one record when counted and stay two when mapped.
    Every deduplicated file can have that layout. With a recount, both tries
    came back one short, and an idle 50 GiB qcow2 was skipped as "extents
    changed while mapping it" on every run.
    `test_fiemap_map_outgrows_a_short_count` builds the layout with clones,
    and skips on a filesystem that counts it right.
- **A synthetic record array cannot reach `do_fiemap()` itself.** Every fiemap
  test until now built `struct fm_rec` fixtures and called the map walkers
  directly, which is right for `fiemap_maps_share()` and the layout key — but
  it left the ioctl wrapper, its buffer sizing and its range arithmetic with no
  test at all. Three tests now drive a **real file**, which is the only way to
  ask whether the thing that produces those arrays produces them correctly.
- **Then the fixture is a filesystem, and it has to be asked rather than
  assumed.** `/tmp` is tmpfs on most current distributions and tmpfs has no
  `->fiemap` at all — it answers `EOPNOTSUPP` to every request. So a test
  written against `/tmp` fails on a dev box and passes in CI, or worse passes
  everywhere by asserting a zero it would have got either way. The fixture
  prefers `DUPEREMOVE_TEST_DIR`, **probes `FS_IOC_FIEMAP` before relying on
  it**, and skips by name when the answer is no. Verified by pointing
  `DUPEREMOVE_TEST_DIR` at `/dev/shm`: three named skips, suite still green.
  - Punch holes with `fallocate(FALLOC_FL_PUNCH_HOLE)` and `ftruncate` the
    file, don't just seek past them. XFS speculative preallocation
    (`xfs_iomap_prealloc_size`) fills the gap and reports it `DELALLOC`, so a
    sparse fixture built by seeking has no hole on the very filesystem half the
    CI matrix runs.
  - **A negative assertion about a map needs the map to exist.**
    `fiemap_count_shared()` returns early on a NULL map, so "a fresh file shares
    nothing" is equally satisfied by fiemap mapping *nothing* — the test stayed
    green with `fiemap_count_extents()` stubbed to zero. Establish the
    precondition first; that is what makes `0` a different statement from "there
    was nothing to look at".
- **`storage_recommend_io_threads()` had a thorough-looking table and nine
  survivors, and eight of the nine are provably equivalent.** `<` → `<=` at
  `ncpus < AUTO_THREADS_CAP` differs only where both answer `CAP`; `num_devices
  <= 1` → `<= 2` is the same function, because a two-device pool's `2 * 2`
  already equals the single-disk clamp of 4. **Check the equivalents rather
  than counting them** — the two that were not are the ones worth the trip:
  - `<= 1` → `== 1` on the device count. A zeroed profile reaches the pool arm,
    computes `2 * 0`, and recommends **no reader threads at all** — which sizes
    three pools. `storage_detect()` seeds `num_devices = 1` so it is defensive,
    but "defensive" is exactly the code a test has to hold in place.
  - Every existing case sat at `base >= 4` or `base <= 2`, where `base < 4 ?
    base : 4` and a clamp of *three* agree. One three-core case separates them.
- **`storage_describe()` had no test at all**, so all four spellings of its
  device-count test survived — `>= 1`, `> 0`, `> 2` each describe a single disk
  as a pool or a pool as a single disk, in the line a run prints about its own
  storage. It is a pure function taking a struct and a buffer; there was never a
  reason beyond nobody looking.
- **`btrfs-util.c` gets no tests, and that is the answer rather than a gap.**
  All 28 survivors are inside three `BTRFS_IOC_*` wrappers, as is most of
  `storage_detect`/`read_rotational`/`detect_btrfs` — sysfs reads and ioctls on
  real block devices. Triage to that point and stop; the next honest move there
  is a fault-injection seam, not a bigger fixture.
