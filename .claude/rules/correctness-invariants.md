---
paths:
  - "src/run_dedupe.c"
  - "src/dedupe.c"
  - "src/fiemap.c"
  - "src/file_scan.c"
  - "src/oans.c"
  - "tests/integration/test_convergence.py"
---

# Correctness invariants

- **Ctrl+C is safe:** `FIDEDUPERANGE` is atomic and the hashfile stays
  WAL-consistent on kill. Only power loss risks it (`synchronous=OFF`).
- **A no-op rescan must net 0 changes** and leave row counts identical — a smoke
  test after any scan-path change.
- **"Reclaimed" is a logical figure, not disk.** It's the honest bytes freed
  (kernel-deduped = one physical copy kept per group), but on compressed btrfs
  dedupe frees *compressed* blocks while the number is *logical*, so real disk
  freed is ~ratio smaller. Verify with `compsize` **Disk Usage** before/after
  (not `df`); `Referenced` staying constant proves nothing was lost. The piped /
  `-q` "net change in shared extents" line is a separate fiemap diagnostic
  (counts the surviving copy too, ~2× for pairs); `--json`
  `reclaimable_logical_bytes` is a logical upper bound.
- **Never credit the kernel's `bytes_deduped` as space freed (#187).**
  `FIDEDUPERANGE` reports the whole compared length whether or not the range
  already shared the target's storage, so summing it counted work as savings: a
  destination half of which was already shared credited twice what it freed, and
  before #186 a run that freed *nothing* claimed 6.6 GiB. Each destination is
  measured before submission — `fiemap_unshared_bytes()` over the target's
  sorted physical addresses — and only that part is credited, capped by what the
  kernel got through and only for destinations it accepted. Approximate by at
  most one extent either way (a partial reference at a different offset into an
  uncompressed extent reads as unshared; any part of a compressed extent reads
  as shared), which is why it must never gate a *decision*, only the figure.
  Pinned by `test_reclaimed_excludes_what_was_already_shared` and the
  `test_fiemap_unshared_bytes` unit test.
- **Check a reported figure against `physical_footprint()`, not against
  sharing.** `tests/integration/test_convergence.py` asserts the two properties
  a summary cannot show — a second run frees nothing, and the reported figure
  equals the drop in distinct physical storage the tree references (summed from
  fiemap in the harness, independently of anything oans computes). #186 and
  #187 both shipped because the suite checked what oans *said* and which
  extents it *shared*, and never either of these. Its layout zoo lives in one
  tree on purpose: #186 only reproduced at that scale.
  - **A whole-file group is not a substitute for an extent group.** Whole-file
    groups load with `poff = 0` and skip `clean_deduped()` entirely, so only an
    extent-pass layout (same tail, different heads) reproduces #186 — a
    four-identical-file version of the same physical shape does not.
  - **The accounted-for set accumulates across a group's destinations (#191).**
    Seeded from the target, then grown by `fiemap_unshared_bytes()` as each
    destination is measured — because two destinations can already share an
    extent with *each other*, and crediting both in full claimed twice what
    releasing that one extent frees. Merged per destination, never inserted per
    address: the fragmented whole-file groups the sorted array exists for make
    the latter hopeless. Still counts one address repeated *within* a single
    destination twice; bounded, and noted at the definition.
