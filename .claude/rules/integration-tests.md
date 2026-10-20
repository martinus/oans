---
paths:
  - "tests/integration/**"
  - "tests/run.py"
  - "tests/unit/fixtures.h"
---

# Integration tests: building extent layouts

- **A hole gives two extents on any filesystem (#242)**; fsync and CoW do
  not. XFS builds the unique-head / hole / shared-tail layout
  (`test_einval.py`, `@requires_reflink`); don't gate such a test on
  `@requires_btrfs` (the old `test_extent_dedupe.py`/`harness.py` claim
  otherwise was wrong).
  - **Punch the hole, never seek over it**: XFS reserves blocks past a
    buffered extending write and can map the gap as one DELALLOC record.
    `make_sparse()` punches then `ftruncate`s, like `mkfile_fiemap()` in
    `tests/unit/fixtures.h`.
  - Reverting the punch fails only CI's xfs leg. `punch_hole()` raises on
    failure, and a second test pins the raise on every filesystem.
  - CoW is needed only to reflink a file and rewrite its head in place so the
    tail extents survive (`ExtentDedupeCowTest`); that is all
    `requires_btrfs` means.
