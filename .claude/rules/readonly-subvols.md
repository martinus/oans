---
paths:
  - "src/file_scan.c"
  - "src/btrfs-util.{c,h}"
  - "tests/integration/test_readonly_*.py"
---

# Read-only subvolumes are scanned by default (#182)

- **The kernel deduplicates into a read-only btrfs subvolume**; only a
  read-only mount is refused (`vfs_dedupe_file_range_one()` →
  `mnt_want_write_file()`, seen on 7.1.3). Never reintroduce "the kernel
  refuses read-only destinations": #156/#164 skipped snapshots under `-d` on
  it; #171/#172 made a read-only member the preferred source; #182 flipped the
  default.
- `--skip-readonly-subvols` (off, no longer tri-state/`-d`-derived) stays for
  cost: snapper/Timeshift snapshots share live extents (20 snapshots read ~20
  TiB per TiB), while rsync-then-snapshot backups hold copies worth deduping.
- Accepted trade: a file deduped against a snapshot points at its extents, so
  deleting the snapshot frees less than its size.
- 1.7.x stored `opt_skip_readonly_subvols = -1` (old "auto");
  `dbfile_load_scan_config` coerces `< 0` to 0. Only an explicit `1` survives
  a replay.
