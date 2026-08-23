#!/usr/bin/env python3
"""Every test in tests/unit/ is actually run, and every test run exists.

This restores a guarantee the multi-translation-unit split removed.

While the suite was one TU, `MU_TEST` expanded to a *static* function, so a test
nobody ran was an unused function and `WERROR=1` failed the build naming it.
Giving tests external linkage - which is what lets main.c keep the run order
while the bodies live in other TUs - ends that: an orphan test now compiles
clean, links clean as an unused extern, and never runs. Measured on the commit
that introduced it, with a body of `mu_check(1 == 2)`: `make WERROR=1
test-build` exits 0 and the suite reports green.

That is the shape this repository's tooling exists to catch, so it is checked
here rather than left to prose. `scripts/lint-escape.py` exists for the same
reason, worded the same way.

The reverse direction - a test run but not defined - the linker already
catches, and a test declared but not defined cannot happen since MU_RUN
declares it at the point it runs it. So there is one question left here, and it
is the one nothing else asks.

The check is on the *sets*, never the order. The run order in main.c is chosen
by hand and is load-bearing - every memdb() handle opens the same shared-cache
in-memory database, so some dbfile tests must run before anything has stored a
row - so this must not push the list towards matching the file layout.
"""

import re
import sys
from pathlib import Path

UNIT = Path(__file__).resolve().parent.parent / "tests" / "unit"

DEFINED = re.compile(r"^MU_TEST\((\w+)\)", re.M)
RUN = re.compile(r"^\s*MU_RUN\((\w+)\);", re.M)


def main():
    defined = {}
    for path in sorted(UNIT.glob("test_*.c")):
        for name in DEFINED.findall(path.read_text()):
            defined[name] = path.name
    main_c = (UNIT / "main.c").read_text()
    run = set(RUN.findall(main_c))

    problems = []
    for name in sorted(set(defined) - run):
        problems.append("%s defines %s, which main.c never runs"
                        % (defined[name], name))
    for name in sorted(run - set(defined)):
        problems.append("main.c runs %s, which no test file defines" % name)

    if problems:
        sys.stderr.write("tests/unit/ registry is inconsistent:\n")
        for p in problems:
            sys.stderr.write("  %s\n" % p)
        sys.stderr.write("\nA test that is defined but not run is silently not "
                         "run: since the suite\nbecame several translation "
                         "units its function is extern, so nothing\nwarns.\n")
        return 1
    print("lint-test-registry: %d tests, all defined and run" % len(defined))
    return 0


if __name__ == "__main__":
    sys.exit(main())
