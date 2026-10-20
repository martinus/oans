---
paths:
  - "src/progress.{c,h}"
  - "src/run_dedupe.c"
  - "tests/integration/test_progress_bytes.py"
  - "tests/integration/test_progress_json.py"
---

# Dedupe progress is byte-weighted

- **Group work = `de_len * (de_num_dupes - 1)`** (as `cmp_dext_work()`). The
  total comes up front from `dbfile_count_dupe_work()` (whole-file + extent,
  minus whole-file members' extents) at `seq_lo = first_seq`, batch-invariant
  (`test_progress_bytes.py::…_stable_across_passes`).
- **Each group credits exactly `W0` to `work_done_bytes`** by worker return:
  ticks per ≤32 MiB round (`ctxt->progress_fn`) plus one settle-up lump in
  `dedupe_worker()` for skips (clean_deduped, already-shared, changed,
  ENOENT/EINVAL, `DEDUPE_EXTENTS_CLEANED`). **Don't credit skip paths
  individually**; capture `w0` first (worker may free `dext`). Over-ticking is
  absorbed (99% cap, monotone clamp).
- `--dedupe-options=partial` groups add via `pdedupe_add_pushed_work()`;
  `total = max(upfront, pushed)`.
- `--progress=json` emits `work_done_bytes`/`work_total_bytes` **raw**;
  `pdedupe_end()` emits a final `done == total` record.
- **#184, #270:** one `dbfile_count_dupe_work()` query per pass gives count
  and sum, grouping only rows newer than `first_seq` with one older-member
  probe per group (#265; `hashfile-sqlite.md`). `seq_lo == 0` has no window,
  no probe. Not a `WHERE` on the group key (#184): 12.5 s at 100% new.
- Group estimate is **per-run**, like `pdd.done`, for `max(estimate, queued)`.
- `process_duplicates()` skips the call at `passes == 0`: insurance for a
  failed best-effort `dbfile_create_search_indexes()`.
