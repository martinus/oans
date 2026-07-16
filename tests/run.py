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
