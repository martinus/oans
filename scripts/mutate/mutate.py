#!/usr/bin/env python3
"""What mutation testing means for oans in particular.

`--help` prints the shared manual from mutate_core.py first; this is the part
underneath it. The file mutated by default is `src/csum.c`, the suite is the C
unit binary `./test`, and make builds the lanes.

    scripts/mutate/mutate.py --bugs scripts/mutate/bugs/fiemap.txt
    scripts/mutate/mutate.py --replace 'OLD CODE' 'NEW CODE'
    scripts/mutate/mutate.py --file src/glob.c --diff   # whatever is uncommitted
    scripts/mutate/mutate.py --file src/fiemap.c --lines 120-190

**The target is any C file, and `--file` is how you say which.** oans is not a
single-header library, so unlike the other two projects that vendor this core
there is no one file that is obviously *the* code. The default is only a default;
a run without `--file` is almost always the wrong question.

**What this can and cannot see.** The suite it scores against is `make test` -
`src/tests.c`, which `#include`s the other sources so that it can reach their
static functions. That is the whole of what a mutant is measured by here, and
what it leaves out is the end-to-end Python suite in `tests/`, which needs a
btrfs or XFS scratch directory and drives the `oans` binary rather than linking
its code. So a survivor means "nothing in the C unit tests noticed", which is a
narrower claim than the report's wording, and for anything the integration suite
is the real cover for - the dedupe phase, the scan pipeline, the progress block -
it is the wrong question to have asked. The pure functions are where this earns
its keep: fiemap arithmetic, the glob compiler, `sanitize_ctrl`, the checksum
state, the scan work queue.

A mutant costs one compile of `src/tests.c` and one run of the suite, which is
about five seconds - the suite itself is four milliseconds, so this is a build
harness with a test run attached. There is no incremental build to be had: every
source is `#include`d into that one translation unit, so any mutant rebuilds all
of it. That is also why the `-fsyntax-only` pre-filter is worth having, at about
a tenth of the cost of the build it replaces.

`--make-arg` is how you ask a different question - whether a sanitizer build
catches what the plain one cannot. make remembers nothing between invocations,
so these ride on the build line itself and there is no configured state to get
out of step with them:

    scripts/mutate/mutate.py --file src/fiemap.c --make-arg=SANITIZE=address,undefined --make-arg=CC=clang
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import mutate_core  # noqa: E402 - the path above is what makes it importable


class Oans(mutate_core.Project):
    slug = "oans"
    repo = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))

    # There is no single-file library here, so this is a starting point rather
    # than the answer: `src/csum.c` because the hashing path is the one place a
    # wrong result is invisible downstream - nothing can tell a wrong digest
    # from a file that simply had no duplicate.
    target = os.path.join("src", "csum.c")

    # The only translation unit there is. `src/tests.c` includes the other
    # sources rather than linking them, which is how the suite reaches their
    # static functions - so it is both what the pre-filter compiles and what
    # every mutant rebuilds.
    syntax_tu = os.path.join("src", "tests.c")

    # make builds in the tree, so a lane's build directory is the lane.
    build_dir = "."
    test_binary = "test"
    # `test-build` and not `test`: the latter builds *and runs* the binary, so a
    # mutant the suite caught would exit nonzero at the build step and be scored
    # `compiler` - the compiler credited with protection the tests provided.
    backend = mutate_core.MakeBackend("test-build")
    harness = mutate_core.MinunitHarness()

    compiler_env = "CC"
    compiler_probe = "cc"

    # Ignored so a lane copies sources and not artifacts. `oans`, `test` and
    # `.version-stamp` are build outputs at the root; `*.d` are the makefile's
    # own dependency files, which name absolute paths in whatever tree they were
    # generated in and would have make rebuilding against a directory that is
    # not this lane.
    ignore = mutate_core.Project.ignore + ("*.d",)
    root_ignore = ("oans", "test", ".version-stamp", ".vglogs", ".pandoc",
                   ".itest-scratch", "*.tar.gz")

    # Measured: ~4 MB of sources, ~13 MB once built (one binary with -ggdb).
    lane_bytes = 24 * mutate_core.MIB

    # Measured on this tree, gcc 13 at -O2: one mutant is 4.4 s of compiling
    # `src/tests.c`, 0.5 s to link and 0.004 s to run the suite. Nearly all of
    # it is one serial compile inside one lane, so the lanes divide it and the
    # machine does not - the same shape as nanobench and the opposite of
    # unordered_dense's ~90 translation units.
    lane_seconds_per_mutant = 5.0
    overhead_seconds_per_mutant = 0.3
    setup_seconds = 8.0
    setup_seconds_per_lane = 0.2

    def lane_env(self, lane):
        """The suppressions the makefile's own sanitizer legs export.

`make test` wraps the run in `SANITIZE_RUN`, and running the binary
        directly - which is what this tool does, so that a failing suite is not
        a failing build - leaves that behind. Without them a `--make-arg
        SANITIZE=address` lane reports GLib's cached idle pool threads as leaks
        in every single mutant, and a suite that is red before any mutation is
        applied cannot score anything: the baseline refuses first, which is at
        least loud. TSan is worse, because it is not loud - GLib's internals
        come back as races and every mutant reads as `caught`.

        That file names three GLib functions and nothing wider. It used to
        carry a module-wide `leak:libglib-2.0`, which - since oans keeps nearly
        everything in GLib containers - suppressed most of oans's own heap:
        a `SANITIZE=address` sweep of src/glob.c came back with 66 survivors
        under it and 54 without, all 13 of the difference a deleted free or
        unref. If a survivor here is a missing deallocation, check what this
        file is matching before believing it.

        UBSAN_OPTIONS is deliberately not here; the core owns it, and setting it
        would drop the `halt_on_error=1` that makes UBSan fail a run rather than
        print and exit 0.
        """
        return {
            "LSAN_OPTIONS": "suppressions=" + os.path.join(lane.dir, "tests", "lsan.supp"),
            "TSAN_OPTIONS": "suppressions=" + os.path.join(lane.dir, "tests", "tsan.supp")
                            + ":halt_on_error=0:second_deadlock_stack=1",
        }

    def sanitizer(self, setup_args):
        """Which sanitizer a lane builds with, in this makefile's spelling.

        `SANITIZE=` and `DEBUG=1` are oans's own variables rather than anything
        make knows about, so the backend cannot answer this and says "unknown".
        The fingerprint prints "no sanitizer in this build" off the back of it,
        and that sentence is a claim about what the run could observe - a
        survivor read under it is being called a hole in the tests.
        """
        for arg in setup_args:
            if arg.startswith("SANITIZE="):
                return arg.split("=", 1)[1] or "none"
            if arg.startswith("DEBUG=") and not arg.endswith("="):
                return "address"  # what the makefile's DEBUG build turns on
        return "none"

    def default_syntax_tu(self, path):
        """Every source here is compiled through `src/tests.c`, so that is the
        pre-filter's TU whichever one is mutated - unlike the core's default,
        which would check a `.c` file on its own and miss anything that only
        breaks where the includes meet.

        Anything outside `src/` is not in that translation unit at all, and a
        check that compiles something the mutation cannot reach passes every
        time and quietly stops filtering.
        """
        if path.startswith("src" + os.sep) and path.endswith((".c", ".h")):
            return self.syntax_tu
        return None

    def extra_facts(self, args):
        """Whether the reflink scratch directory the *other* suite needs exists.

        Not used by this run, and said anyway: the number this prints is about
        the C unit tests alone, and someone reading a survivor later is owed the
        fact that the end-to-end suite was not part of the question.
        """
        return dict(test_dir=os.environ.get("DUPEREMOVE_TEST_DIR"))

    def extra_fingerprint(self, facts):
        rows = ["suite           src/tests.c (C unit tests only)"]
        notes = [
            "NOTE: the end-to-end suite in tests/ is not run here. It drives\n"
            "      the oans binary against a btrfs or XFS scratch tree, so a\n"
            "      `survived` means nothing in the C unit tests noticed - not\n"
            "      that nothing in oans would. For the dedupe phase, the scan\n"
            "      pipeline and the progress block, that is the wrong question."]
        return rows, notes


if __name__ == "__main__":
    mutate_core.run(Oans(), __doc__)
