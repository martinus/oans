---
paths:
  - "src/progress.{c,h}"
  - "src/debug.h"
  - "tests/integration/test_progress_tty.py"
---

# Nothing prints past the live block (#179)

The redraw moves up `drawn_lines`, so **all output while a block is up goes
through `progress_printf()`** (`dprintf`/`vprintf`/`qprintf`/`eprintf`), never
bare `printf`. Mechanism: see the definition in `progress.c`.

- **Route on `printer || block_live()`** (#179): `pscan_join(continues=true)`
  … `pdedupe_begin()` keeps a block up with no printer.
- **Route whole lines** (one call = one redraw); `report_scan_skips()` builds
  a `GString` first.
- Non-tty is untouched (`block_live()` needs `tty`).
  `tests/integration/test_progress_tty.py` replays a pty; invariant: no
  worker-slot row survives a run.
- **#203:** `print_above_block()` writes to the caller's `FILE *`; the block
  is stdout-only. **Keep both `fflush`es** (erase before message, message
  before redraw) or streams interleave.
  - `--progress=json` shares **stderr** with diagnostics; consumers skip
    unparsable lines. Write each record as one buffer, not several
    `fprintf()`s (#286), or an `eprintf()` lands inside it.
- **#286:** redraw only if `block_live()` held before the message (pipes got a
  block dump per `-v` line; the search printer redrew a dead block).
- **#286:** `print_progress()` renders into a buffer; `put_fitted()` cuts each
  line to visible columns (escapes 0, UTF-8 glyph 1) and excess workers fold
  into "… N more", since wrapped rows make `drawn_lines` undercount.
  `test_a_narrow_short_terminal_strands_nothing` needs both geometries.
  - **Render into `R`, never a swapped `stdout`** (unlocked writers; TSan).
  - **`progress_printf()` preserves `errno`**: callers count by it
    (`filescan_count_errno_skip(errno)`), and `open_memstream()`/`free()`
    clobbered it.
- **#286:** `main()` calls `progress_abandon()` on exit to restore the cursor a
  failed scan or signal left hidden; it writes nothing after a finished block.
