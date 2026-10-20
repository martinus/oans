---
paths:
  - "src/run_dedupe.{c,h}"
  - "src/oans.c"
  - "src/filerec.{c,h}"
  - "src/find_dupes.c"
  - "src/threads.{c,h}"
  - "tests/integration/test_streaming_dedupe.py"
  - "tests/integration/test_partial_*.py"
  - "tests/integration/test_pool_spawn_fail.py"
---

# Streaming dedupe pipeline (Stage 2)

The dedupe phase (`-d`) runs **one persistent thread pool** for the whole phase;
the main thread is a **bounded producer** that loads generation batch *i+1* while
batch *i* dedupes. No pool drain between batches or between the whole-file and
extent passes. Lives in `run_dedupe.c` (`dedupe_phase_begin/end`,
`dedupe_begin_batch`/`dedupe_push`/`dedupe_seal_batch`) driven by
`stream_duplicates()` in `oans.c`. The report path (no `-d`) stays sequential
(`report_duplicates()`).

- **A batch whose load failed does not move the watermark (#288).** It still
  reaps and dedupes what was loaded, but the groups a failed loader never
  returned would otherwise wait until their files change: `load_failed()`
  caps `dedupe_advance_seq()` at the window's lower bound, and the run exits
  1. On a first scan that bound is 0, so the cap is a flag plus a value.
- **At most `DEDUPE_MAX_INFLIGHT` (2) batches in flight** — the RAM double
  buffer. `dedupe_await_slot()` blocks the producer until a slot frees.
- **Generation-ordered watermark.** Batches are reaped strictly in FIFO
  (generation) order; `dedupe_seq` only advances to a batch's `seq_hi` once that
  batch *and all earlier ones* are complete (`dedupe_advance_seq` callback, under
  `dbfile_lock`). This preserves the **Ctrl+C invariant**: on kill, `dedupe_seq`
  reflects only fully-processed generations. Empty windows still advance it.
- **Filerec lifetime = batch-held refs (NOT per-extent).** `struct filerec.refs`
  is a lifetime count separate from `fd_refs`. Each batch holds one ref per
  filerec it loaded (taken in `push_results`, released in `free_batch` at reap).
  **All get/put/new/find happen on the single producer thread**, so the filerec
  registry (`filerec_by_fileid` tree / `filerec_head`) needs **no lock** and
  workers never touch it — they only read `filerec` fields and mutate their
  batch's own results tree (under the global `mutex`). A cross-window anchor is
  referenced by two in-flight batches; each holds a ref, so it survives until
  both reap. **Never free filerecs while a batch referencing them is live**
  (`free_all_filerecs()` is teardown-only now; it's gone from between batches).
  This is a deliberate, safer alternative to the plan's per-extent/worker-release
  refcount — same lifetime guarantee, far less lock surface.
- **Producer DB handle.** The producer loads on a **separate read connection**
  (`dbfile_open_handle`) so it doesn't share a sqlite handle with the workers
  writing on the global handle (WAL: readers don't block the writer). The
  in-memory shared-cache db (no `--hashfile`) has no WAL, so there the producer
  reuses the global handle and serializes loads with `dbfile_lock()` (`inmem`).
- **Order-independence (Stage 2.1).** `GET_DUPLICATE_EXTENTS` excludes whole-file
  dup members *statically* (the `FILEDUP_MEMBER` predicate) instead of relying on
  the whole-file pass having deleted their extent rows first — the prerequisite
  for loading both passes without a barrier. As a bonus the two per-batch results
  trees reference **disjoint** filerec sets. Pinned by
  `test_extent_order_independent.py`; streaming by `test_streaming_dedupe.py`.
  **Keep it a correlated probe, not a materialized set:** a `group by digest,
  size` CTE here cost ~6 s per batch load on a 3.5M-file hashfile (the producer
  stalls at "loading duplicate extents" while the pool idles; measured 21.7 s →
  0.2 s per load after the switch).
- **A pushed dext belongs to the worker.** The moment `g_thread_pool_push()`
  hands a group to the pool, a worker can free it (an already-shared group is
  cleaned in microseconds) — the producer must capture anything it needs
  (`dext_work()` etc.) **before** the push. Reading after once fed
  `len * (0 - 1)` from a freed dext into the pushed-work total (frozen bar,
  multi-thousand-year ETA); `push_results` now asserts sane per-group work.
- **Partial mode drains — *before* `dedupe_begin_batch()`, not mid-load (#227).**
  `find_additional_dedupe()` walks the **global** filerec list, so with
  `--dedupe-options=partial` the producer calls `dedupe_drain()` — earlier
  batches reaped, filerecs freed — at the cost of cross-batch pipelining in that
  mode. Default mode keeps the full overlap; don't add other drain points.
  - **A reap may never land while a batch is open.** A batch takes its filerec
    refs in `push_results()`, so between a load and its push the open batch's
    groups point at filerecs kept alive only by the refs the reap drops — and a
    group spanning two windows is exactly that shape, since the anchor member
    (min id) is reloaded by every later window. The drain used to sit between
    the extent load and the push, which freed that anchor and made
    `push_results()` walk into it: a heap UAF that only showed up under
    `--dedupe-options=partial`, and only when the previous batch was still in
    flight. `free_batch()` now asserts `open_batch == NULL`, so putting a reap
    back in the middle of a load aborts at the violation instead of corrupting
    the heap.
  - `DUPEREMOVE_DEDUPE_DELAY_MS` holds each dedupe worker back so a batch is
    still in flight while the next one loads; on a test-sized tree the window
    never opens without it (`test_partial_cross_window.py`). Sibling of
    `DUPEREMOVE_SEARCH_DELAY_MS` below.
  - **The drain frees only what a batch holds.** The block-hash loader makes
    a filerec for every file with a duplicate block, and one the search
    matches to nothing is in no group, so no batch refs it. Those stayed on
    the list all run and every later window searched them again.
    `filerec_free_unreferenced()` drops them after the batch's push
    (`test_partial_stale_filerecs.py`).
- **The search must outlive nothing.** `find_additional_dedupe()` waits for
  every worker it pushed (its own counter/cond in `find_dupes.c`) before
  returning, because the caller reaps batches — freeing filerecs — right after.
  Don't route that wait through `psearch_join()`: that's a *progress* concern
  and no-ops during the dedupe phase, which was #123 (a UAF that reproduced in
  ~5% of memcheck runs). `free_batch()` asserts `extents_search_idle()`, and
  `DUPEREMOVE_SEARCH_DELAY_MS` makes the race deterministic for the regression
  test (`test_partial_search_waits_for_its_workers`).
- **A failed thread start is not a failed push (#277).** GLib's
  `g_thread_pool_push()` sets an error only when it could not start a new
  thread, and queues the item all the same. Both pools read it as "not
  queued": the search freed the item a worker still held and dropped its
  outstanding count twice, so a later wait returned early or hung for good,
  and the dedupe phase aborted. `pool_push()` (`src/threads.c`) is the one
  place that pushes now: a warning, printed once, and fatal only when the pool
  has no thread at all to run the item. `DUPEREMOVE_POOL_SPAWN_FAIL` reports
  the error after every real push; `test_pool_spawn_fail.py` passes `dm(...,
  timeout=120)`, because the old code's failure is a hang. TasksMax on the
  `oans@` units and `RLIMIT_NPROC` are the real triggers.
- **Valgrind is the gate** (`make integration-valgrind`): this is the UAF-prone
  area (see `valgrind.md` / PR #105). Run it before any PR here — but
  note it did **not** catch the pushed-dext race above (valgrind's serialization
  makes a fast worker win too rarely); producer-vs-worker lifetime rules need
  review, not just the suite.
