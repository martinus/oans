---
paths:
  - "src/util.{c,h}"
  - "src/progress.c"
  - "scripts/lint-escape.py"
  - "scripts/test_lint_escape.py"
  - "tests/integration/test_escape_names.py"
---

# File names are untrusted input (#202)

- **Every printed path goes through `sanitize_ctrl()`/`path_for_display()`**
  (`src/util.c`): C0, DEL, UTF-8 C1 become `\t`/`\n`/`\r`/`\xNN`. A `\r`
  rewrites the line above; `ESC[2J` clears the screen.
- `make lint` runs `scripts/lint-escape.py` (like `lint-longpath.py`): flags a
  path-shaped identifier passed to a printf-family macro. Waive with
  `escape-ok: <why>`; only valid case so far is the hashfile path.
  `scripts/test_lint_escape.py` pins every argument position (the last one
  was missed, #276).
- **Escape at the print site, never the stored string**:
  `declare_display_path(disp, p);` then print `disp` (a declaration macro like
  `declare_alloc_tracking()`). Not a statement expression: `({ ... })` frees
  the string before `printf`. Not a thread-local buffer like `pretty_size()`:
  paths exceed `PATH_MAX` (#117), and truncated names another file.
- **Hot path: escape inside the printing branch.** `check_file()` and
  `is_excluded()` escape their `-v` messages inside `if (verbose)`
  (`vprintf`'s guard is too late); `probe_fs()` escapes once at the top.
- `ctrl_seq_len()` alone defines dangerous bytes; `sanitize_ctrl()` renders
  `\xNN`, `print_json_str()` `\u00NN`. Add encodings (U+2028, C1) there only.
- Clean names take the `has_ctrl()` fast path (exact `strdup`); the progress
  row skips the copy. Worst case `SANITIZE_CTRL_MAX` (4) bytes per byte;
  `sanitize_ctrl()` truncates at the last complete escape.
- Pinned by `tests/integration/test_escape_names.py` (no raw
  `\x1b`/`\r`/`\x07`/`\x7f` on either stream, any report mode) and
  `test_progress_tty.py::test_a_crafted_name_cannot_inject_ansi_into_the_block`.
  Assert on bytes (`text=False`) of a crafted name.
