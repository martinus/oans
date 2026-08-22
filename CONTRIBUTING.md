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
make -j$(nproc)              # build
make check                   # C unit tests + Python integration suite
make -j$(nproc) WERROR=1     # ...with any warning treated as an error, as CI builds
```

`WERROR=1` is how every CI job builds, so a warning fails the build rather than
scrolling past in the log. The tree is warning-free under both gcc and clang;
the Makefile probes `$(CC)` for each extra warning flag it enables, since the two
compilers accept different sets (`-Wduplicated-cond` and friends are GCC-only).

The integration suite is stdlib-only Python `unittest`. Dedupe test cases need a
**reflink-capable filesystem** (btrfs or xfs) as the scratch dir — see
[`tests/README.md`](tests/README.md). Never run tests or benchmarks out of
`/tmp` (tmpfs is not reflink-capable; a scan there silently stores zero files).

Before opening a PR, run the full pre-flight gate:

```sh
scripts/verify.sh   # build + make check + a valgrind scan/dedupe/replay smoke
```

### Mutation testing

Coverage says a line ran; it does not say anything would have noticed it
misbehaving. `scripts/mutate/mutate.py` asks the second question: it breaks a
source file in a throwaway copy of the tree, rebuilds, runs the C unit suite and
reports whether anything went red.

```sh
scripts/mutate/mutate.py --file src/fiemap.c --bugs scripts/mutate/bugs/fiemap.txt
scripts/mutate/mutate.py --file src/glob.c --diff        # only what you changed
scripts/mutate/mutate.py --file src/util.c --dry-run     # how many, and how long
```

The everyday use is the first one: the bug files in `scripts/mutate/bugs/` are
real oans bugs — #147, #159, #186, #187, #191, #202 — put back one at a time, so
"does this test earn its place" has an answer. Add a block whenever you fix
something, and check that the fix's own test is what catches it.

**Read the survivors by function, not as a total.** The first sweep of
`src/util.c` reported 213 of 413 surviving, which sounds damning and says
nothing: survival was 5-9% everywhere a test existed and 85-94% in four pure
functions that had none. Writing tests for those four took it to 94, and what
remains is code no unit test can reach (`setrlimit`, `sysconf`,
`clock_gettime`), `parse_size`'s `exit()` paths, and a few provably equivalent
mutants. The tool cannot tell an absent test from a weak one; the grouping can.

Two things it cannot see, both worth knowing before reading a `survived`:

- **it runs the C unit suite only** (`src/tests.c`), not the end-to-end Python
  suite, which drives the binary rather than linking its code. For the dedupe
  phase, the scan pipeline and the progress block, a survivor means nothing.
- **it builds without a sanitizer by default**, so a mutant that reads one slot
  too far is only caught if it corrupts something a test checks. Re-run a
  surprising survivor with `--make-arg SANITIZE=address,undefined --make-arg
  CC=clang`.

`mutate_core.py` beside the adapter is **vendored** from unordered_dense, which
is where its own test suite lives, and nanobench holds the same copy. Do not
edit it here: change it there, run that repository's `scripts/test_mutate.py`,
copy it into all three and update each `mutate_core.sha256`. `make lint` fails
if this copy has drifted.

### Property-based tests

`src/proptest.h` is a small generator harness for the C unit suite — no
dependencies, one header. An ordinary test names an input and its answer; a
property names a relationship that must hold for *every* input and then goes
looking, which reaches cases nobody sits down and writes (a `0xc2` immediately
before the terminator, a buffer that runs out one byte into an escape).

The seed is fixed, so `make test` runs the same cases every time and CI can
trust it. To go looking for more:

```sh
OANS_PROPTEST_SEED=random ./test    # prints the seed it chose
OANS_PROPTEST_SEED=12345 ./test     # replay one
```

A failure always names the seed and the case number. Write properties for pure
functions with a stated invariant; where one needs a large input to mean
anything, write an ordinary test with a fixture instead.

### Sanitizer builds

The suites also run under clang's AddressSanitizer and UndefinedBehaviorSanitizer
in CI. To reproduce locally (needs `clang` and its `libclang-rt-dev`):

```sh
make check CC=clang SANITIZE=address,undefined   # build + run both suites, instrumented
```

`SANITIZE=...` compiles and links with `-fsanitize=<flavor>` (drops release
hardening), and the `test`/`integration` targets automatically export the
ASAN/UBSAN/LSAN run options that make any finding abort — so a sanitizer error
fails the suite. LeakSanitizer's GLib false positives are filtered by
[`tests/lsan.supp`](tests/lsan.supp) (the ASAN analogue of `tests/valgrind.supp`).
CI runs ASAN and UBSAN as separate legs; combining them locally as above is fine.

#### ThreadSanitizer

```sh
make check CC=clang SANITIZE=thread
```

TSAN needs one extra thing the other sanitizers do not, because the codebase
synchronizes with GLib rather than pthreads. `libglib-2.0` is a system library
built without instrumentation, and on Linux `GMutex`/`GCond` sit directly on
futexes rather than on `pthread_mutex` (which TSAN intercepts), so TSAN sees no
happens-before edge across a correctly locked critical section and reports it as
a race — an unannotated run reported ~145 of them, and even a *provably* correct
two-thread `GMutex` program reports one.

[`src/tsan.h`](src/tsan.h) closes that gap: it publishes the edges TSAN cannot
see (`__tsan_acquire`/`__tsan_release` keyed on each primitive, plus the
pool/queue handoffs keyed on the item). The Makefile force-includes it with
`-include` for `SANITIZE=thread`, so no call site changes, and it also defines
`LOCK_MEMSTATS` — the allocation counters in `memstats.h` are deliberately
unlocked outside `DEBUG_BUILD`, a genuine if benign race that would otherwise
bury the reports worth reading.

Keep the annotations tight and prefer fixing a race over widening one:
over-annotating hides real bugs, which is the whole point of the tool.
[`tests/tsan.supp`](tests/tsan.supp) is for the residue that genuinely cannot be
annotated — synchronization that happens entirely inside GLib.

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
