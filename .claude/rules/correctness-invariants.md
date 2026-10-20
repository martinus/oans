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

- **Ctrl+C is safe**: `FIDEDUPERANGE` is atomic, the hashfile WAL-consistent.
  Only power loss risks it (`synchronous=OFF`).
- **A no-op rescan nets 0 changes**, row counts identical; smoke-test after
  any scan-path change.
- **"Reclaimed" is logical**; compressed btrfs frees ~ratio less. Verify with
  `compsize` **Disk Usage** (not `df`); constant `Referenced` = nothing lost.
  The piped/`-q` "net change in shared extents" line is a fiemap diagnostic
  (~2× for pairs); `--json` `reclaimable_logical_bytes` is an upper bound.
- **Never credit `bytes_deduped` as freed (#187)**: it is the whole compared
  length. Credit `fiemap_unshared_bytes()` over the target's sorted physical
  addresses, measured before submission, capped by what the kernel got
  through, accepted destinations only. Off by up to one extent: **never let
  it gate a decision**. Pinned by
  `test_reclaimed_excludes_what_was_already_shared`, unit
  `test_fiemap_unshared_bytes`.
- **Check reported figures against `physical_footprint()`**:
  `tests/integration/test_convergence.py` asserts a second run frees nothing
  and the figure equals the drop in distinct physical storage; #186/#187
  shipped without this. Keep its layouts in one tree (#186 needs the scale).
  - Only an extent-pass layout (same tail, different heads) reproduces #186;
    whole-file groups load `poff = 0` and skip `clean_deduped()`.
  - **#191:** the accounted-for set starts from the target and grows by
    `fiemap_unshared_bytes()` per destination (destinations can share with
    each other). Merge per destination, never insert per address. An address
    repeated within one destination counts twice (bounded, noted in code).
