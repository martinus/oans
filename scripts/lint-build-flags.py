#!/usr/bin/env python3
"""WERROR=1 must not change what the compiler optimizes.

This exists because it silently did, for however long CI has been running.

`ifdef WERROR` does `override CFLAGS += -Werror` near the top of the Makefile,
and once a variable carries the `override` origin GNU make **ignores every
later ordinary assignment to it**. The release block's `CFLAGS += -O2
$(HARDENING)` sat below that, so with WERROR set it evaporated - no warning, no
error, just a different binary.

.github/workflows/ci.yml sets `WERROR: 1` for every job, so every check this
project runs - btrfs, xfs, the four valgrind shards, the three sanitizer legs,
the mutation sweep - was verifying -O0 code while what ships is -O2. That is
the configuration where UB-sensitive differences hide, and CLAUDE.md already
records that 17 of dbfile.c's 1,879 mutants differ between the two builds.

Nothing noticed because a build that is merely *slower and less checked* still
passes every test. A comment would rot the same way the original did, so the
invariant is asserted instead: ask make what it would run, and compare.
"""

import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
OPT = re.compile(r"(?:^|\s)(-O[0-3sgz])(?:\s|$)")


def first_compile_flags(env_extra):
    """The -O level make would use for a test object.

    `-B` because `make -n` alone prints nothing when the tree is already
    built, and a check that silently examines an empty command list would
    pass for the wrong reason.
    """
    out = subprocess.run(["make", "-Bn", "test-build"], cwd=REPO, text=True,
                         capture_output=True,
                         env={**dict(__import__("os").environ), **env_extra})
    for line in out.stdout.split("\n"):
        if line.lstrip().startswith(("cc ", "gcc ", "clang ", "$(CC)")) and ".c" in line:
            found = OPT.findall(line)
            return found[-1] if found else None
    return None


def main():
    plain = first_compile_flags({})
    werror = first_compile_flags({"WERROR": "1"})

    if plain is None:
        sys.stderr.write("lint-build-flags: no optimization flag in a plain "
                         "build - the release block is not being applied\n")
        return 1
    if werror != plain:
        sys.stderr.write(
            "lint-build-flags: WERROR changes the optimization level\n"
            "  plain build:    %s\n  WERROR=1 build: %s\n\n"
            "Almost certainly the `override` bug again: `ifdef WERROR` marks\n"
            "CFLAGS with the override origin, and make then ignores any later\n"
            "plain `CFLAGS +=`. The release block must use `override` too.\n"
            "CI sets WERROR=1 for every job, so this makes every check run\n"
            "against a build that is not the one being shipped.\n"
            % (plain, werror or "no -O at all"))
        return 1
    print("lint-build-flags: WERROR keeps %s" % plain)
    return 0


if __name__ == "__main__":
    sys.exit(main())
