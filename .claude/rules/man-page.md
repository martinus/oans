---
paths:
  - "docs/man/**"
---

# The man page

- **`make doc`** regenerates the man page from `docs/man/oans.md` and needs
  `pandoc` (on `PATH` via `devenv.sh`). roff escapes `-` as `\-`, so grep the
  generated `.8` accordingly.
  - **No pandoc? `make pandoc`** fetches a pinned prebuilt one into `.pandoc/`
    from PyPI (`pypandoc_binary`) — for sandboxes where GitHub is blocked but
    PyPI works; `make doc` picks it up. It's the sandbox counterpart to devenv's
    `OANS_PANDOC_BIN`. That pandoc (3.9) is newer than the committed `.8`'s, so a
    full `make doc` reformats the whole file — expected, not a bug.
  - **Multi-paragraph def-list gotcha:** pandoc ≥3.9 collapses *tight* def-list
    items (e.g. `--hashfile`) into one blob; a blank line after the term makes it
    *loose* and restores the breaks (a no-op under older pandoc). Keep the
    guarding `<!-- -->`; repeat for any new multi-paragraph option.
