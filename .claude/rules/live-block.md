---
paths:
  - "src/progress.{c,h}"
  - "src/debug.h"
  - "tests/integration/test_progress_tty.py"
---

# Nothing prints past the live block (#179)

The block is redrawn by moving the cursor up `drawn_lines`, so **every byte
written while one is on screen must go through `progress_printf()`** — i.e. the
`dprintf`/`vprintf`/`qprintf`/`eprintf` macros, never a bare `printf`. The
mechanism and the failure mode are documented at the definition in `progress.c`;
what belongs here is when it bites:

- **"A block is on screen" ≠ "a printer thread is running."** The scan hands its
  block straight to the dedupe phase (`pscan_join(continues=true)` …
  `pdedupe_begin()`) with no printer alive across the gap, so `progress_printf()`
  routes on `printer || block_live()`. Keying it on the printer alone was #179:
  one stranded worker row per unrouted line, *and* the scan-skip report silently
  erased on every run that had one.
- **Route whole lines.** One call is one erase/print/redraw cycle, so a message
  assembled from several calls redraws the block between the pieces —
  `report_scan_skips()` builds its whole report in a `GString` first.
- Non-tty output is untouched (`block_live()` is gated on `tty`), which is also
  why the plain integration suite can't see any of this. The regression test
  drives a real pty and replays the stream through a small ANSI emulator:
  `tests/integration/test_progress_tty.py`. Its invariant — *no worker-slot row
  may survive the end of a run* — catches the whole class, not just this site.
- **Routing honors the caller's stream (#203).** `print_above_block()` takes the
  `FILE *` and writes the message there, so an `eprintf` reaches stderr whether
  or not a block was up (it used to always land on stdout, and `2>errors.log`
  lost exactly the errors raised mid-run). The block itself is still stdout-only,
  so the erase must be `fflush`ed before the message and the message flushed
  before the redraw — otherwise the two streams' buffers interleave and the
  message lands inside the block. Don't drop those flushes.
  - Consequence for `--progress=json`: the JSON stream is **stderr**, so
    diagnostics share it. A record is formatted into one buffer and written
    once (#286): made of several `fprintf()`s on unbuffered stderr, a
    worker's `eprintf()` could land in the middle of it.
- **Redraw only a block that is on screen (#286).** `print_above_block()`
  redrew after every routed message, so into a file or a pipe every `-v`
  message was followed by a block dump (163 lines for 30 files), and while the
  partial-mode search printer ran it redrew the long-gone scan block, which then
  sat under every later message. It redraws only when `block_live()` held
  before the message.
- **Every row is cut to the terminal, and the block to its height (#286).**
  Only worker rows were fitted; the dedupe detail line runs to ~96 columns
  between batches and the bar line with an ETA to ~66, and a wrapped row, like
  a block taller than the terminal, makes `drawn_lines` count too few and
  strands one row per redraw. `print_progress()` renders into a buffer and
  `put_fitted()` cuts every line by visible columns (escape sequences are free,
  a UTF-8 glyph is one); workers past the height fold into "… N more".
  `test_a_narrow_short_terminal_strands_nothing` runs two geometries because
  each hides the other's bug.
  - **Render into `R`, never into a swapped `stdout`.** The first version set
    the global `stdout` to the buffer for the render: other threads print to
    `stdout` without the mutex, so ThreadSanitizer flagged every `-v` message
    as a race, and a message could have landed in the block buffer.
  - **`progress_printf()` preserves `errno`.** Callers print an error and
    then count it by `errno` (`filescan_count_errno_skip(errno)` after an
    `eprintf()`, in a dozen places); the redraw's `open_memstream()`/`free()`
    changed it, and under TSan a permission error was counted as
    "unreadable". Saving it in the one routing function covers every caller.
- **Whoever hides the cursor, `progress_abandon()` shows it (#286).** The scan
  leaves its block and the hidden cursor for the dedupe phase, and a failed
  scan or a signal in the gap never got there, so the shell was left without
  a cursor. `main()` calls `progress_abandon()` on the way out; after a phase
  that ended its block it writes nothing. That was already true for anything printed outside the
    printer's lifetime; a consumer must skip lines that don't parse.
