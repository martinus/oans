---
paths:
  - "src/dbfile.c"
  - "src/oans.c"
  - "src/run_dedupe.c"
  - "tests/integration/test_cross_pass.py"
  - "tests/integration/test_pass_stride.py"
---

# dedupe_seq (incremental dedup)

- Scan assigns `seq = config+1`, bumped every `--batchsize`/`-B` files (1024).
  `process_duplicates` loops `for i=dedupe_seq; i<max`; each group is deduped
  once, so a no-change rerun nets 0.
- `GET_DUPLICATE_*` load a pass's new members plus one earlier member as
  target, loaded first (the anchor; #265 in `hashfile-sqlite.md`, #197/#237).
  **Never load all `dedupe_seq <= ?2` members**: re-fiemaps deduped members,
  target drifts per pass. Test with `DUPEREMOVE_FILES_PER_PASS`
  (`test_cross_pass.py`).
- **Every window, in every run, must elect the same target**, or the group
  silently converges to two clusters:
  - **#272:** a later whole-file copy can outrank (read-only, fewest extents,
    id) the target an earlier run used. The window holding a target new this
    run (generation in `(?1, ?2]`) also loads every member at or below `?3`
    (the phase's start generation), not earlier windows' members (already on
    it).
    `COUNT_FILES_WORK` counts it `new - 1 + old`.
  - `grp` must count all members, not only `dedupe_seq <= ?2`, or an early
    window's lone copy is never deduped (27% of a first scan's bytes on
    `bench-queries.py` `manyfiles`). Per-window loader work must sum to
    `dbfile_count_dupe_work()`; a mismatch is a loader bug.
  - **#279:** extent target is smallest (generation, file id, rowid). Not
    `order by wo.id, o.rowid`: breaks on a resumed file (#159), which keeps
    its id with a later generation (`bugs/dbfile.txt`).
  - `tgt` keeps a target-only window's group only if another member loads,
    else `push_results()` skips a group of one; that line is `--debug`, with
    one `-v` count.
  - Pinned by `test_a_new_whole_file_target_takes_the_older_copies_with_it`,
    `test_every_window_takes_the_same_extent_target` (row order defeats every
    weaker key).
