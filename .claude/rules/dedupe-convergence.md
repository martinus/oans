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

# Dedupe must converge — the already-shared skip (#186)

**A second `-d` run over an unchanged tree must reclaim 0.** Nothing in the
output tells you when it doesn't: `FIDEDUPERANGE` returns `bytes_deduped` for a
range that already shares storage, so a run that frees nothing still reports
gigabytes. On a 415 GiB home, `oans -dr ~/` claimed **6.6 GiB every single run**
with a measured free-space change of **0**. Check convergence by *repeating the
command*, not by reading the summary.

Three separate causes, each worth a fix; the numbers are successive stages on
that same tree (6.6 GiB → 1.4 GiB → 131 MiB → 21.3 MiB → **0 B**):

- **`clean_deduped()` culls against the target, not pairwise.** It used to
  collapse a group to one survivor per distinct physical offset. Only that
  survivor is repointed by the ioctl, so the rest of its class stayed on the old
  extent and that extent was never freed — one copy moved per run, nothing
  reclaimed until the last one landed. **Whole-file groups load with `poff = 0`**
  (`GET_DUPLICATE_FILES` has no fiemap to draw on) so they never hit this, which
  is why `--dedupe-options=only_whole_files` converged where the default did not.
  That asymmetry is the diagnostic — if only_whole_files converges, suspect the
  extent path.
- **`fiemap_maps_share()` compares coverage, not extent records.** The
  same shared storage is described with different record boundaries in each
  file: a dedupe stops on a block boundary and splits the destination's tail
  where the target has one record. Record-for-record equality reads that as "not
  shared" and resubmits the whole file forever. **Never do `fe_physical + delta`
  arithmetic** — on a compressed extent `fe_physical` addresses the compressed
  extent as a whole, so an offset into it is meaningless; compare `fe_physical`
  for equality only, at points where both sides sit the same distance into their
  record. Holes count too: fiemap omits them, matching holes on both sides *are*
  shared (a browser cache is mostly hole), and a hole facing data is not.
- **Compare whole blocks only.** The kernel rounds a dedupe length down to a
  filesystem block, so a trailing partial block that is not at both files' EOF
  is never shared, and comparing it makes the check permanently unsatisfiable.
  Trim with `dedupe_shareable_len()` — but **fall back to the untrimmed length
  below one block**, or the skip is silently disabled for every sub-block file
  (that alone was the last 21.3 MiB).
  - **At EOF on both sides the tail block *is* shared.** Measured on btrfs,
    kernel 7.1 (#280): a 10,000-byte pair deduped with length 10,000 ends as
    one shared 3-block extent; a 5,000-byte range ending at neither EOF is
    shortened to 4,096 and still *reported* as 5,000. So trimming stays right
    for the check, which only compares less, but a *request* must not be
    trimmed unless the kernel asks for it.
  - **The kernel asks per request, and so does oans now (#280).** An EINVAL
    on the first destination makes oans retry with whole blocks, for kernels
    that reject an unaligned length. That was a process-wide static, so one
    EINVAL - a NODATASUM mismatch from a file copied out of a `chattr +C`
    directory is enough - trimmed every later request on every thread, left
    every later file's tail unshared, and the check above then called those
    files done forever. It is `dedupe_ctxt.aligned` now, and the retry is
    skipped when the length is already whole blocks.
  - The "changed since scan" size check allows `dedupe_blocksize() - 1` of
    fiemap overshoot, not 4095: on 16K and 64K blocks the old slack skipped
    most shared tails as changed.
  - A destination that fails in a later 32 MiB round keeps the credit of the
    rounds before it, and an ioctl that fails outright still reports its
    destinations (`fail_in_flight()`).

Diagnosing this class: **bisect by scope.** Every subdirectory and every *pair*
of subdirectories converged while their union did not — that pattern means the
group got bigger, not that some directory is cursed.

Pinned by `tests/integration/test_dedupe_idempotent.py`,
`test_extent_dedupe.py::test_group_on_two_physical_extents_converges_in_one_run`
and the `test_fiemap_maps_share` unit test (the map walk is pure and worth
testing directly — synthetic fiemap records beat coaxing btrfs into a layout).

Two traps when writing tests here, both of which make a test pass while testing
nothing:

- **coreutils `cp` and `cat` reflink.** They go through `copy_file_range()`,
  which btrfs implements as a clone — `cp --reflink=never` does *not* give you
  an independent copy. Use `dd`, or write the bytes from python.
- **A file below btrfs `max_inline` (2048 by default) is stored inline**, has no
  physical location, and the kernel declines to dedupe it. Any "reclaimed 0"
  assertion on such a file holds no matter what the code does.
