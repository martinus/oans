# Contributing to oans

Thanks for your interest! oans is a performance-focused fork of
[duperemove](https://github.com/markfasheh/duperemove) — the core engine is the
work of Mark Fasheh and the upstream contributors, and contributions here are
expected to keep that lineage and its GPL-2.0 license intact.

## Reporting bugs and requesting features

Open an [issue](https://github.com/martinus/oans/issues) using the templates. For
a suspected dedupe/correctness problem, please include your kernel version,
filesystem (`btrfs` / `xfs`), the exact command line, and whether it reproduces
on a fresh hashfile.

> Deduplication goes through the kernel's `FIDEDUPERANGE` ioctl, which
> byte-compares every range before sharing it. A bug can waste work or miss a
> dedupe — it cannot corrupt your data.

## Building and testing

Install the build dependencies listed in the [README](README.md), then:

```sh
make -j$(nproc)     # build (warnings are treated as failures)
make check          # C unit tests + Python integration suite
```

The integration suite is stdlib-only Python `unittest`. Dedupe test cases need a
**reflink-capable filesystem** (btrfs or xfs) as the scratch dir — see
[`tests/README.md`](tests/README.md). Never run tests or benchmarks out of
`/tmp` (tmpfs is not reflink-capable; a scan there silently stores zero files).

Before opening a PR, run the full pre-flight gate:

```sh
scripts/verify.sh   # build + make check + a valgrind scan/dedupe/replay smoke
```

## Pull requests

- Branch from `master`, keep the change focused, and describe what you measured.
- **Performance claims must be backed by a measurement.** A/B two distinctly
  named binaries, interleave the runs, and confirm they actually differ before
  trusting numbers. See [`docs/benchmarks.md`](docs/benchmarks.md).
- Add or update tests for behavior changes; keep the suite green.
- If you touch the man page, edit `docs/man/oans.md` and run `make doc` to
  regenerate `oans.8` (needs `pandoc`). No pandoc installed? `make pandoc`
  fetches a prebuilt one into `.pandoc/` from PyPI, and `make doc` picks it up.
- New code should read like the code around it — match the existing style.

## License

By contributing you agree your changes are licensed under
[GPL-2.0](LICENSE), the same as upstream.
