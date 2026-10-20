---
paths:
  - "src/glob.{c,h}"
  - "tests/unit/test_glob.c"
  - "tests/integration/test_exclude.py"
---

# --exclude matching (src/glob.{c,h})

- `.gitignore` syntax, not `fnmatch`: bare name = basename at any depth,
  leading `/` = absolute anchor, interior `/` = any depth, trailing `/` = dirs
  only, `*` stops at `/`, `**` does not. No negation (`!`), deliberately.
- 1.6.0 broke compatibility (#147: full-path `fnmatch` made
  `--exclude node_modules` match nothing). Stored patterns replay under the new
  semantics; no syntax versioning.
- All patterns join into **one** PCRE2 `GRegex` (cost independent of count);
  exact absolute paths skip it via a hash lookup.
- **Every pattern compiles twice (#283)**, UTF-8 and `G_REGEX_RAW`; `raw_path()`
  picks per name (PCRE2 leaves invalid UTF-8 undefined: `*.iso` missed
  `caf\xe9.iso`). Don't translate `?`/classes to byte sequences: in non-UTF-8
  names a byte is a character. Both use `DOTALL` (`**` stopped at `\n`) and
  `DOLLAR_ENDONLY` (`foo` excluded `foo\n`). A class never matches `/`; POSIX
  classes (`[[:digit:]]`) pass through. Property generator draws `\n`, `\xe9`.
- **Compile once in `filescan_init()`** on the main thread before walkers
  exist; read-only afterwards, so no lock. Never lazy or later.
- Hashfile and `-wal`/`-shm` go in via `add_exclude_path()` (literal), never as
  globs: `*` or `[` in that path must not be a wildcard.
- Per-pattern match counts back the "matched nothing" warning;
  `glob_set_stat()` enumerates **user** patterns only.
