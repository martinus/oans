---
paths:
  - "src/find_dupes.{c,h}"
  - "tests/unit/test_find_dupes.c"
---

# The extent search

`find_dupes.c` (`--dedupe-options=partial`) walks two block trees in lockstep
for runs sharing digests and contiguous in both.

- **`compare_extents()` sets `start[]` after the fast-forward**, not at
  `next_match:`; else a skipped-to match starts in the gap and
  `FIDEDUPERANGE` refuses it whole, silently losing the matching tail.
  `start_running_checksum()` is below the fast-forward (`match_id` covers
  matching blocks only) and `find_alloc_dext()` keys on `(len, digest)`, so a
  wider `start[]` is wrong. `start[]` is read only by `record_match()`: it
  cannot change control flow, termination, matches, or the `extent_end` gate.
- **Fixtures must be asymmetric**: `record_match()` fills `recs[]`, `soff[]`,
  `eoff[]` per file, so identical layouts hide side mix-ups (47/72 mutants
  survived). Same trap as the #197 election fixture.
- `compare_extents()`/`record_match()` are unit-reachable because the
  manifest `#include`s the file; `find_additional_dedupe`,
  `search_file_extents`, `search_extent` and pool helpers need pool, progress
  and hashfile (untested residue).
- `test_block_len` covers `block_len()` fully; don't add another.
