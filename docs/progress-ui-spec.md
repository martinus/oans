# Live progress UI — unification spec

Status: **draft for review** — no code written yet. This proposes one unified
on-screen layout for every phase of a run so the tool looks like a single tool
throughout, instead of the three different live displays it grows today.

## 1. Motivation

A run passes through several kinds of work, each of which currently draws its own
kind of screen:

- **scanning + hashing** — `pscan_*` in `progress.c`: N per-worker lines on top,
  then a 3-line totals block (files, bytes, listing status). *(You said you like
  this one best — it's the template.)*
- **block-level extent search** — `psearch_*`: a lone `[####    %      ]` bar on a
  single line, no worker lines, no ETA. (This work is folded into the dedupe
  stage here — see §4/§6 — so it stops being its own screen.)
- **dedupe** — `pdedupe_*`: reuses the scan area (worker lines + a 3-line block:
  groups, reclaimed, status).
- **finished** — the last totals block is left frozen in place as scrollback.

They share plumbing (the reserved screen area, the relative-redraw, `pscan_printf`
for messages) but differ in what they show and how progress/ETA are expressed. The
goal is one layout, one mental model, driven by a single stage indicator.

## 2. Target layout

Bottom-anchored live block, redrawn in place (~10 fps on a tty). Top-to-bottom:

```
… scrollback (completed messages, errors) …
  1  hashing  /home/u/media/movie.mkv              1.2 GiB/2.0 GiB (60.00%)
  2  hashing  /home/u/…/archive/backup-2021.tar     842 MiB/842 MiB (100.0%)
  3  mapping  /home/u/photos/IMG_8123.CR3           (size: 34.0 MiB)
  4  hashing  /home/u/…/repos/linux/mm/slub.c           18 KiB/18 KiB (100.0%)
  5  idle
  6  idle
  7  idle
  8  idle
                                                  ← one blank line
✔ scanning   ⠹ hashing   · dedupe   · done
hashing  [██████████████████████░░░░░░░░░░░░░░]  62%  ·  ETA ~1m20s
Hashing 12,340 / 20,000 files · 6.1 GiB / 9.8 GiB · 210 MiB/s
```

Structure, top to bottom:

1. **Worker lines** — one per worker slot, numbered `1`…`N`, showing what that
   worker is doing *right now*. No header line above them.
2. **One blank line.**
3. **Stage line** — the four words `scanning hashing dedupe done`, each prefixed
   by a spinner (running), a tick (finished), or a dim dot (not started).
4. **Progress bar + ETA** — for the current stage, prefixed by that stage's name.
5. **Detail line** — the concrete numbers for whatever is happening now.

Messages and errors are printed *above* the block (existing `pscan_printf`
behaviour) and scroll up into history; the block always stays at the bottom.

The block height is `N + 4` lines (N worker slots + blank + 3). The existing
relative-redraw already handles a variable line count, terminal scrolling, and
resize, so this needs no new cursor bookkeeping.

## 3. Worker lines (top region)

- **One line per worker slot**, in slot order. `pscan_claim_slot` /
  `pscan_slot_idle` already give a bounded, stable set of slots (bounded by pool
  size, not OS-thread churn).
- **Numbered, not the pid.** A **right-aligned 3-character** slot number (`  1`,
  ` 10`, `100`) = the slot's 1-based index in `pscan.threads`. No brackets. The
  pid (`tid`) stays in the struct for debugging but is never shown. *(Today the
  line prints `[<tid>]`.)*
- **Two spaces**, then the **status word with no colon**, then **two spaces**,
  then the path. The status word is left-padded to a constant width so the paths
  line up column-wise:

  ```
    1  hashing    /home/u/…/archive/backup-2021.tar   842 MiB/842 MiB (100.0%)
    2  mapping    /home/u/photos/IMG_8123.CR3         (size: 34.0 MiB)
    3  wait lock  /home/u/…/repos/linux/mm/slub.c     (size: 18.0 KiB)
    4  deduping   /home/u/…/media/movie.mkv           1.1 GiB/2.0 GiB (55.00%)
    5  idle
  ```

  Status words: `mapping`, `hashing`, `wait lock`, `commit`, `deduping`, `idle`
  (the current labels, minus their trailing `:`). Constant width = the widest of
  these (`wait lock`, 9).
- Per-status trailing content is unchanged from the current hashing screen:
  - `mapping` / `wait lock` / `commit` — `(size: <S>)`, before the first byte.
  - `hashing` / `deduping` — `<done>/<total> (<pct>%)`
  - `idle` — nothing after the word.
- Path ellipsizing, control-byte sanitising, and the 80-column fallback for
  zero-width ptys are kept exactly as they are (`ellipsize_path`,
  `sanitize_ctrl`). The metrics on the right stay visible; the path's middle is
  shortened to fit.

Errors continue to pile up at the top (they're `pscan_printf`'d into scrollback,
above the live block) — unchanged.

## 4. Stage line

```
✔ scanning   ⠹ hashing   · dedupe   · done
```

Four fixed words in run order. Each gets a **state glyph** in front:

| State       | Glyph (UTF-8)         | ASCII fallback |
|-------------|-----------------------|----------------|
| finished    | `✔` (green if colour) | `[x]`          |
| running     | spinner (animated)    | `-\|/` cycle   |
| not started | `·` (dim)             | ` ` (space)    |

- **Spinner**: braille cycle `⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏`, advanced one frame per redraw
  (~10 fps). A single global frame counter drives every running spinner so they
  animate in sync.
- The **currently-running** word(s) may be shown bold; finished words dim. Colour
  and bold are only emitted on a tty; see §8.
- **UTF-8 detection**: if `nl_langinfo(CODESET)` isn't `UTF-8`, fall back to the
  ASCII glyphs above and an ASCII progress bar (§5). (The tool already prints `…`
  unconditionally, so UTF-8 is the common case; the fallback is insurance.)

### Which stages run when

The four stages are **monotonic**: a glyph only ever advances
`not-started → running → finished`, never back. Two facts about the real control
flow shape the transitions:

1. **scanning ∥ hashing.** The directory walk (listing) and the csum pool run
   concurrently inside `scan_files()`. Listing finishes first
   (`pscan_finish_listing`); hashing finishes when the last discovered file is
   csummed. So early on, **both `scanning` and `hashing` spin**; then `scanning`
   ticks while `hashing` keeps spinning.
2. **the block-level extent search is part of dedupe, not a stage of its own.**
   `process_duplicates()` loops over generation passes; each pass does: load
   identical files → dedupe whole-file dups → (optionally) load extent hashes +
   **block-level extent search** (`find_additional_dedupe` / `psearch`) → dedupe
   extents. That searching recurs *inside* the dedupe phase and interleaves with
   the actual deduping, so it can't be a clean separate stage before dedupe.
   Instead it shows up as a sub-activity on the dedupe **detail line** (§6) while
   the `dedupe` spinner stays lit.

Transitions:

- `scanning`: running at launch; ✔ at `pscan_finish_listing`.
- `hashing`: running at launch; ✔ when `scan_files` drains the csum pool.
- `dedupe`: running from `pdedupe_begin` (its analysis/search/dedupe passes) to
  `pdedupe_end`; ✔ at the end.
- `done`: ✔ once the whole run finishes (final screen, §9).

## 5. Progress bar + ETA

One bar for the **current stage** = the right-most stage currently spinning
(work advances left→right, so the right-most active stage is the live edge). During
the scan∥hash overlap that's `hashing`; afterwards `dedupe`.

```
hashing  [██████████████████████░░░░░░░░░░░░░░]  62%  ·  ETA ~1m20s
```

- **Constant-width stage-name prefix** so the bar never shifts horizontally as the
  stage changes: the stage name is left-padded to the width of the longest name
  (`scanning`, 8) plus two spaces, then the bar. `dedupe` renders as
  `dedupe  ` (padded), so the `[` always lands in the same column.
- **Longer bar**: fixed inner width of **40** cells (was ~25), `█` filled / `░`
  empty; ASCII fallback `[###------]`. A fractional trailing cell uses the 1/8
  block ramp `▏▎▍▌▋▊▉` on UTF-8 for smoothness (optional; plain `█`/`░` is fine
  for v1). On a very narrow terminal the width shrinks to fit, but the prefix
  stays constant.
- Percentage always shown, right after the bar. ETA shown only once measurable
  (`> 1s` elapsed and non-zero progress), formatted with the existing
  `human_duration`. Percent/ETA sit to the *right* of the bar, so their varying
  length never moves the bar.

Per-stage bar semantics and ETA source:

| Stage     | Fraction                              | ETA source                                   |
|-----------|---------------------------------------|----------------------------------------------|
| scanning  | *indeterminate* until listing done¹   | none (unbounded until listing completes)     |
| hashing   | weighted work: `(bytes + w·files) / total` | `scan_eta_seconds()` (existing weighted ETA) |
| dedupe    | `done / max(estimate, queued)`, capped 99%² | `(total-done)/(done/elapsed)` (existing) |

¹ While listing is still in progress the *hashing* total isn't known, so the bar
is **indeterminate**: render a small bouncing/pulsing fill (a moving lit segment)
rather than a bogus percentage. As soon as `pscan_finish_listing` fires, the total
is known and the bar switches to the determinate weighted form with an ETA. This
is the one visual state not present today and the only genuinely new rendering
code; everything else reuses existing counters.

² The dedupe total is deliberately fuzzy (`dbfile_count_dupe_groups()` estimate);
keep the current `~` marker and the 99% cap so the bar never sits at 100% while
work continues.

## 6. Detail line

One line, current stage's concrete numbers. Reuses today's strings, minus the
`\t`-prefixed multi-line blocks (those collapse into this single line):

- **scanning** (before listing done): `Scanning: 48,120 files found · 12,340 need hashing`
- **hashing**: `Hashing 12,340 / 20,000 files · 6.1 GiB / 9.8 GiB · 210 MiB/s`
- **dedupe**: `Deduping 512 / ~1,900 groups · batch 2/5 · reclaimed 3.4 GiB`
  - while the per-pass block-level extent search runs, it appends here so the work
    is visible without a separate stage:
    `Deduping 512 / ~1,900 groups · searching extents 4.2k/9.8k · reclaimed 3.4 GiB`

## 7. Non-tty, quiet, verbose

- **Not a tty**: no ANSI, no cursor moves, no spinner. Emit an append-only log: a
  one-line marker when each stage starts and finishes, plus a throttled progress
  line (e.g. every ~5 s or every +10%). This replaces today's 1 Hz raw reprint and
  is friendlier to `tee`/journald.
- **`-q` (quiet)**: no live UI at all (unchanged). Counters still accumulate for
  the final summary and `--json`.
- **`-v` (verbose)**: per-item notices instead of the live block (unchanged); the
  block would fight the redraws. Stage-transition markers are still fine to print.
- **Colour**: only on a tty and only when `NO_COLOR` is unset (respect the
  convention). All colour is optional decoration — the layout must be fully
  legible in monochrome.

## 8. Finished screen

When the run ends, the live block is wiped and replaced in place by the final
state: the stage line fully ticked, then a compact summary.

```
✔ scanning   ✔ hashing   ✔ dedupe   ✔ done
Scanned 20,000 files · 9.8 GiB · deduped 1,900 groups · reclaimed 3.4 GiB · 4m02s
```

This unifies the current two different endings (frozen scan totals vs. frozen
dedupe totals) into one line, fed by `pdedupe_counters()` + the latched scan
counts. The existing "Reclaimed is a logical figure" caveat still applies to the
reclaimed number; wording unchanged from today.

## 9. Edge cases

- **Tiny / warm no-op run**: stages flick to ✔ almost immediately; the final line
  still renders. No special-casing.
- **Nothing to hash** (all up to date): `hashing` bar reaches 100% instantly; fine.
- **Empty extent search** (`search_total == 0`): already guarded (#348); the
  dedupe detail line shows the search reaching done without NaN.
- **Terminal resize / scroll**: handled by the existing relative-redraw + wipe;
  the taller/shorter block self-corrects each frame.
- **More concurrent items than slots**: impossible — slots are claimed per work
  item and bounded by pool size (see `pscan_claim_slot`).

## 10. Implementation map (for later, not part of this review)

All in `src/progress.{c,h}`; call sites in `oans.c`, `find_dupes.c`,
`run_dedupe.c` already exist and mostly stay put.

- Add a single `enum stage { STAGE_SCAN, STAGE_HASH, STAGE_DEDUPE, STAGE_DONE }`
  with a per-stage state (`pending/running/done`) array, advanced by the existing
  lifecycle hooks (`pscan_finish_listing`, `pscan_join`, `pdedupe_begin`,
  `pdedupe_end`). No new threads — the existing single printer thread renders all
  of it.
- Fold `print_scan_progress` / `print_dedupe_progress` / the standalone
  `psearch_progress_thread` bar into **one** renderer: worker lines, blank, stage
  line, bar, detail line. `psearch` stops drawing its own bar and just feeds
  `search_processed/total` into the shared renderer as the dedupe detail-line
  sub-activity (it already does this when `pdd.phase` is set — extend it to
  always).
- Reuse: `scan_eta_seconds`, `human_size`, `human_duration`, `ellipsize_path`,
  `sanitize_ctrl`, `pscan_printf`, the relative-redraw (`progress_home` /
  `progress_wipe` / `drawn_lines`).
- New rendering: the stage line, the block progress bar (with constant-width stage
  prefix), and the indeterminate (pulsing) bar for the pre-listing-complete window.

## 11. Questions for you

1. **Spinner/tick glyphs**: braille spinner + `✔` as specced, or do you want a
   specific set?
2. **Colour**: use green ticks / dim pending, or stay monochrome?
