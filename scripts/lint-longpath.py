#!/usr/bin/env python3
"""Guard the long-path invariant (issue #117).

oans hashes and dedupes files whose absolute path exceeds PATH_MAX (4096) by
opening a reachable ancestor and openat-walking the rest (src/longpath.c). That
only holds as long as no syscall takes the path of a *scanned file* as a single
argument. Reintroducing one loses the feature in the nastiest possible way: the
kernel answers ENAMETOOLONG, the caller reads that as "gone" or "unreadable",
the file is silently dropped, and the run still exits 0.

So: every path-taking syscall in src/ must either go through a longpath_*
helper, be relative to a dirfd, or carry a waiver saying why its path can never
be a scanned file's.

    open(p, ...)                     -> flagged
    longpath_open(p, ...)            -> fine
    openat(dfd, name, ...)           -> fine (dirfd-relative)
    statx(0, p, ...)                 -> flagged (AT_FDCWD is a path lookup)
    statx(dirfd(dirp), name, ...)    -> fine

Waive a line by putting a comment containing

    longpath-ok: <why this path can never be a scanned file>

on that line or one of the 3 lines above it. Keep the reason concrete -- it is
the only record of why the exemption is safe.

Run standalone or via `make lint`. Exits 1 on any unwaived call.
"""

import re
import sys
from pathlib import Path

# Path-in-argument syscalls: always a path lookup, always flagged unless waived.
PATH_CALLS = (
    "open", "opendir", "creat", "stat", "lstat", "statfs", "realpath",
    "unlink", "rmdir", "access", "truncate", "chdir", "mkdir", "rename",
    "readlink", "chmod", "chown", "utimensat",
)

# *at() syscalls: a path lookup only when the dirfd is AT_FDCWD (or a literal 0),
# otherwise resolved relative to an already-open directory, which is what we want.
DIRFD_CALLS = (
    "statx", "openat", "fstatat", "unlinkat", "faccessat", "renameat",
    "readlinkat", "mkdirat", "utimensat",
)

# longpath.c *is* the sanctioned implementation of the walk, so it is the one
# file allowed to call the bare syscalls.
#
# The unit suite is out of scope rather than skipped: this walks src/*.c, and
# the tests moved to tests/unit/. They build their own scratch trees with
# whatever is convenient, and none of it is the product walking a user's
# filesystem - which is the only thing this rule is about.
SKIP_FILES = {"longpath.c"}

WAIVER = re.compile(r"longpath-ok:\s*(\S.*?)\s*$")
WAIVER_LOOKBACK = 3

_PATH_RE = re.compile(
    r"(?<![_A-Za-z0-9])(" + "|".join(PATH_CALLS) + r")\s*\(")
_DIRFD_RE = re.compile(
    r"(?<![_A-Za-z0-9])(" + "|".join(DIRFD_CALLS) + r")\s*\(\s*([^,]*),")


def strip_noise(src: str) -> str:
    """Blank out comments and string literals, preserving line structure.

    Prose mentioning "stat()" and paths inside literals are not calls; without
    this the linter is too noisy to keep enabled.
    """
    out = []
    i, n = 0, len(src)
    while i < n:
        two = src[i:i + 2]
        if two == "/*":
            j = src.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("".join(c if c == "\n" else " " for c in src[i:j]))
            i = j
        elif two == "//":
            j = src.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif src[i] in "\"'":
            quote, j = src[i], i + 1
            while j < n and src[j] != quote:
                j += 2 if src[j] == "\\" else 1
            j = min(j + 1, n)
            out.append("".join(c if c == "\n" else " " for c in src[i:j]))
            i = j
        else:
            out.append(src[i])
            i += 1
    return "".join(out)


def is_waived(raw_lines: list[str], idx: int) -> bool:
    lo = max(0, idx - WAIVER_LOOKBACK)
    return any(WAIVER.search(line) for line in raw_lines[lo:idx + 1])


def check_file(path: Path) -> list[tuple[int, str, str]]:
    raw = path.read_text().splitlines()
    code = strip_noise(path.read_text()).splitlines()
    hits = []

    for idx, line in enumerate(code):
        found = None

        m = _PATH_RE.search(line)
        if m:
            found = f"{m.group(1)}() takes a path"
        else:
            m = _DIRFD_RE.search(line)
            if m:
                dirfd = m.group(2).strip()
                if dirfd in ("0", "AT_FDCWD"):
                    found = f"{m.group(1)}() with {dirfd or '0'} is a path lookup"

        if found and not is_waived(raw, idx):
            hits.append((idx + 1, found, raw[idx].strip()))
    return hits


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    srcdir = root / "src"
    files = sorted(p for p in srcdir.glob("*.c") if p.name not in SKIP_FILES)

    total = 0
    for path in files:
        for lineno, why, text in check_file(path):
            rel = path.relative_to(root)
            print(f"{rel}:{lineno}: {why}")
            print(f"    {text}")
            total += 1

    if total:
        print(f"\n{total} unwaived path-taking call(s).", file=sys.stderr)
        print(
            "Reach scanned files via longpath_open/opendir/stat/lstat "
            "(src/longpath.h) or a dirfd.\n"
            "If the path can never be a scanned file's, add a comment "
            "'longpath-ok: <why>' on or just above the line.",
            file=sys.stderr)
        return 1

    print(f"lint-longpath: {len(files)} files clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
