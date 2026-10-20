---
paths:
  - "src/oans.c"
  - "src/opt.{c,h}"
  - "src/dbfile.c"
  - "systemd/**"
  - "tests/integration/test_self_describing.py"
  - "tests/integration/test_history_metrics.py"
---

# Self-describing hashfile, history & scheduling (fork features)

- **Self-describing hashfile.** Each run stores its scan-shaping options (`opt_*`
  keys in `config`), roots (realpath'd) and `--exclude` patterns
  (`scan_roots`/`scan_excludes` tables) via
  `dbfile_store_scan_config`/`load_scan_config`. A bare `oans --hashfile=FILE`
  **replays** the last run (`apply_scan_config`+`drop_missing_roots`):
  last-run-wins for the stored settings, while `--io-threads`, `-B`, `-q`,
  `-v` and an extra (unstored) `--exclude` still apply; a missing root skipped
  with a warning — but if *all* roots are gone oans refuses (so the stat prune
  can't wipe the hashfile, e.g. an unmounted drive); a replay doesn't
  re-persist. Pinned by `test_self_describing.py`.
  - **A bare replay needs the hashfile to exist (#284).** Opening one creates
    it, so a typo in the path used to leave an empty hashfile behind.
  - **The "has no -d" hint prints every stored setting (#285)**, through
    `scan_config_options_str()` plus the stored excludes. The command it
    offers is a normal run, which stores its own configuration: naming only
    `-r`, `-d` and the roots, it erased the excludes and size limits, and the
    timer then scanned and deduplicated what the job was set up to skip.
    `test_the_command_it_prints_keeps_every_stored_setting` runs the printed
    command and compares the stored settings.
- **`-B` sizes scan generations, nothing else below 65,536 (#285).** The
  dedupe phase always runs after the whole scan, in passes of
  `DEDUPE_FILES_PER_PASS` files, so `-B 1` and `-B 1024` give the same passes
  and the same memory. The docs said it ran the dedupe every N files and
  capped memory. It is 1..2^31 now: 0, or a value that narrowed to 0 like
  `4G`, divided by zero after the scan.
- **Run history & metrics.** Each run appends to `run_history`
  (`dbfile_record_run`, from `main()` after `process_duplicates`). `--history` =
  human timeline + lifetime totals (`dbfile_get_run_summary`); `--json` = a flat
  metrics object for jq/Telegraf. Gotcha: take the per-run file count from
  `pscan_files_scanned()` (out-param) **inside `scan_files`** — the dedupe phase
  reuses the progress counters, so a later read is 0. (Not `files_examined`,
  which counts every file the walk *visited*, up-to-date ones included, not just
  those hashed.) Pinned by `test_history_metrics.py`.
- **Report modes** `-L`/`-R`/`--stats`/`--history`/`--json` are mutually
  exclusive (one `report_count` check in `parse_options`): `--stats` = hashfile
  report, `-L` lists files, `-R` removes paths. All but `-R` open the hashfile
  read-only.
- **Scheduling.** `systemd/oans@.{service,timer}` (via `make install-systemd`,
  kept out of `make install`) run `oans --hashfile=/var/cache/oans/%i.hash` on a
  timer, replaying the stored config. Guide: `docs/nas-quickstart.md`.
- The `fdupes` mode was **removed** — don't reintroduce it.
