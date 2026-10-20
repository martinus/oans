---
paths:
  - "src/find_dupes.{c,h}"
  - "tests/unit/test_find_dupes.c"
---

# The extent search, and the bug testing it found

`find_dupes.c` is what `--dedupe-options=partial` runs: two files' block trees
walked in lockstep for runs of blocks that share a digest *and* sit
contiguously in both. It was **237 of 247 mutants surviving**, and writing the
first tests for it found a real bug.

- **`compare_extents()` recorded a match starting before the fast-forward.**
  When the current blocks disagree, a loop skips ahead until they match - its
  whole purpose - but `start[]` was assigned at the `next_match:` label above
  that loop and never re-assigned after it. So a match found by skipping was
  recorded as beginning inside the gap, and the range covered bytes that
  differ. `FIDEDUPERANGE` byte-verifies and is atomic, so the kernel refuses
  the request whole and the genuinely matching tail goes down with it: partial
  mode finds less than it should, exits 0, and says nothing.
  - **The evidence it was a bug rather than a design is inside the function.**
    `start_running_checksum()` was already *below* the fast-forward, so
    `match_id` describes only the matching blocks. A deliberately wider
    `start[]` would file the digest of n blocks under a length of n+k, and
    `find_alloc_dext()` keys on `(len, digest)` - so the same run reached after
    different-sized skips lands in different groups and meets nothing.
  - `start[]` is write-only in `compare_extents()` and read only by
    `record_match()`, which is what bounds the fix: it cannot change control
    flow, termination, which matches are found, or the `extent_end` gate.
- **A symmetric fixture cannot see which side of a pair was written.**
  `record_match()` fills `recs[]`, `soff[]` and `eoff[]` one slot per file, and
  laying both files out identically - the natural thing to write - makes the
  two slots hold the same numbers, so writing either into both changes nothing.
  47 of its 72 mutants survived on that alone; putting the matching run at a
  different offset in each file killed 11 of them. Same defect as the #197
  election fixture whose winner led on every column.
- **Its residue is the pool and the database.** 66 of the remaining 142 are in
  `find_additional_dedupe`/`search_file_extents`/`search_extent` and the pool
  helpers, which need the thread pool, the progress block and a hashfile to
  drive. `compare_extents()` and `record_match()` are reachable directly
  because the suite's manifest `#include`s the file, which is what made this iteration
  possible without any of that.
- `block_len()` already had `test_block_len`, covering all three branches
  including past-EOF. A second one was written for it here before that was
  checked, and deleted; the sweep credits the old test either way.
