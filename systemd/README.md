# Running oans on a schedule with systemd

Two template units turn oans into a weekly background job: `oans@.service` runs
one deduplication pass, and `oans@.timer` starts it. Each job is one hashfile,
`/var/cache/oans/<name>.hash`, and that hashfile also stores which paths and
options the job uses. So the unit only names the hashfile
(`oans --quiet --hashfile=/var/cache/oans/%i.hash`), and every run after the
first one only hashes what is new or changed.

For a full walk-through on a NAS, from checking the filesystem to monitoring,
see [docs/nas-quickstart.md](../docs/nas-quickstart.md).

## Install the units

- **AUR** (`oans`, `oans-git`): the package installs both units into
  `/usr/lib/systemd/system`. Nothing to do.
- **From source**: `sudo make install-systemd`. It installs into
  `$(PREFIX)/lib/systemd/system` (`PREFIX` defaults to `/usr/local`) and
  rewrites `ExecStart=` to `$(PREFIX)/bin/oans`. It is not part of
  `make install`.
- **Release tarball**: it does not contain the units. Copy
  [`oans@.service`](oans@.service) and [`oans@.timer`](oans@.timer) to
  `/etc/systemd/system/`, change `/usr/bin/oans` in `ExecStart=` to
  `/usr/local/bin/oans` (where `INSTALL.txt` puts the binary), and run
  `sudo systemctl daemon-reload`.

## A job is set up by one run by hand

Pick a short name for the job, e.g. `media`, and run oans once with the paths
and options the job should use. Then enable the timer of the same name:

```sh
sudo install -d -m 0755 /var/cache/oans
sudo oans -dr --exclude .snapshots --hashfile=/var/cache/oans/media.hash /srv/media
sudo systemctl enable --now oans@media.timer
```

The name after the `@` is the hashfile's name without `.hash`, so
`oans@media.timer` runs `/var/cache/oans/media.hash`. The first run hashes
everything and deduplicates, which makes it the slow one. It also stores its
configuration in the hashfile: `-r`, `-d`, `--exclude`, `--min-filesize`,
`--max-filesize`, `--skip-zeroes`, `--skip-readonly-subvols`,
`--dedupe-options` and the paths, as absolute paths. The block size (`-b`)
belongs to the hashfile anyway. Flags that only shape one run, like
`--io-threads`, `-q` or `-v`, are not stored.

**Include `-d` in that first run.** The timer replays exactly what the first
run stored. A hashfile seeded by a preview run without `-d` gives a job that
hashes every week and never deduplicates: it exits 0 and reclaims 0 B, which is
also what a healthy job on an already deduplicated tree reports. Thankfully oans
warns about this on every replay and prints the command that fixes it:

```
WARNING: the stored configuration for /var/cache/oans/media.hash has no -d, so this run will hash but not deduplicate.
         Re-run once with -d and the paths to update it:
           oans -r -d --hashfile=/var/cache/oans/media.hash --exclude='.snapshots' '/srv/media'
```

To see what a job does, ask its hashfile:

```sh
oans --stats --hashfile=/var/cache/oans/media.hash
```

```
stored scan   (replayed by: oans --hashfile=/var/cache/oans/media.hash)
  options         -r -d
  paths       /srv/media
  excludes    .snapshots
```

## Changing a job means running the whole setup command again

A run that names paths replaces the stored configuration completely: options,
paths and excludes. That means a run that leaves out an `--exclude` also removes
it from the job, and from then on the timer scans and deduplicates what you
wanted to skip. So copy the current settings from `--stats`, change what you
need, and run the full command once by hand.

A replay (`oans --hashfile=FILE` without paths, which is what the unit runs)
never changes the stored configuration. An extra `--exclude` given to a replay
applies to that one run only.

## The hashfile holds the job, not only a cache

The hashes are a cache. A scan rebuilds them, and that only costs the time to
read all the data again. The job definition is not a cache: it exists only in
the hashfile. Delete `/var/cache/oans/media.hash` and the paths, the excludes
and the options of the job are gone with it.

Unfortunately systemd stays quiet about that. `oans@.service` has
`ConditionPathExists=/var/cache/oans/%i.hash`, so the timer still fires, systemd
skips the service because the condition is not met, and records the skip as a
success. No `OnFailure=` fires. The job just stops doing anything.

To re-hash a job from scratch:

1. Note the stored configuration from `oans --stats --hashfile=...`.
2. Check that no run is active (`systemctl status oans@media.service`), then
   delete the hashfile.
3. Run the setup command again.

The same happens if a future oans version changes the hashfile format: oans
then rebuilds the file from scratch, and the stored configuration does not
survive that either. In that case the next timer run fails with exit 1 and
says so:

```
Error: no files given and the hashfile has no stored scan configuration to replay.
Run once with the paths to record it, e.g.:
  oans -dr --hashfile=/var/cache/oans/media.hash <dir>...
```

Keep the setup command somewhere outside the hashfile, e.g. in a small script,
and both cases cost one command.

## The exit status says whether a run covered the whole job

| Exit | What happened | systemd |
|---|---|---|
| 0 | The run completed. | success |
| 2 | The run completed, but a stored path no longer exists, e.g. an unmounted drive or a renamed share. | failed |
| 1 | oans refused to run or hit an error, e.g. none of the stored paths exist, or the stored configuration cannot be read. | failed |
| 130, 143 | Stopped by SIGINT or SIGTERM (`systemctl stop`, a reboot). | success, via `SuccessExitStatus=` |
| none | The hashfile does not exist. | skipped, recorded as success |

Exit 2 is the one worth an alert, because such a run deduplicates everything
else and otherwise looks healthy. When every stored path is gone, oans refuses
to run instead: a scan of nothing would prune every file from the hashfile. To
get told about a failed run, add an `OnFailure=` unit of your own, e.g. one that
sends a mail:

```sh
sudo systemctl edit oans@media.service
```

```ini
[Unit]
OnFailure=status-email@%i.service
```

## Running and watching jobs

```sh
systemctl list-timers 'oans@*'                          # when each job runs next
sudo systemctl start oans@media.service                 # run a job now
journalctl -u oans@media.service                        # what the runs printed
oans --history --hashfile=/var/cache/oans/media.hash    # reclaimed per run
oans --json --hashfile=/var/cache/oans/media.hash       # metrics for a dashboard
```

The report commands (`--stats`, `--history`, `--json`, `-L`) open the hashfile
read-only, so they are safe while a run is active. Don't start a second run that
writes to the same hashfile (a manual `oans -dr`, or `-R`) while the timer's run
is active. oans expects one writer per hashfile, and systemd only knows about
the runs it started itself.

Several jobs are several hashfiles: `oans@photos.timer`, `oans@backups.timer`
... Each one has its own schedule and its own history.

## Tuning with drop-ins: reset a list before you set it

`systemctl edit` writes a drop-in that is added on top of the shipped unit.
`OnCalendar=` and `ExecStart=` are lists, so a drop-in line adds an entry
instead of replacing the shipped one. An empty assignment resets the list
first:

```ini
# sudo systemctl edit oans@media.timer
[Timer]
OnCalendar=
OnCalendar=Mon *-*-* 03:00:00
```

Without the empty `OnCalendar=` line the job runs on both schedules. For the
service it is the same, and there a missing reset makes systemd run both
commands, one after the other:

```ini
# sudo systemctl edit oans@media.service
[Service]
ExecStart=
ExecStart=/usr/bin/oans --quiet --io-threads=4 --progress=json --hashfile=/var/cache/oans/%i.hash
```

Use the binary path of your install there: `systemctl cat oans@media.service`
shows the shipped `ExecStart=`. `--progress=json` writes one JSON object per
phase, about once a second, to
stderr, which lands in the journal. `journalctl -u oans@media.service -o cat`
gives you the plain stream.

What the shipped units already set:

- `OnCalendar=weekly` with `Persistent=true`, so a run that was missed while the
  machine was off happens at the next boot, and `RandomizedDelaySec=30min`, so
  several jobs do not all start at once.
- `Nice=19` and `IOSchedulingClass=idle`, so a run gives way to everything
  interactive.
- `TimeoutStopSec=5min` for a clean stop, see below.

## Stopping a run is safe

`systemctl stop` or a reboot sends SIGTERM. oans then commits what it hashed,
finishes the dedupe requests that are already running, leaves the rest for the
next run and exits 143, which the unit counts as success. Deduplication goes
through the kernel's `FIDEDUPERANGE` ioctl, which is atomic and compares the
bytes before it shares anything. So even a run that gets killed after
`TimeoutStopSec=` cannot corrupt data. It costs at most the hashing done since
the last commit, which happens every 10 seconds.

## Notes

- The service runs as root, so it can read and deduplicate files anywhere in
  the job's paths. For the same reason it has no path sandboxing, only
  `NoNewPrivileges=true`.
- oans creates a new hashfile with mode `0600`, since it lists every path in
  the job.
- A hash-only job, set up without `-d`, is a valid setup, e.g. to keep a
  hashfile warm for a later manual `-d` run. It prints the warning above on
  every run.
