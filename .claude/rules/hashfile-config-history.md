---
paths:
  - "src/oans.c"
  - "src/opt.{c,h}"
  - "src/dbfile.c"
  - "systemd/**"
  - "tests/integration/test_self_describing.py"
  - "tests/integration/test_history_metrics.py"
---

# Self-describing hashfile, history & scheduling

- **Each run stores its scan config** (`opt_*` keys in `config`, realpath'd
  roots, `--exclude` patterns in `scan_roots`/`scan_excludes`) via
  `dbfile_store_scan_config`/`load_scan_config`.
- **A bare `oans --hashfile=FILE` replays it** (`apply_scan_config` +
  `drop_missing_roots`, no re-persist). `--io-threads`, `-B`, `-q`, `-v` and an
  extra `--exclude` still apply. Missing roots warn; if all are gone oans
  refuses, so the stat prune cannot wipe the hashfile. The hashfile must exist
  (#284). Pinned by `test_self_describing.py`.
  - **The "has no -d" hint prints every stored setting (#285)**
    (`scan_config_options_str()` + excludes): the printed command (`-r`, `-d`,
    roots) stores its own config, so a dropped setting is erased. Pinned by
    `test_the_command_it_prints_keeps_every_stored_setting`.
- **`-B` only sizes scan generations (#285)**; dedupe passes are
  `DEDUPE_FILES_PER_PASS`, so `-B 1` and `-B 1024` give the same passes and
  memory. Range 1..2^31 (0, or `4G` narrowed to 0, divided by zero).
- **Run history:** `dbfile_record_run` (from `main()` after
  `process_duplicates`) appends to `run_history`; `--history` and `--json` read
  it (`dbfile_get_run_summary`). Take the file count from
  `pscan_files_scanned()` **inside `scan_files`** (later reads are 0), not
  `files_examined` (every visited file). Pinned by `test_history_metrics.py`.
- **Report modes** `-L`/`-R`/`--stats`/`--history`/`--json` are mutually
  exclusive (`report_count` in `parse_options`); all but `-R` are read-only.
- **Scheduling:** `systemd/oans@.{service,timer}` (`make install-systemd`, not
  `make install`) run `oans --hashfile=/var/cache/oans/%i.hash`. Guide:
  `docs/nas-quickstart.md`.
- The `fdupes` mode was removed; don't reintroduce it.
