#!/usr/bin/env python3
"""Every test hook goes through test_hook_env() (#295).

`make TEST_HOOKS=0` builds oans without the DUPEREMOVE_* test hooks, and
test_hook_env() is the one place that makes that true: it answers NULL in such
a build, so the hook cannot be set and the compiler drops the code behind it. A
hook read with a bare getenv() would still be live there, and nothing would
say so. This flags every getenv() of a DUPEREMOVE_* name except the two that
are deliberately in every build.
"""

import re
import sys
from pathlib import Path

# Read in every build: a diagnostic, and the layout copy's kill switch.
NOT_HOOKS = {"DUPEREMOVE_SCAN_STATS", "DUPEREMOVE_NO_LAYOUT_COPY"}

GETENV = re.compile(r'(?<![_A-Za-z0-9])getenv\s*\(\s*"(DUPEREMOVE_[A-Z_]+)"')


def main() -> int:
    src = Path(__file__).resolve().parent.parent / "src"
    bad = []
    for path in sorted(src.glob("*.[ch]")):
        for n, line in enumerate(path.read_text().splitlines(), 1):
            for m in GETENV.finditer(line):
                if m.group(1) not in NOT_HOOKS:
                    bad.append(f"{path}:{n}: {m.group(1)} read with getenv(); "
                               f"use test_hook_env()")
    if bad:
        print("\n".join(bad), file=sys.stderr)
        return 1
    print("lint-test-hooks: every DUPEREMOVE_* hook goes through "
          "test_hook_env()")
    return 0


if __name__ == "__main__":
    sys.exit(main())
