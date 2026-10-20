---
paths:
  - "tests/valgrind*"
  - "scripts/verify.sh"
  - "src/run_dedupe.c"
  - "src/dedupe.c"
---

# Valgrind

`verify.sh` runs the smoke. Manual, with the suppressions file (filters the one
library false-positive):

```sh
valgrind --leak-check=full --track-origins=yes \
    --suppressions=tests/valgrind.supp ./oans -rd --hashfile=/tmp/h.db <tree>
```

The suppressed noise is GLib/glibc **thread TLS** ("possibly lost" under
`pthread_create`→`_dl_allocate_tls`: GLib caches idle pool threads). Everything
else must be clean — `definitely lost` and any uninitialised-value /
invalid-access errors are real. (One fixed: `__dbfile_get_config` read an
unterminated UUID buffer; `get_config_text` buffers are raw `memcpy`, so anything
`strlen`'d must be zero-initialised.)

- **`make integration-valgrind`** runs the *whole* end-to-end suite under
  memcheck — each `oans` invocation via `tests/valgrind-wrap.sh` (set
  `DUPEREMOVE=` to it, `OANS_VG_LOGDIR=` for per-pid logs). Findings land in
  `.vglogs/`; a non-empty log fails the target. ~7× slower than plain
  `integration` (so opt-in, not in `check`), but it catches use-after-free /
  leaks the plain suite reads straight past — it found the `dbfile_prepare`
  recreate-path UAF (a rejected hashfile reopened into a by-value local, so the
  caller kept the closed handle; fixed by passing `sqlite3 **`). The CI
  `valgrind` job runs it (btrfs only — memory behaviour is fs-independent).
