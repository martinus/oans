---
paths:
  - "tests/valgrind*"
  - "scripts/verify.sh"
  - "src/run_dedupe.c"
  - "src/dedupe.c"
---

# Valgrind

`verify.sh` runs a smoke. Manual run:

```sh
valgrind --leak-check=full --track-origins=yes \
    --suppressions=tests/valgrind.supp ./oans -rd --hashfile=/tmp/h.db <tree>
```

- The suppressions cover only GLib/glibc thread TLS ("possibly lost" under
  `pthread_create`→`_dl_allocate_tls`). `definitely lost`, uninitialised
  values and invalid access are always real.
- `get_config_text` buffers are raw `memcpy`: zero-initialise anything
  `strlen`'d (`__dbfile_get_config` read an unterminated UUID).
- **`make integration-valgrind`** runs every `oans` call of the end-to-end
  suite through `tests/valgrind-wrap.sh` (`DUPEREMOVE=` it, `OANS_VG_LOGDIR=`
  for per-pid logs); a non-empty log in `.vglogs/` fails it. ~7× slower, so
  not in `check`; the CI `valgrind` job runs it on btrfs only (memory
  behaviour is fs-independent). It found the `dbfile_prepare` recreate-path
  UAF (fixed by passing `sqlite3 **`).
