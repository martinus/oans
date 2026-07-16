# Upstream open issues vs. this fork

All **59 open** issues on
[markfasheh/duperemove](https://github.com/markfasheh/duperemove/issues) as of
2026-07-16, assessed against this fork (`martinus/duperemove`). Verdicts come
from reading the fork's code and this session's changes; cases reproduced or
covered by a test are called out.

Legend: ✅ fixed/addressed · ⚠️ still affected · ❌ out of scope / not a code
bug · ❓ unverified (needs a repro) · 🔒 assessed, deliberately deferred (needs
maintainer decision or is too risky to change unilaterally).

## Fixed by pull requests in this fork

| # | Title | PR / where |
|---|-------|-----------|
| [#398](https://github.com/markfasheh/duperemove/issues/398) | ioctl returns 22 (EINVAL) | Length clamped to real file size + skip destinations changed since scan (`test_einval.py`, `test_changed_since_scan.py`). |
| [#389](https://github.com/markfasheh/duperemove/issues/389) | Crash: `--fdupes` with hard links | #25 — skip an inode already listed (`test_fdupes.py`). |
| [#394](https://github.com/markfasheh/duperemove/issues/394) | 32-bit output shows total `0` | #26 — `%lu` → `PRIu64` for the 64-bit counters. |
| [#353](https://github.com/markfasheh/duperemove/issues/353) | Control chars in filenames block the terminal | #26 — `sanitize_ctrl()` on status + result output (`test_sanitize_ctrl`). |
| [#348](https://github.com/markfasheh/duperemove/issues/348) | Search status `pos == NaN` | #26 — guard divide-by-zero. |
| [#396](https://github.com/markfasheh/duperemove/issues/396) / [#407](https://github.com/markfasheh/duperemove/issues/407) | Infinite loop / hang during dedupe | #27 — stop when a round dedupes 0 bytes with no error (zero-progress guard). |
| [#388](https://github.com/markfasheh/duperemove/issues/388) | `show-shared-extents` not installed | #28 — install the shipped script. |
| [#387](https://github.com/markfasheh/duperemove/issues/387) | Tarball build has no version | #29 — `version` file fallback; `make tarball` embeds it. |
| [#374](https://github.com/markfasheh/duperemove/issues/374) | "Compressed FS" extent handling broken (really: sparse trailing hole) | #31 — store the last extent + stop cleanly at a trailing hole (`test_sparse.py`). Compression itself maps fine on current kernels. |

## Already addressed by earlier fork work

| # | Title | Basis |
|---|-------|-------|
| [#292](https://github.com/markfasheh/duperemove/issues/292) | Process largest chunks first | `push_extents()` sorts by verify work, largest-first (LPT). |
| [#379](https://github.com/markfasheh/duperemove/issues/379) | "Database is locked" | `PRAGMA busy_timeout = 30000`. |
| [#350](https://github.com/markfasheh/duperemove/issues/350) | No final summary (one per batch) | Single aggregated summary in `dedupe_end()`. |
| [#159](https://github.com/markfasheh/duperemove/issues/159) | Time-to-finish estimate | ETA on scan and dedupe status. |
| [#378](https://github.com/markfasheh/duperemove/issues/378) | Exclude files by size | `--min-filesize`. |
| [#395](https://github.com/markfasheh/duperemove/issues/395) | Incorrect `-nan%` (0/0) | `percent()` guards zero; scan totals guard zero. |
| [#391](https://github.com/markfasheh/duperemove/issues/391) | Crash on path > PATH_MAX | Walk skips over-long paths instead of `abort()`. |
| [#358](https://github.com/markfasheh/duperemove/issues/358) | `--std=c23` unrecognized | Fork builds with `-std=gnu11`. |
| [#383](https://github.com/markfasheh/duperemove/issues/383) | `-q` can't suppress "format under development" warning | Warning no longer exists. |
| [#286](https://github.com/markfasheh/duperemove/issues/286) | Progress indicator not predictable | Dedupe bar capped at 99% while working; scan shows files + bytes. |
| [#168](https://github.com/markfasheh/duperemove/issues/168) | Dedupe increases fragmentation | Least-fragmented-target selection (partial; no defrag). |
| [#397](https://github.com/markfasheh/duperemove/issues/397) | `--hashfile` dislikes leading-`-` paths | Not reproducible on the fork. |
| [#367](https://github.com/markfasheh/duperemove/issues/367) | 0.15 fails to build | Fork builds clean under CI. |

## Still affected — deferred, with rationale

Real but not fixed here: each is either a larger project needing its own
investigation/hardware, or a behavior change the maintainer should decide.

| # | Title | Why deferred |
|---|-------|--------------|
| [#331](https://github.com/markfasheh/duperemove/issues/331) | Re-dedupes already-shared extents | 🔒 `clean_deduped()` already skips extents seen shared within a run, but cross-run already-shared extents are still re-issued. A pre-ioctl "already shared?" check risks skipping legitimate work; needs design + measurement. |
| [#382](https://github.com/markfasheh/duperemove/issues/382) | `-d <path>` dedupes the whole hashfile, not `<path>` | 🔒 Deliberate design (dedupe works from hashfile generations). Restricting to the path argument changes long-standing behavior — a maintainer call. |
| [#401](https://github.com/markfasheh/duperemove/issues/401) / [#371](https://github.com/markfasheh/duperemove/issues/371) / [#393](https://github.com/markfasheh/duperemove/issues/393) / [#306](https://github.com/markfasheh/duperemove/issues/306) | io-threads slow on HDD | 🔒 Auto-detecting rotational devices is unreliable behind btrfs multi-device / LVM / dm. Fork caps threads at 8; recommend `--io-threads=1` on HDD. |
| [#251](https://github.com/markfasheh/duperemove/issues/251) | No pre-check for NoCOW (+C) | ⚠️ Optimization only now — the hang it caused is fixed (#27) and NoCOW is handled via EINVAL. Low value. |
| [#404](https://github.com/markfasheh/duperemove/issues/404) | Handle in-use executables (ETXTBSY) | ⚠️ Still skipped (now tallied in the summary); the "busy list / reflink-swap" feature is unimplemented. |
| [#88](https://github.com/markfasheh/duperemove/issues/88) / [#386](https://github.com/markfasheh/duperemove/issues/386) | Re-hashes unchanged extents across snapshots | 🔒 Wants a per-extent checksum cache; real feature, not a quick fix. |
| [#155](https://github.com/markfasheh/duperemove/issues/155) / [#176](https://github.com/markfasheh/duperemove/issues/176) | Confusing per-file statuses | ⚠️ Kernel-returned statuses; fork aggregates errors in the summary but doesn't annotate each. |

## Filesystem / environment support

| # | Title | Note |
|---|-------|------|
| [#380](https://github.com/markfasheh/duperemove/issues/380) / [#312](https://github.com/markfasheh/duperemove/issues/312) | ZFS: needs non-fiemap dedupe | ⚠️ Fork still requires fiemap; ZFS works only via `--fdupes`. |
| [#342](https://github.com/markfasheh/duperemove/issues/342) | Support bcachefs | ❌ Support request. |
| [#363](https://github.com/markfasheh/duperemove/issues/363) | `-d` "succeeds" on NFS | ❓ Test-harness probe; not reproduced. |
| [#250](https://github.com/markfasheh/duperemove/issues/250) | Corruption via active gocryptfs (fuse) | ❓ Deduping a fuse view is unsupported. |
| [#199](https://github.com/markfasheh/duperemove/issues/199) | EINVAL as non-root (kernel 4.4) | ❓ Kernel-era specific; likely N/A now. |
| [#314](https://github.com/markfasheh/duperemove/issues/314) / [#189](https://github.com/markfasheh/duperemove/issues/189) / [#191](https://github.com/markfasheh/duperemove/issues/191) | RO-subvolume / snapshot semantics | ❌ Questions about read-only subvolumes and unsharing. |

## Feature requests (out of scope)

| # | Title |
|---|-------|
| [#400](https://github.com/markfasheh/duperemove/issues/400) | rsync from the hashfile |
| [#213](https://github.com/markfasheh/duperemove/issues/213) | Defrag while dedup |
| [#204](https://github.com/markfasheh/duperemove/issues/204) | Duplication-ratio-only report |
| [#215](https://github.com/markfasheh/duperemove/issues/215) | systemd units |
| [#185](https://github.com/markfasheh/duperemove/issues/185) | Rolling-window hash (analyzed: not useful — reflink needs 4K alignment) |
| [#27](https://github.com/markfasheh/duperemove/issues/27) | overlayfs-like listing/revert |

## Packaging / release / unreproducible

| # | Title | Note |
|---|-------|------|
| [#313](https://github.com/markfasheh/duperemove/issues/313) | No 0.13 RPM for AlmaLinux 9 | ❌ Distro packaging. |
| [#392](https://github.com/markfasheh/duperemove/issues/392) | Abort after long XFS dedupe | ❓ Asserts still present; cause unknown, not reproduced. |
| [#372](https://github.com/markfasheh/duperemove/issues/372) | Crash `hash-tree.c:68` (v0.11.2) | ❓ Very old; after I/O read errors. |
| [#337](https://github.com/markfasheh/duperemove/issues/337) | Early SIGSEGV in DB init | ❓ Non-deterministic, no backtrace. |
| [#305](https://github.com/markfasheh/duperemove/issues/305) / [#319](https://github.com/markfasheh/duperemove/issues/319) / [#370](https://github.com/markfasheh/duperemove/issues/370) | Hang / slow on partial + hashfile | Same hang class as #396/#407, now guarded (#27); re-test on the reporters' data recommended. |

## Suggested follow-ups (not done here)

- **Upstream the #398 EINVAL fix and the #27 zero-progress guard** — both are
  absent upstream and fix real hangs/failures.
- **#331 already-shared re-dedupe** and **#382 path-scoped dedupe** remain the
  notable open items, both needing a maintainer decision (see the deferred table).
