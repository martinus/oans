---
paths:
  - "src/interrupt.{c,h}"
  - "src/file_scan.c"
  - "src/csum.c"
  - "src/run_dedupe.c"
  - "tests/unit/test_interrupt.c"
  - "tests/integration/test_signal_flush.py"
---

# SIGINT/SIGTERM flush the batch (#201)

`src/interrupt.{c,h}`. The handler **only sets a flag** (SQLite and GLib are not
async-signal-safe); every loop that owns state polls it and unwinds through its
ordinary exit path, and `filescan_free()` → `scan_writer_close()` already
commits the batch on the way out. So nothing new is made durable — the loops
just have to *reach* it. Measured on a 3000-file tree, SIGINT at 0.5 s:
v1.10.1 persisted **0** files, this persists ~975, in ~70 ms.

- **The flag is a lock-free `_Atomic int`, not `volatile sig_atomic_t`.** The
  readers are on *other* threads (csum workers, walkers), and `volatile` orders
  nothing across threads — ThreadSanitizer flags it, and is right to. C11 lets a
  handler touch a lock-free atomic, which is what makes it both race-free and
  async-signal-safe; a `static_assert` on `ATOMIC_INT_LOCK_FREE` keeps that
  true. Relaxed on both sides: the flag carries no data.
- `sigaction` with **`SA_RESTART`** (a walker blocked in `read()` resumes, so no
  error path has to learn about EINTR) and **`SA_RESETHAND`** — the kernel puts
  the default action back on delivery, so the second Ctrl-C kills with no work
  in the handler. Verified by watching `SigCgt` in `/proc/PID/status` drop the
  SIGINT bit.
- **Four checkpoints, one per loop that would otherwise run to completion:** the
  walkers still call `dirq_finished()` for every directory they skip (leaving
  `walk_dir_pending` non-zero means `WALK_STOP` is never pushed and the consumer
  blocks forever); the consumer stops calling `__scan_file`; the csum workers
  *drop* queued files rather than hash them (the queue holds thousands — working
  through it is not a shutdown); `csum_whole_file` abandons the file it is on
  after writing an off-interval checkpoint, so a 1 TiB file costs one buffer of
  latency instead of hours.
  - **That was not true until #281.** The `interrupted()` check sat below the
    checkpoint-interval test, so a worker saw Ctrl-C once per GiB, never
    without a hashfile, and the off-interval checkpoint could not fire at all
    (dead since #212). Measured before: a 4 GiB file read 3,895 MiB past the
    SIGINT without a hashfile. It is checked on every pass now, and a lost
    batch (#274) stops the file the same way.
  - **Opens that only probe use `O_NONBLOCK`.** A FIFO named as a root made
    `storage_detect()`'s `open()` wait for a writer, and `SA_RESTART`
    restarted it after the first Ctrl-C.
- **The dedupe phase stops admitting batches, and workers skip the groups
  still queued.** No new drain points: a group already running finishes
  (`FIDEDUPERANGE` is atomic), and in-flight batches reap normally. Working
  through the two in-flight batches took minutes on a big tree, past
  `oans@.service`'s stop timeout, so the unit was SIGKILLed and marked
  failed. A batch with a skipped group sets `cut_short`, and from its reap on
  **no batch moves the watermark** - a later one that finished would
  otherwise jump `dedupe_seq` past the skipped generation. Pinned by
  `test_signal_flush.py::…_skips_the_queued_groups`.
- **An interrupt during the *scan* skips the dedupe phase outright**
  (`process_duplicates` returns right after the "Hashfile written" line), and
  the run does not `dbfile_maybe_vacuum()` on the way out. The batch-loop check
  above is too late: everything *before* it still ran — the deleted-file prune,
  the find-dupes index build, the ~9 s group analysis — and then a VACUUM that
  rewrites the whole file and cannot itself be interrupted. It deduped nothing
  (the loop broke at once), so the only effect was a Ctrl-C that looked ignored
  for minutes and then printed `Nothing to deduplicate`.
  - **Wiping the progress block is part of it.** The scan hands its block to
    the dedupe phase (`pscan_join(continues=true)`), so `dedupe_live` must also
    test `!interrupted()` — without it the early return strands the worker rows
    on screen for the rest of the session, which is #179 from the other side.
    Pinned by `test_progress_tty.py::…_an_interrupted_scan_leaves_nothing_behind`.
- **An interrupted run is not written to `run_history`** — it would read as a
  complete scan of the tree, skip counters and all. Exit is `128 + signo`, set
  last and only over a success.
- **Test hooks `DUPEREMOVE_INTERRUPT_AFTER=N` / `_AFTER_BATCHES=N` /
  `DUPEREMOVE_INTERRUPT_SIGNAL=TERM`** count files that have **finished
  hashing**, not files queued — so when the signal lands, N files are already in
  the open batch and the durability assertion is exact. Counting queued files
  let a loaded CI runner finish *none* of them, and the test asserted `> 0`
  against 0. Two more traps in that hook: it is ticked from the csum workers, so
  the counter is atomic and only the thread that crosses the limit raises
  (`==`, not `>=` — with `SA_RESETHAND` a second `raise()` kills outright);
  a worker past the limit waits until the signal has landed, because a loaded
  CI runner once paused the raising thread between the increment and
  `raise()`, and the other workers hashed all 300 files (#266); and
  the env is read once in `interrupt_install()` on the main thread, since a lazy
  first-tick read races between workers. They raise the *real* signal at a named
  point,
  the same way `DUPEREMOVE_CHECKPOINT_STOP` does for #159. A `sleep N; kill`
  race needs a tree too big for the suite (~500 MB/s of scan) and still
  coin-flips on faster storage; `test_signal_flush.py` keeps one real-signal
  test, which skips rather than fails if the scan won the race.
