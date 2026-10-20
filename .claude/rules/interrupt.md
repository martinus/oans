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

- **The handler only sets a flag** (`src/interrupt.{c,h}`): SQLite and GLib
  are not async-signal-safe. Loops poll it and unwind normally;
  `filescan_free()` -> `scan_writer_close()` commits the batch.
- **The flag is a lock-free `_Atomic int`, not `volatile sig_atomic_t`**
  (cross-thread readers; TSan). `static_assert` on `ATOMIC_INT_LOCK_FREE`;
  relaxed ordering. `sigaction` with **`SA_RESTART`** (no EINTR handling) and
  **`SA_RESETHAND`** (second Ctrl-C kills).
- **Four checkpoints:** walkers still call `dirq_finished()` per skipped
  directory (else `walk_dir_pending` never reaches zero, `WALK_STOP` is never
  pushed, the consumer hangs); the consumer stops calling `__scan_file`; csum
  workers *drop* queued files; `csum_whole_file` abandons its file after an
  off-interval checkpoint.
  - Check `interrupted()` on every pass, not below the checkpoint-interval test
    (#281; that was once per GiB, never without a hashfile, and the
    off-interval checkpoint dead since #212). A lost batch (#274) stops the
    file the same way.
- **Probe-only opens use `O_NONBLOCK`**: a FIFO root hung `storage_detect()`'s
  `open()`, restarted by `SA_RESTART`.
- **Dedupe phase: admit no new batches, workers skip queued groups**; running
  groups finish (`FIDEDUPERANGE` is atomic), in-flight batches reap. No new
  drain points (draining overran `oans@.service`'s stop timeout). A batch
  with a skipped group sets `cut_short`; from its reap on **no batch moves
  the watermark**, or `dedupe_seq` skips a generation. Pinned by
  `test_signal_flush.py::…_skips_the_queued_groups`.
- **An interrupt during the scan skips the dedupe phase** (`process_duplicates`
  returns after "Hashfile written") and `dbfile_maybe_vacuum()`; otherwise
  prune, index build, group analysis and an uninterruptible VACUUM run first.
  `dedupe_live` must test `!interrupted()` or the handed-over block
  (`pscan_join(continues=true)`) strands worker rows (#179). Pinned by
  `test_progress_tty.py::…_an_interrupted_scan_leaves_nothing_behind`.
- **Interrupted runs are not written to `run_history`.** Exit `128 + signo`,
  set last and only over a success.
- **Hooks `DUPEREMOVE_INTERRUPT_AFTER=N` / `_AFTER_BATCHES=N` /
  `DUPEREMOVE_INTERRUPT_SIGNAL=TERM`** raise the real signal at a named point,
  like `DUPEREMOVE_CHECKPOINT_STOP` (#159):
  - count files that **finished hashing**, not queued, so N are in the batch;
  - atomic counter, only the crossing thread raises (`==`, not `>=`; a second
    `raise()` kills under `SA_RESETHAND`);
  - workers past the limit wait until the signal landed (#266);
  - env read once in `interrupt_install()` on the main thread (lazy read races).
- No `sleep N; kill` tests. `test_signal_flush.py` keeps one real-signal test
  that skips if the scan won the race.
