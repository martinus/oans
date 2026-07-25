#!/usr/bin/env python3
"""Run the oans integration tests.

Thin wrapper over unittest that prints a short banner (which binary, where the
scratch lives, whether reflink works) and lets you filter tests by substring.

Usage:
  tests/run.py                     run everything
  tests/run.py hardlink dedupe     only tests whose id contains a given string
  DUPEREMOVE=/path tests/run.py            test a specific binary
  DUPEREMOVE_TEST_DIR=/mnt/btrfs tests/run.py   choose the scratch filesystem

Equivalent to `python3 -m unittest discover -s tests/integration`, minus the
banner and filtering. Exit status is non-zero if any test fails.
"""

import os
import subprocess
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
INTEGRATION_DIR = os.path.join(HERE, "integration")
sys.path.insert(0, INTEGRATION_DIR)

import harness  # noqa: E402  (needs the sys.path insert above)


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


def main(argv):
    patterns = argv[1:]

    version = subprocess.run([harness.DUPEREMOVE, "--version"],
                             capture_output=True, text=True).stdout.strip()
    fstype = subprocess.run(["stat", "-f", "-c", "%T", harness.TEST_ROOT],
                            capture_output=True, text=True).stdout.strip()
    print("oans integration tests")
    print(f"  binary : {harness.DUPEREMOVE} ({version})")
    print(f"  scratch: {harness.TEST_ROOT} ({fstype})")
    print(f"  reflink: {'yes' if harness.REFLINK else 'no (dedupe tests will skip)'}")
    tsan = _tsan_note()
    if tsan:
        print(f"  sanitize: {tsan}")
    print(flush=True)

    loader = unittest.TestLoader()
    discovered = loader.discover(start_dir=INTEGRATION_DIR, pattern="test_*.py")

    # Flatten and apply the substring filter.
    suite = unittest.TestSuite()
    for test in _iter_tests(discovered):
        if _matches(test.id(), patterns):
            suite.addTest(test)

    result = unittest.TextTestRunner(verbosity=2).run(suite)
    return 0 if result.wasSuccessful() else 1


def _iter_tests(suite):
    for item in suite:
        if isinstance(item, unittest.TestSuite):
            yield from _iter_tests(item)
        else:
            yield item


if __name__ == "__main__":
    sys.exit(main(sys.argv))
