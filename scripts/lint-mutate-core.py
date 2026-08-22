#!/usr/bin/env python3
"""`scripts/mutate/mutate_core.py` is vendored, and this is what says so out loud.

unordered_dense and nanobench each hold a byte-identical copy of that file and drive it through
an adapter of their own. Sharing it is what lets one test suite -- unordered_dense's
`scripts/test_mutate.py`, which is where the core is developed -- cover the code all three
projects run, including the make backend and the minunit harness that only oans executes. A copy
edited in place ends that quietly: the suite over there stays green, oans runs something nothing
tests, and the three drift apart one convenient local fix at a time.

There is no way for a lint in one repository to see the others, so this checks the next best
thing: that the core still hashes to what was recorded when it was last vendored. Editing the
core means editing it in unordered_dense, running that suite, re-copying it here and into
nanobench, and updating all three `mutate_core.sha256` in the same commit -- after which "are
these in sync?" is a question `diff` can answer:

    diff <(cat oans/scripts/mutate/mutate_core.sha256) \\
         <(cat unordered_dense/scripts/mutate/mutate_core.sha256)

That is deliberately a hand check rather than a promise. What this can enforce is that nobody
changed the shared file without noticing that it is shared -- and, below, that the adapter beside
it still fits the core it is vendored against. A renamed hook is otherwise silent: `syntax_tu`
naming a file that has moved makes the pre-filter fail for every mutant, and the whole run comes
back `compiler`, which is a verdict in the flattering direction.
"""

import hashlib
import importlib.util
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
CORE = REPO / "scripts" / "mutate" / "mutate_core.py"
RECORDED = CORE.with_suffix(".sha256")
ADAPTER = CORE.parent / "mutate.py"


def main():
    if not CORE.exists():
        print("%s is missing" % CORE.relative_to(REPO))
        return 1

    digest = hashlib.sha256(CORE.read_bytes()).hexdigest()
    if not RECORDED.exists():
        print("%s is missing - write %s into it" % (RECORDED.relative_to(REPO), digest))
        return 1

    recorded = RECORDED.read_text(encoding="utf-8").split()[0]
    if digest != recorded:
        print("%s has changed since it was last vendored.\n"
              "  recorded  %s\n"
              "  actual    %s\n"
              "It is shared with unordered_dense and nanobench, and it is developed in the "
              "first of those - which is where its test suite lives. Make the change there, run "
              "scripts/test_mutate.py, copy the file back into all three and record the new "
              "hash in each:\n"
              "  echo %s > %s"
              % (CORE.relative_to(REPO), recorded, digest, digest,
                 RECORDED.relative_to(REPO)))
        return 1

    # The adapter against the core. `Project.problems()` is in the core rather
    # than written out here, so a newly required attribute reaches this
    # repository through the re-vendor that has to happen anyway.
    spec = importlib.util.spec_from_file_location("mutate_adapter", ADAPTER)
    adapter = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(adapter)
    project = adapter.Oans()
    problems = list(project.problems())
    if Path(adapter.mutate_core.__file__).resolve() != CORE:
        problems.append("the adapter imported %s rather than the vendored core beside it"
                        % adapter.mutate_core.__file__)
    if Path(project.repo) != REPO:
        problems.append("repo resolves to %s, not %s - every lane would copy the wrong tree"
                        % (project.repo, REPO))
    # Guarded, because `problems()` has already said if there is no backend at
    # all and a traceback on top of that message helps nobody read it.
    if project.backend is not None and project.backend.arg_flag != "--make-arg":
        problems.append("the pass-through flag is %s, not --make-arg" % project.backend.arg_flag)
    # The one thing this repository can check that the others cannot: the make
    # target the lanes build must not also *run* the suite, or a mutant the
    # tests caught exits nonzero at the build step and is scored `compiler`.
    target = getattr(project.backend, "target", None)
    if target and target not in (REPO / "Makefile").read_text(encoding="utf-8"):
        problems.append("the backend builds `%s`, which is not a target in the Makefile" % target)
    if problems:
        print("%s does not fit the core it is vendored with:" % ADAPTER.relative_to(REPO))
        for problem in problems:
            print("  %s" % problem)
        return 1

    # A core that does not import is a broken vendor whatever it hashes to, and
    # the adapter is what a person actually runs. --help exercises both, plus
    # every argparse default that a typo in the project object would break.
    r = subprocess.run([sys.executable, str(ADAPTER), "--help"],
                       capture_output=True, text=True)
    if r.returncode != 0:
        print("%s --help failed:\n%s%s" % (ADAPTER.relative_to(REPO), r.stdout, r.stderr))
        return 1

    print("lint-mutate-core: vendored at %s, adapter fits" % digest[:12])
    return 0


if __name__ == "__main__":
    sys.exit(main())
