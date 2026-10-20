---
paths:
  - "tests/integration/**"
  - "tests/run.py"
  - "tests/unit/fixtures.h"
---

# Integration tests: building extent layouts

- **A hole is how a test gets two extents on *any* filesystem — not an fsync,
  and not copy-on-write (#242).** `test_extent_dedupe.py` was `@requires_btrfs`
  for years on the strength of a docstring saying "XFS overwrites in place and
  keeps the file as one extent, so it cannot reproduce the partially-shared
  layout", and `harness.py`'s `requires_btrfs` comment said the same. Both were
  wrong, and the repo disproved them one file over the whole time:
  `test_einval.py` builds the identical unique-head / hole / shared-tail layout,
  is only `@requires_reflink`, and has been passing on the xfs leg all along. An
  outside report with `filefrag` and `xfs_bmap` output is what finally said so.
  - **Punch the hole, don't seek over it.** Seeking leaves one on btrfs and
    ext4 — measured identical, 106,496 bytes allocated either way — but XFS
    reserves blocks past the end of a buffered extending write and the gap can
    come back mapped as one DELALLOC record. `make_sparse()` punches and then
    `ftruncate`s, the same recipe `mkfile_fiemap()` in `tests/unit/fixtures.h`
    already used for the same reason.
  - **The test for that can only fail on one leg**, since ext4 and btrfs leave
    a hole whichever way it was built. So `punch_hole()` *raises* instead of
    ignoring the return value, and a second test pins that — the half of the
    guarantee every filesystem can show. Reverting the raise fails it; reverting
    the punch fails nothing outside CI's xfs leg, which is worth knowing before
    reading a green run as proof.
  - Copy-on-write is still genuinely required for one thing: reflinking a file
    and rewriting its head *in place* so the tail extents survive untouched.
    That is what `ExtentDedupeCowTest` still needs btrfs for, and it is the only
    thing `requires_btrfs` should be read as meaning.
