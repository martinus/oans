---
paths:
  - "src/util.{c,h}"
  - "src/progress.c"
  - "scripts/lint-escape.py"
  - "scripts/test_lint_escape.py"
  - "tests/integration/test_escape_names.py"
---

# File names are untrusted input (#202)

Anyone who can create a file inside a scanned tree picks bytes oans writes to
the admin's terminal, so **every path oans prints goes through
`sanitize_ctrl()`/`path_for_display()`** (`src/util.c`) — C0 controls, DEL and
the UTF-8 C1 controls become `\t`/`\n`/`\r`/`\xNN`. A `\r` in a name rewrites
the line above it (the summary, a progress row); `ESC[2J` clears the screen.

- **`make lint` enforces it — `scripts/lint-escape.py`.** The rule is otherwise
  prose, and prose rots: it was violated **eleven** times in the commit that
  introduced it, three found by review and eight more by the lint. It flags a
  path-shaped identifier passed to a printf-family macro; waive with
  `escape-ok: <why>` (the only legitimate reason so far is the hashfile path,
  which is oans's own argument rather than something found in a walk). Modelled
  on `lint-longpath.py`, and wired into the same `make lint` CI job.
  - **The lint could not see a path passed as the last argument (#276)**, the
    most common place for one: its lookahead wanted a `,` or `)` after the
    name, and the argument text it searched had the closing `)` cut off. Four
    dedupe-worker messages and a resume notice printed scanned names raw
    behind it. `scripts/test_lint_escape.py` runs in `make lint` and pins
    each argument position, so a lint that cannot fail now fails.
- **Escape at the print site, never in the stored string.** The path is what
  oans opens, stats and stores; only the display copy is escaped. The idiom is
  `declare_display_path(disp, p);` then print `disp` — a *declaration* macro, in
  the shape of `declare_alloc_tracking()`. It cannot be a statement expression
  (`({ ... })` ends the cleanup scope, so the string is freed before `printf`
  reads it) and it cannot use a fixed thread-local buffer like `pretty_size()`
  does, because a path may exceed `PATH_MAX` (#117) and a truncated one names a
  different file.
- **On the hot path, escape inside the branch that prints, under the same guard
  the print macro has.** `check_file()` and `is_excluded()` run per directory
  entry, so their `-v` skip messages sit inside `if (verbose)` — `vprintf`'s own
  guard is too late, the allocation would already have happened. `probe_fs()`
  runs per root, so it escapes once at the top.
- **One classifier, many renderings.** `ctrl_seq_len()` is the single definition
  of *which* bytes are dangerous; `sanitize_ctrl()` spells them `\xNN` and
  `print_json_str()` spells them `\u00NN`. Adding U+2028 or another C1 encoding
  is then one edit, not two that can drift.
- Every real name takes the `has_ctrl()` fast path — one exact-sized `strdup`
  instead of a 4x over-allocation and a per-byte loop — and the progress row
  skips the copy entirely. Worst-case expansion is `SANITIZE_CTRL_MAX` (4) bytes
  per input byte; `sanitize_ctrl()` truncates at the last *complete* escape.
- Pinned by `tests/integration/test_escape_names.py` (non-tty, asserts no raw
  `\x1b`/`\r`/`\x07`/`\x7f` reaches either stream in any report mode) and
  `test_progress_tty.py::test_a_crafted_name_cannot_inject_ansi_into_the_block`.
  A test that only checks a plain name proves nothing — assert on the *bytes*,
  with `text=False`.
