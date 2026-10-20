---
paths:
  - "src/run_dedupe.c"
  - "src/dedupe.{c,h}"
  - "src/fiemap.{c,h}"
  - "tests/integration/test_dedupe_idempotent.py"
  - "tests/integration/test_extent_dedupe.py"
  - "tests/integration/test_convergence.py"
  - "tests/integration/test_already_shared.py"
---

# Dedupe must converge: the already-shared skip (#186)

**A second `-d` run over an unchanged tree must reclaim 0.** `FIDEDUPERANGE`
reports `bytes_deduped` for already-shared ranges, so the summary hides it
(`oans -dr ~/` once claimed 6.6 GiB per run, freed 0). Check by rerunning.

- **`clean_deduped()` culls against the target, not pairwise**; one survivor
  per physical offset moved one copy per run. Whole-file groups (`poff = 0`,
  `GET_DUPLICATE_FILES`) skip it: if `--dedupe-options=only_whole_files`
  converges and the default does not, suspect the extent path.
- **`fiemap_maps_share()` compares coverage, not records** (a dedupe splits
  the destination's tail). **Never do `fe_physical + delta`**: a compressed
  extent's `fe_physical` addresses the whole extent. Compare `fe_physical` for
  equality where both sides are equally deep into their record. Holes facing
  holes are shared; a hole facing data is not.
- **Compare whole blocks** via `dedupe_shareable_len()` (the kernel rounds
  down), but **below one block use the untrimmed length**, or every sub-block
  file loses the skip.
  - **#280:** at EOF on both sides the tail block is shared. Trim the check,
    never a *request* unless the kernel asks.
  - **#280:** the whole-block retry after EINVAL on the first destination is
    per request (`dedupe_ctxt.aligned`), skipped if already whole blocks. A
    process-wide static let one EINVAL (NODATASUM, `chattr +C` copy) trim all
    later requests, which the check then called done forever.
  - "Changed since scan" allows `dedupe_blocksize() - 1` of fiemap overshoot,
    not 4095 (16K/64K blocks).
  - A destination failing in a later 32 MiB round keeps earlier rounds'
    credit; an ioctl failing outright still reports them (`fail_in_flight()`).

**Bisect by scope**: subdirectories and pairs converging while their union
does not means the group grew.

Pinned by `tests/integration/test_dedupe_idempotent.py`,
`test_extent_dedupe.py::test_group_on_two_physical_extents_converges_in_one_run`,
unit `test_fiemap_maps_share` (prefer synthetic fiemap records).

Vacuous-test traps: coreutils `cp` and `cat` reflink via `copy_file_range()`
(even `cp --reflink=never`), so use `dd` or python. Files under btrfs
`max_inline` (2048) are inline and never deduped, so "reclaimed 0" always
holds.
