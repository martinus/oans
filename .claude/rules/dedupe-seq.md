---
paths:
  - "src/dbfile.c"
  - "src/oans.c"
  - "src/run_dedupe.c"
  - "tests/integration/test_cross_pass.py"
  - "tests/integration/test_pass_stride.py"
---

# dedupe_seq (incremental dedup)

Scan assigns `seq = config+1`, bumped every `--batchsize`/`-B` files (default
1024). `process_duplicates` loops `for i=dedupe_seq; i<max` over generations;
each group is deduped exactly once (a no-change rerun nets 0). The
`GET_DUPLICATE_*` loaders load only the members new in a pass plus one stable
member from an earlier pass as the target, loaded first (the anchor; see #265
in `hashfile-sqlite.md` and #197/#237 for whole files) — this
fixed both wasted per-pass re-load/re-fiemap of already-deduped members and
per-pass target drift (a group spanning passes used to converge to one cluster
*per pass* instead of a single extent). Exercise with `DUPEREMOVE_FILES_PER_PASS`
(`test_cross_pass.py`); don't reintroduce loading all `dedupe_seq <= ?2` members.

- **"The same target in every window" has to hold across runs too.** Two ways
  it did not, both silent, both converging to two clusters instead of one:
  - **A new whole-file copy that wins the election (#272).** The ranking
    (read-only, fewest extents, id) ranges over every member, so a copy that
    arrives later in one piece, or in a new read-only snapshot, outranks the
    older copies - which an earlier run deduped onto the *previous* target.
    Loaded alone it was a group of one, and no later run loaded the group
    again. The window that holds a target new this run (its generation in
    `(?1, ?2]`) also loads every member at or below `?3`, the generation the
    dedupe phase started at. Not the members of earlier windows of this run:
    every window of the run elected the same target, so they are on it
    already. `COUNT_FILES_WORK` counts such a group as `new - 1 + old`.
  - **Inside one run as well, and on a first scan.** `grp` counted only the
    members up to the window's end (`dedupe_seq <= ?2`) while the election
    counted all of them. A copy alone in an early window, with the better
    copy in a later one, saw a group of one and was skipped; the later
    window did not load it either, so it was never deduped. On
    `bench-queries.py`'s `manyfiles` that was 27% of a first scan's
    duplicate bytes (133 MB loaded of 181 MB estimated). Found by summing
    the loader's work per window against `dbfile_count_dupe_work()`: the
    two must be equal on every profile and case, and a mismatch is a loader
    bug until shown otherwise.
  - **Extent groups keyed the target two ways (#279).** The first window took
    the lowest file id, later ones the older copy with the lowest rowid, and
    rowids follow hashing order. Both now take the smallest (generation, file
    id, rowid): the earliest generation's member is in the first window with
    the group, and an older copy in every later one. The fix first proposed,
    `order by wo.id, o.rowid`, still breaks on a resumed file (#159), which
    keeps its id and gets a later generation; `bugs/dbfile.txt` has it.
  - **A window that holds only the target loads nothing.** Its copies are
    in other windows, so loading it alone made a group of one, which
    `push_results()` skipped with a line at default verbosity: 69,395 of
    them on `manyfiles`' first scan. `tgt` now keeps such a group only if
    another member loads with it; the per-group line is `--debug`, with one
    `-v` count.
  - Pinned by `test_a_new_whole_file_target_takes_the_older_copies_with_it`
    and `test_every_window_takes_the_same_extent_target`, whose fixtures store
    rows in an order that makes every weaker key disagree.
