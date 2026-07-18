# Running oans on a schedule (systemd)

These two template units turn oans into a set-and-forget background job: pick a
directory tree once, then let a timer re-deduplicate it (weekly by default),
picking up only what changed since last time.

They lean on the **self-describing hashfile** — a normal run records its
options, paths and excludes in the hashfile, so the scheduled run only needs to
name that file (`oans --hashfile=FILE`).

- `oans@.service` — a `oneshot` service; instance `%i` maps to
  `/var/cache/oans/%i.hash`.
- `oans@.timer` — the matching weekly trigger.

## Install

If you built from source:

```sh
sudo make install-systemd          # installs both units (see the note on paths)
```

Or copy them by hand into your system unit directory (usually
`/usr/lib/systemd/system` or `/etc/systemd/system`) and run
`sudo systemctl daemon-reload`.

> The unit ships with `ExecStart=/usr/bin/oans …`. `make install-systemd`
> rewrites that to wherever the binary actually lands (`$(PREFIX)/bin/oans`).
> If you install the units by hand and oans is elsewhere, edit `ExecStart`.

## Set up a job

Give the job a short name (here `media`) and do the first run yourself, naming
the tree and any options you want — this creates the hashfile **and** records
the configuration the timer will replay:

```sh
sudo install -d -m 0755 /var/cache/oans
sudo oans -dr --hashfile=/var/cache/oans/media.hash /srv/media
```

Then enable the timer for that job:

```sh
sudo systemctl enable --now oans@media.timer
```

That's it. Every week oans re-scans `/srv/media`, hashes only changed/new
files, and deduplicates. To change what a job does, just re-run the setup
command with new paths/options — the hashfile remembers the latest.

## Operate

```sh
systemctl list-timers 'oans@*'            # when each job next runs
systemctl start oans@media.service        # run one now, out of band
journalctl -u oans@media.service          # see what a run did
oans --history --hashfile=/var/cache/oans/media.hash   # reclaimed-over-time
```

## Tuning

- **Frequency**: `sudo systemctl edit oans@media.timer` and override
  `OnCalendar=` (e.g. `daily`, `Mon *-*-* 03:00:00`). Per-job overrides work
  because each instance is its own unit.
- **Politeness**: the service already runs at `Nice=19` / `IOSchedulingClass=idle`
  so it yields to interactive work. Override with `systemctl edit` if needed.
- **Read-only report instead of dedupe**: set the job up without `-d`
  (`oans -r --hashfile=…`) and the scheduled runs will only refresh hashes and
  report, never change data.

## Notes

- The service runs as **root** so it can read and re-extent files anywhere in
  the configured tree. It does no path sandboxing for that reason.
- Deduplication only ever adjusts extent sharing through the kernel's
  byte-verified `FIDEDUPERANGE` ioctl, so an interrupted run can waste work but
  never corrupt data.
- The hashfile under `/var/cache/oans` is just a cache; deleting it only forces
  the next run to re-hash from scratch.
