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

`-d` runs **one persistent pool** for the phase; the main thread is a bounded
producer loading batch *i+1* while *i* dedupes; no drain between batches or
passes. `run_dedupe.c` (`dedupe_phase_begin/end`, `dedupe_begin_batch`,
`dedupe_push`, `dedupe_seal_batch`), driven by `stream_duplicates()`
(`oans.c`). No `-d`: sequential `report_duplicates()`.

- **#288:** a failed load still dedupes what loaded, but `load_failed()` caps
  `dedupe_advance_seq()` at the window's lower bound (0 on a first scan, so a
  flag plus a value) and the run exits 1.
- **`DEDUPE_MAX_INFLIGHT` (2) batches max**; `dedupe_await_slot()` blocks.
- **Reap FIFO.** `dedupe_seq` advances to `seq_hi` only when this and all
  earlier batches finished (`dedupe_advance_seq`, under `dbfile_lock`), so on
  Ctrl+C it covers only finished generations. Empty windows advance it too.
- **Filerec lifetime = batch refs** (`struct filerec.refs`, not `fd_refs`):
  one per loaded filerec, taken in `push_results()`, dropped in `free_batch`.
  **Only the producer thread gets/puts/creates/finds**, so `filerec_by_fileid`
  and `filerec_head` need no lock; workers read `filerec` fields and touch
  their batch's results tree under `mutex`. **Never free a filerec a live
  batch references**; `free_all_filerecs()` is teardown-only.
- Producer loads on its own read connection (`dbfile_open_handle`). In-memory
  (no `--hashfile`, no WAL) it reuses the global handle under `dbfile_lock()`
  (`inmem`).
- **Stage 2.1:** `GET_DUPLICATE_EXTENTS` excludes whole-file members via
  `FILEDUP_MEMBER`, not by relying on deleted rows, so passes need no barrier
  and results trees hold disjoint filerecs. **Keep it a correlated probe**: a
  `group by digest, size` CTE took 21.7 s vs 0.2 s per load on 3.5M files.
  Pinned by `test_extent_order_independent.py`, `test_streaming_dedupe.py`.
- **A pushed dext belongs to the worker**, which may free it at once. Read
  `dext_work()` etc. **before** `g_thread_pool_push()`; `push_results` asserts
  sane per-group work.
- **#227:** `--dedupe-options=partial` calls `dedupe_drain()` before
  `dedupe_begin_batch()` (`find_additional_dedupe()` walks the global filerec
  list). Default mode keeps full overlap; add no other drain points.
  - **No reap while a batch is open**: between load and push its groups (e.g.
    the reloaded min-id anchor) live only on refs a reap drops (heap UAF).
    `free_batch()` asserts `open_batch == NULL`.
  - `DUPEREMOVE_DEDUPE_DELAY_MS` keeps a batch in flight during the next load
    (`test_partial_cross_window.py`).
  - `filerec_free_unreferenced()` after the push frees block-loader filerecs
    in no group; else every later window re-searches them
    (`test_partial_stale_filerecs.py`).
- **`find_additional_dedupe()` waits for its own workers** (counter/cond in
  `find_dupes.c`) before the caller reaps. Not via `psearch_join()`, which
  no-ops in this phase (#123). `free_batch()` asserts `extents_search_idle()`;
  `DUPEREMOVE_SEARCH_DELAY_MS` forces the race
  (`test_partial_search_waits_for_its_workers`).
- **#277:** `g_thread_pool_push()` errors only on a failed thread start and
  still queues the item (reading it as "not queued" freed a held item and
  double-counted). Push only via `pool_push()` (`src/threads.c`): warn
  once, fatal only if the pool has no thread. `DUPEREMOVE_POOL_SPAWN_FAIL`
  injects it; `test_pool_spawn_fail.py` uses `dm(..., timeout=120)` (old bug
  hangs). Real triggers: `oans@` TasksMax, `RLIMIT_NPROC`.
- **Run `make integration-valgrind` before a PR here** (`valgrind.md`, PR
  #105). It missed the pushed-dext race; review lifetimes by hand.
