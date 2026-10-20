---
paths:
  - "src/glob.{c,h}"
  - "tests/unit/test_glob.c"
  - "tests/integration/test_exclude.py"
---

# --exclude matching (src/glob.{c,h})

`.gitignore` syntax — the one git/ripgrep/fd use — not POSIX `fnmatch`. Bare
name = basename at any depth, leading `/` = absolute anchor, interior `/` = any
depth, trailing `/` = directories only, `*` stops at `/`, `**` does not.
Negation (`!`) is deliberately unsupported.

- **Breaking change in 1.6.0.** Patterns used to be `fnmatch`'d against the full
  path with relative ones resolved against the cwd, so `--exclude node_modules`
  matched at most one literal directory — usually nothing, silently (#147). Old
  patterns stored in a hashfile are replayed under the *new* semantics; that was
  a deliberate call (no syntax versioning) so there is only ever one meaning.
- Patterns compile to PCRE2 fragments joined into **one** combined `GRegex`, so
  match cost is independent of pattern count. `GRegex` is PCRE2 and GLib is
  already linked — no new dependency. Exact absolute paths skip the regex via a
  hash lookup.
- **Every pattern is compiled twice (#283)**: for UTF-8, where `?` and a class
  take one character, and `G_REGEX_RAW`, over bytes. `raw_path()` picks per
  name: PCRE2 leaves matching invalid UTF-8 undefined, and in practice no
  wildcard crossed such a byte, so `*.iso` silently missed a Latin-1
  `caf\xe9.iso`. In the names that are not UTF-8 - Latin-1, mostly - a byte
  *is* a character, which is why that split beats translating `?` and classes
  into byte sequences. Both get `DOTALL` (a name may hold a newline, and `**`
  stopped at one, letting a crafted name out from under an exclude) and
  `DOLLAR_ENDONLY` (`foo` excluded `foo\n`). A class never matches `/`, and
  POSIX classes (`[[:digit:]]`) pass through instead of being escaped into a
  class of their letters. The property path generator draws `\n` and `\xe9`
  for this; with the old code two properties fail on their own.
- **Compile once in `filescan_init()`**, on the main thread before any walker
  exists; the set is read-only afterwards so walkers need no lock. Don't move
  the compile later or make it lazy.
- The hashfile and its `-wal`/`-shm` sidecars are added with
  `add_exclude_path()` (literal), not as globs — a `*` or `[` in the hashfile's
  own path must not become a wildcard.
- Per-pattern match counts back the "matched nothing" warning; `glob_set_stat()`
  enumerates **user** patterns only, skipping the internal ones.
