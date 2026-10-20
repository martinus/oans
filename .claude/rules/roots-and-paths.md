---
paths:
  - "src/file_scan.c"
  - "src/util.c"
  - "src/oans.c"
  - "tests/integration/test_scan_scope.py"
---

# One run, one filesystem; paths as the walk spells them (#282)

- **A root on another filesystem is an uncovered root**: `other_fs()` warns
  and counts it in `nr_roots_unusable`, so the run exits 2 like a missing
  root. `check_file()` used to drop it silently and exit 0, on every replay. A
  filesystem mounted below a root stays out, `-v` message only.
- **Any path compared with stored ones goes through `absolute_path()`**
  (`src/util.c`): `realpath()`, else resolved directory plus last component
  (`-R` of a deleted file), else lexical. Otherwise a relative `--hashfile`
  exclude never matches and it and its `-wal`/`-shm` get hashed.
- `-R` checks `sqlite3_changes()` and exits 1 when a path removed nothing.
