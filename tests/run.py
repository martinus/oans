#!/usr/bin/env python3
"""Run the oans integration tests.

Thin wrapper over unittest that prints a short banner (which binary, where the
scratch lives, whether reflink works), lets you filter tests by substring, and
spreads the suite over worker processes.

Usage:
  tests/run.py                     run everything
  tests/run.py hardlink dedupe     only tests whose id contains a given string
  tests/run.py -j 8                run on 8 workers
  tests/run.py -j 1                one worker, i.e. strictly sequential
  DUPEREMOVE=/path tests/run.py            test a specific binary
  DUPEREMOVE_TEST_DIR=/mnt/btrfs tests/run.py   choose the scratch filesystem

Workers are *processes*, not threads: test_long_path chdir()s and cwd is
process-global. Tests need no cooperation to be split up - harness.setUp already
gives each one its own mkdtemp scratch and its own hashfile inside it - but a
test that reaches outside that will flake in parallel. The exception is tests
asserting on the *physical* extent layout, which concurrent I/O perturbs no
matter how isolated their files are; those set DuperemoveTest.serial and run one
at a time once the pool has drained.

Results are collected as they land and replayed into a real unittest result, so
the output is unittest's own in every mode, `-j 1` included.

Exit status is non-zero if any test fails.
"""

import argparse
import concurrent.futures as futures
import multiprocessing
import os
import subprocess
import sys
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
INTEGRATION_DIR = os.path.join(HERE, "integration")
sys.path.insert(0, INTEGRATION_DIR)

import harness  # noqa: E402  (needs the sys.path insert above)

# The suite is I/O-bound, not CPU-bound: workers sit in subprocess.run() and
# sync(), so total CPU is flat across job counts and nproc alone
# under-subscribes badly. Measured on a 4-core box over XFS: j=4 6.9s,
# j=8 4.4s, j=12 3.6s, j=16 3.6s. Hence 2x nproc, and a ceiling because a worker
# is not always a plain oans - under valgrind or ASAN each one costs far more
# memory, and the scratch filesystem is shared. 8 is a deliberate compromise
# short of the ~12 plateau, not a measured optimum.
MAX_AUTO_JOBS = 8

# What a worker ships back per test, and the result method that replays it.
# addUnexpectedSuccess takes no detail argument; the rest take one.
_REPLAY = {
    "failures": "addFailure",
    "errors": "addError",
    "skipped": "addSkip",
    "expectedFailures": "addExpectedFailure",
    "unexpectedSuccesses": "addUnexpectedSuccess",
}


def _shard(value):
    """Parse an "I/N" shard spec into a 1-based (i, n) pair."""
    try:
        i, n = (int(x) for x in value.split("/", 1))
    except ValueError:
        raise argparse.ArgumentTypeError(f"expected I/N, got {value!r}")
    if not 1 <= i <= n:
        raise argparse.ArgumentTypeError(f"shard {i} out of range 1..{n}")
    return i, n


def _deal(ids, i, n):
    """Every n-th id starting at i-1, over a stable ordering.

    Sorted first so the split does not depend on discovery order: a given test
    lands in the same shard on every machine and every run, which keeps a
    failure reproducible from the shard number alone.
    """
    return sorted(ids)[i - 1::n]


def _matches(test_id, patterns):
    return not patterns or any(p in test_id for p in patterns)


def _tsan_note():
    """Describe the binary's ThreadSanitizer state, if it has one.

    `make integration` exports TSAN_OPTIONS (suppressions=tests/tsan.supp,
    halt_on_error=0). Running this script directly - which the usage above
    invites - silently drops them, and without tsan.supp GLib's thread-pool
    malloc/free pair reports as a data race: ~85 tests fail with reports that
    look damning and are entirely artefacts of the missing options. Warn instead
    of failing, since running the suite by hand is legitimate.

    Returns None for a non-TSAN build; detection reads the binary rather than
    guessing from the environment, because the build flags are not visible here.
    """
    try:
        with open(harness.DUPEREMOVE, "rb") as f:
            tsan_build = b"__tsan_init" in f.read()
    except OSError:
        return None
    if not tsan_build:
        return None
    if "suppressions=" in os.environ.get("TSAN_OPTIONS", ""):
        return "thread (suppressions active)"
    return ("thread, but TSAN_OPTIONS has no suppressions= -- expect a wall of "
            "false races from GLib internals.\n"
            "           Use `make integration SANITIZE=thread` instead, which "
            "sets them.")


def _jobs(value):
    """Parse -j: a positive count, or 'auto'. argparse reports int()'s ValueError."""
    if value == "auto":
        return min(2 * (os.cpu_count() or 1), MAX_AUTO_JOBS)
    jobs = int(value)
    if jobs < 1:
        raise argparse.ArgumentTypeError("must be >= 1 or 'auto'")
    return jobs


class _Stream:
    """The writeln()-capable stream unittest's result object expects."""

    def write(self, text):
        sys.stdout.write(text)

    def writeln(self, text=""):
        sys.stdout.write(text + "\n")

    def flush(self):
        sys.stdout.flush()


class _WorkerTest:
    """Stands in for a test that ran in a worker, for display purposes only."""

    def __init__(self, test_id):
        self.test_id = test_id

    def __str__(self):
        return f"{self.test_id.rsplit('.', 1)[-1]} ({self.test_id})"

    def shortDescription(self):
        return None


class _ReplayResult(unittest.TextTestResult):
    """Collects worker outcomes; tracebacks arrive already formatted."""

    def _exc_info_to_string(self, err, test):
        return err


def _run_one(test_id):
    """Run one test in this worker and return a picklable summary of it.

    Dispatch is per test rather than per class, so setUpClass runs once per test
    (120x rather than 33x). That is free today - harness's is a stat and a
    makedirs - but a real class fixture would have to be dispatched as a unit.
    """
    result = unittest.TestResult()
    started = time.monotonic()
    unittest.defaultTestLoader.loadTestsFromName(test_id).run(result)
    elapsed = time.monotonic() - started

    outcomes = [(kind, detail)
                for kind in ("failures", "errors", "skipped", "expectedFailures")
                for _test, detail in getattr(result, kind)]
    outcomes += [("unexpectedSuccesses", None)
                 for _test in result.unexpectedSuccesses]
    return elapsed, outcomes


def _dispatch(test_ids, jobs, result):
    """Run test_ids across `jobs` workers, recording outcomes into `result`."""
    # Pin fork: workers inherit the parent's already-imported test modules, so
    # loadTestsFromName costs ~0.2ms. Python 3.14 defaults Linux to forkserver,
    # under which every worker would re-import harness - whose module level
    # probes reflink support and the scratch fstype - at ~0.15s each.
    context = multiprocessing.get_context("fork")
    with futures.ProcessPoolExecutor(max_workers=jobs, mp_context=context) as pool:
        pending = {pool.submit(_run_one, tid): tid for tid in test_ids}
        for future in futures.as_completed(pending):
            test = _WorkerTest(pending[future])
            result.startTest(test)
            try:
                elapsed, outcomes = future.result()
            except Exception as exc:        # worker died: segfault, OOM, ...
                outcomes = [("errors", f"worker died: {exc}\n")]
            if not outcomes:
                result.addSuccess(test)
            for kind, detail in outcomes:
                add = getattr(result, _REPLAY[kind])
                add(test) if detail is None else add(test, detail)
            result.stopTest(test)


def _run_suite(parallel_ids, serial_ids, jobs, verbosity=2):
    result = _ReplayResult(_Stream(), True, verbosity)
    started = time.monotonic()

    _dispatch(parallel_ids, jobs, result)
    # DuperemoveTest.serial tests want the box to themselves: their setup builds
    # a specific physical extent layout, which concurrent I/O disturbs. Held to
    # the end so they don't serialise the rest of the run either.
    if serial_ids:
        _dispatch(serial_ids, 1, result)

    result.printErrors()
    tail = f" (+{len(serial_ids)} serial)" if serial_ids else ""
    result.stream.writeln(result.separator2)
    result.stream.writeln(f"Ran {result.testsRun} tests in "
                          f"{time.monotonic() - started:.2f}s on {jobs} workers{tail}")
    result.stream.writeln()
    if result.wasSuccessful():
        result.stream.writeln(f"OK (skipped={len(result.skipped)})")
        return 0
    result.stream.writeln(f"FAILED (failures={len(result.failures)}, "
                          f"errors={len(result.errors)}, "
                          f"skipped={len(result.skipped)})")
    return 1


def main(argv):
    parser = argparse.ArgumentParser(
        description="Run the oans integration tests.",
        formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    parser.add_argument("-j", "--jobs", type=_jobs, default="auto",
                        help="worker processes, or 'auto' (default: auto)")
    parser.add_argument("--shard", type=_shard, default=None, metavar="I/N",
                        help="run only shard I of N (1-based), for splitting a "
                             "slow leg across CI jobs")
    parser.add_argument("patterns", nargs="*",
                        help="only run tests whose id contains one of these")
    args = parser.parse_args(argv[1:])

    version = subprocess.run([harness.DUPEREMOVE, "--version"],
                             capture_output=True, text=True).stdout.strip()
    print("oans integration tests")
    print(f"  binary : {harness.DUPEREMOVE} ({version})")
    print(f"  scratch: {harness.TEST_ROOT} ({harness.scratch_fstype()})")
    print(f"  reflink: {'yes' if harness.REFLINK else 'no (dedupe tests will skip)'}")
    print(f"  jobs   : {args.jobs}")
    tsan = _tsan_note()
    if tsan:
        print(f"  sanitize: {tsan}")
    print(flush=True)

    loader = unittest.TestLoader()
    discovered = loader.discover(start_dir=INTEGRATION_DIR, pattern="test_*.py")
    tests = [t for t in _iter_tests(discovered) if _matches(t.id(), args.patterns)]
    serial = [t.id() for t in tests if getattr(t, "serial", False)]
    parallel = [t.id() for t in tests if not getattr(t, "serial", False)]
    if args.shard:
        i, n = args.shard
        # Deal round-robin over each group separately rather than cutting the
        # list in two. Contiguous slices would put whole classes in one shard,
        # and neighbouring tests in a class cost about the same, so the halves
        # come out lopsided. Sharding the serial group too matters most: it runs
        # one-at-a-time, so a shard that got all of it would still pay the full
        # sequential floor and cap the whole split.
        serial = _deal(serial, i, n)
        parallel = _deal(parallel, i, n)
        print(f"  shard  : {i} of {n}\n", file=sys.stderr)
    if serial:
        print(f"  serial : {len(serial)} extent-layout tests held to the end\n",
              flush=True)
    return _run_suite(parallel, serial, args.jobs)


def _iter_tests(suite):
    for item in suite:
        if isinstance(item, unittest.TestSuite):
            yield from _iter_tests(item)
        else:
            yield item


if __name__ == "__main__":
    sys.exit(main(sys.argv))
