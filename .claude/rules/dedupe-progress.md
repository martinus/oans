---
paths:
  - "src/progress.{c,h}"
  - "src/run_dedupe.c"
  - "tests/integration/test_progress_bytes.py"
  - "tests/integration/test_progress_json.py"
---

# Dedupe progress is byte-weighted (not group-counted)

The dedupe bar tracks the kernel **byte-verify volume**, not a fuzzy group
count, so it moves smoothly 0→100% (like hashing) even through one giant group.

- **Work of a group = `de_len * (de_num_dupes - 1)`** — the bytes the kernel
  byte-compares, same figure `cmp_dext_work()` sorts by. The exact phase total
  is summed up front by `dbfile_count_dupe_bytes()` (whole-file + extent, minus
  extents owned by whole-file dup members, since the whole-file pass deletes
  those rows first). Passed `seq_lo = first_seq`, so it matches what the
  per-pass loaders hand the workers regardless of how generations split — the
  sum is batch-invariant (`test_progress_bytes.py::…_stable_across_passes`).
- **Settlement contract:** every group credits *exactly* `W0` bytes to
  `work_done_bytes` by the time its worker returns — ticked smoothly per ≤32 MiB
  ioctl round (via `ctxt->progress_fn`), plus a settle-up lump in
  `dedupe_worker()` for whatever was skipped (clean_deduped, already-shared,
  changed-since-scan, ENOENT/EINVAL, the DEDUPE_EXTENTS_CLEANED early return).
  **Don't try to credit each skip path exactly** — capture `w0` before the work
  (the worker can free `dext`) and settle the shortfall once. Over-ticking is
  fine: the 99% cap + monotone display clamp + exact upfront total absorb it.
- Block-hash-discovered groups (`--dedupe-options=partial`, off by default)
  aren't in the upfront total; each is added via `pdedupe_add_pushed_work()` at
  push time and the renderer clamps `total = max(upfront, pushed)`.
- `--progress=json` emits `work_done_bytes`/`work_total_bytes` **raw** (no
  monotone clamp — machine consumers want truth); `pdedupe_end()` emits one
  final dedupe record so the last line shows the settled `done == total`.
- **The pre-analysis scales with the new work, not the hashfile (#184, #270).**
  Both figures come from one `dbfile_count_dupe_work()` call — one query per
  pass yielding count *and* sum, since they group over the identical row set.
  Each query groups only the rows newer than `first_seq` and asks once per
  group whether an older member exists (the #265 loader shape; details and
  numbers in `hashfile-sqlite.md`). A first scan (`seq_lo == 0`)
  uses a form with no window and no probe, so it no longer pays for the
  incremental case. The #184 form (a `WHERE` on the group key) was 8.95 s ->
  0.10 s at 1% new, but 12.5 s at 100% new and it churned temp pages between.
- The group estimate is therefore **per-run**, not lifetime, which it always
  should have been — `pdd.done` counts groups deduped *this* run, so
  `max(estimate, queued)` was comparing a lifetime figure to a per-run one and
  the bar sat low (one new pair among four deduped read "1 / ~5 groups").
- `process_duplicates()` also skips the call entirely when `passes == 0`. That
  is **insurance, not the fix** — with the macros the no-op case already costs
  ~0.5 ms. It matters only if the search indexes are missing, since
  `dbfile_create_search_indexes()` is best-effort.
