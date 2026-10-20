---
paths:
  - "src/file_scan.c"
  - "src/util.c"
  - "src/oans.c"
  - "tests/integration/test_scan_scope.py"
---

# One run, one filesystem; paths as the walk spells them (#282)

- **A root on another filesystem is a root the run did not cover.**
  `check_file()` returned false for it with no message and no counter, so
  `oans -r A B` with B elsewhere exited 0 having scanned A, and every replay
  of the stored roots did it again. `other_fs()` now warns and counts it in
  `nr_roots_unusable`, so the run exits 2 like a missing root. A filesystem
  mounted *below* a root stays out as before, under a `-v` message only.
- **A path oans compares with stored ones goes through `absolute_path()`**
  (`src/util.c`): `realpath()`, else the resolved directory plus the last
  component (for `-R` of a file already gone), else lexical. A relative
  `--hashfile` inside the tree was added to the excludes as typed, never
  matched the walk's absolute paths, and got hashed with its `-wal`/`-shm`
  every run; a relative `-R` said "Removed" and removed nothing. `-R` now
  checks `sqlite3_changes()` and exits 1 when a path removed nothing.
