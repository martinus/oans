---
paths:
  - "docs/man/**"
---

# The man page

- `make doc` builds the man page from `docs/man/oans.md`; needs `pandoc` (on
  `PATH` via `devenv.sh`). roff escapes `-` as `\-` in the generated `.8`.
- No pandoc (sandbox without GitHub): `make pandoc` fetches a pinned
  `pypandoc_binary` from PyPI into `.pandoc/`, which `make doc` uses (devenv
  uses `OANS_PANDOC_BIN`). It is pandoc 3.9, newer than the committed `.8`'s,
  so a full `make doc` reformats the whole file; expected.
- pandoc ≥3.9 collapses a *tight* multi-paragraph def-list item (e.g.
  `--hashfile`): put a blank line after the term and keep the guarding
  `<!-- -->`, for every multi-paragraph option.
