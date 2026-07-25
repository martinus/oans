#!/usr/bin/env python3
"""Run the oans integration tests.

Thin wrapper over unittest that prints a short banner (which binary, where the
scratch lives, whether reflink works), lets you filter tests by substring, and
runs the suite across several worker processes.

Usage:
  tests/run.py                     run everything
  tests/run.py hardlink dedupe     only tests whose id contains a given string
  tests/run.py -j 8                run on 8 workers
  tests/run.py -j 1                force the plain serial unittest runner
  DUPEREMOVE=/path tests/run.py            test a specific binary
  DUPEREMOVE_TEST_DIR=/mnt/btrfs tests/run.py   choose the scratch filesystem

Parallelism defaults to `auto` (min(nproc, 8)); `-j 1` restores the old serial
runner verbatim. Every test already gets its own tempfile.mkdtemp() scratch with
its own hashfile inside it, so tests don't collide. Workers are *processes*, not
threads, because test_long_path chdir()s and cwd is process-global. Work is
handed out one test at a time, so the slow tests (autotune, vacuum) don't strand
a worker at the end.

Exit status is non-zero if any test fails.
"""

import argparse
import concurrent.futures as futures
import io
import os
import subprocess
import sys
import time
import unittest
from collections import Counter

HERE = os.path.dirname(os.path.abspath(__file__))
INTEGRATION_DIR = os.path.join(HERE, "integration")
sys.path.insert(0, INTEGRATION_DIR)

import harness  # noqa: E402  (needs the sys.path insert above)

# Past this the scratch filesystem, not the CPU, is the limiting factor.
MAX_AUTO_JOBS = 8


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
    """Parse -j: a positive count, or 'auto' for min(nproc, MAX_AUTO_JOBS)."""
    if value == "auto":
        return min(os.cpu_count() or 1, MAX_AUTO_JOBS)
    try:
        jobs = int(value)
    except ValueError:
        raise argparse.ArgumentTypeError(f"not a number or 'auto': {value}")
    if jobs < 1:
        raise argparse.ArgumentTypeError("must be >= 1")
    return jobs


def _run_one(test_id):
    """Run one test in this worker and return a picklable summary of it."""
    suite = unittest.TestLoader().loadTestsFromName(test_id)
    started = time.monotonic()
    result = unittest.TextTestRunner(stream=io.StringIO(), verbosity=0).run(suite)
    elapsed = time.monotonic() - started

    if result.errors:
        return "ERROR", elapsed, result.errors[0][1]
    if result.failures:
        return "FAIL", elapsed, result.failures[0][1]
    if result.skipped:
        return "skip", elapsed, result.skipped[0][1]
    return "ok", elapsed, ""


def _run_parallel(test_ids, jobs):
    counts = Counter()
    problems = []
    started = time.monotonic()

    with futures.ProcessPoolExecutor(max_workers=jobs) as pool:
        pending = {pool.submit(_run_one, tid): tid for tid in test_ids}
        for future in futures.as_completed(pending):
            test_id = pending[future]
            try:
                status, elapsed, detail = future.result()
            except Exception as exc:    # worker died: segfault, OOM, ...
                status, elapsed, detail = "ERROR", 0.0, f"worker died: {exc}"
            counts[status] += 1
            print(f"{status:<5} {test_id} ({elapsed:.2f}s)", flush=True)
            if status in ("FAIL", "ERROR"):
                problems.append((status, test_id, detail))

    for status, test_id, detail in problems:
        print("=" * 70)
        print(f"{status}: {test_id}")
        print("-" * 70)
        print(detail)

    print("-" * 70)
    print(f"Ran {sum(counts.values())} tests in "
          f"{time.monotonic() - started:.2f}s on {jobs} workers\n")
    if problems:
        print(f"FAILED (failures={counts['FAIL']}, errors={counts['ERROR']}, "
              f"skipped={counts['skip']})")
        return 1
    print(f"OK (skipped={counts['skip']})")
    return 0


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("-j", "--jobs", type=_jobs, default="auto",
                        help="worker processes, or 'auto' (default: auto)")
    parser.add_argument("patterns", nargs="*",
                        help="only run tests whose id contains one of these")
    # argparse applies `type` to a string default too, so this is already an int.
    args = parser.parse_args(argv[1:])
    jobs = args.jobs

    version = subprocess.run([harness.DUPEREMOVE, "--version"],
                             capture_output=True, text=True).stdout.strip()
    fstype = subprocess.run(["stat", "-f", "-c", "%T", harness.TEST_ROOT],
                            capture_output=True, text=True).stdout.strip()
    print("oans integration tests")
    print(f"  binary : {harness.DUPEREMOVE} ({version})")
    print(f"  scratch: {harness.TEST_ROOT} ({fstype})")
    print(f"  reflink: {'yes' if harness.REFLINK else 'no (dedupe tests will skip)'}")
    print(f"  jobs   : {jobs}")
    tsan = _tsan_note()
    if tsan:
        print(f"  sanitize: {tsan}")
    print(flush=True)

    loader = unittest.TestLoader()
    discovered = loader.discover(start_dir=INTEGRATION_DIR, pattern="test_*.py")
    tests = [t for t in _iter_tests(discovered) if _matches(t.id(), args.patterns)]

    if jobs == 1:
        result = unittest.TextTestRunner(verbosity=2).run(unittest.TestSuite(tests))
        return 0 if result.wasSuccessful() else 1
    return _run_parallel([t.id() for t in tests], jobs)


def _iter_tests(suite):
    for item in suite:
        if isinstance(item, unittest.TestSuite):
            yield from _iter_tests(item)
        else:
            yield item


if __name__ == "__main__":
    sys.exit(main(sys.argv))
