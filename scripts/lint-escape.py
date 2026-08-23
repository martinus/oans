#!/usr/bin/env python3
"""Guard the escaped-output invariant (issue #202).

A file name is untrusted input: anyone who can create a file inside a scanned
tree chooses the bytes oans writes to the administrator's terminal. A `\\r` in a
name rewrites the line above it - the summary, a progress row - so a name can
spoof "Reclaimed 9 TiB"; an `ESC[2J` clears the screen. So every path oans
prints goes through sanitize_ctrl()/path_for_display(), usually via the
declare_display_path() macro.

That rule is prose, and prose rots: three print sites were missed in the very
commit that introduced it. This turns it into a build failure.

A print call is flagged when a path-typed identifier is passed to one of the
printf-family macros:

    eprintf("cannot open %s\\n", path)          -> flagged
    declare_display_path(disp, path);
    eprintf("cannot open %s\\n", disp)          -> fine
    printf("%s\\n", extent->e_file->filename)   -> flagged

"Path-typed" is by name (PATH_NAMES below), which is what makes this a cheap
regex lint rather than a type system. It is deliberately narrow: it catches the
shapes this codebase actually writes, not every conceivable one.

Waive a line by putting a comment containing

    escape-ok: <why this string can never be a scanned file's name>

on that line or one of the 3 lines above it. Keep the reason concrete -- it is
the only record of why the exemption is safe. The usual reason is that the
string is oans's own argument (a hashfile path) rather than something found
during a walk.

Run standalone or via `make lint`. Exits 1 on any unwaived call.
"""

import re
import sys
from pathlib import Path

# The print macros in src/debug.h, plus bare printf for the report paths that
# do not route through them.
PRINT_CALLS = ("printf", "eprintf", "vprintf", "qprintf", "dprintf",
               "g_string_append_printf")

# Identifiers that hold a path taken from the scanned tree. Names, not types --
# see the module docstring.
PATH_NAMES = (
    r"path", r"in_path", r"child", r"filename", r"name",
    r"\w+->path", r"\w+->filename", r"\w+->d_name", r"\w+\.path",
    r"\w+->e_file->filename", r"roots\[\w+\]", r"sc->roots\[\w+\]",
    r"top\[\w+\]\.path", r"excludes\[\w+\]", r"sc\.excludes\[\w+\]",
    r"sc->excludes\[\w+\]",
)

# util.c defines the escapers, so it is where the raw bytes are allowed.
#
# The unit suite is out of scope rather than skipped: this walks src/*.c, and
# the tests moved to tests/unit/. A path a test prints is one the test made
# up, not one found in a walk, and this rule is about untrusted names only.
SKIP_FILES = {"util.c"}

WAIVER = re.compile(r"escape-ok:\s*(\S.*?)\s*$")
WAIVER_LOOKBACK = 3

_PRINT_RE = re.compile(
    r"(?<![_A-Za-z0-9])(" + "|".join(PRINT_CALLS) + r")\s*\(")
_ARG_RE = re.compile(
    r"(?<![_A-Za-z0-9.>\[])(" + "|".join(PATH_NAMES) + r")\s*(?=[,)])")
# Whatever declare_display_path() introduced is escaped by construction, even
# when it is spelled with an otherwise path-shaped name like `name`.
_SAFE_RE = re.compile(r"declare_display_path\s*\(\s*(\w+)\s*,")


def strip_noise(src: str) -> str:
    """Blank out comments and string literals, preserving line structure.

    A format string mentioning a path, or prose in a comment, is not an
    argument; without this the linter is too noisy to keep enabled.
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


def call_spans(code: str):
    """Yield (start_line, end_line, argument_text) for each print call.

    A print call's arguments routinely wrap over several lines, so the whole
    parenthesised argument list is one unit -- matching line by line would miss
    every wrapped path argument, which is most of them.
    """
    for m in _PRINT_RE.finditer(code):
        depth, i, n = 1, m.end(), len(code)
        while i < n and depth:
            if code[i] == "(":
                depth += 1
            elif code[i] == ")":
                depth -= 1
            i += 1
        yield (code.count("\n", 0, m.start()),
               code.count("\n", 0, i),
               code[m.end():i - 1])


def is_waived(raw_lines: list[str], first: int, last: int) -> bool:
    lo = max(0, first - WAIVER_LOOKBACK)
    return any(WAIVER.search(line) for line in raw_lines[lo:last + 1])


def check_file(path: Path) -> list[tuple[int, str, str]]:
    text = path.read_text()
    raw = text.splitlines()
    code = strip_noise(text)
    safe = set(_SAFE_RE.findall(code))
    hits = []

    for first, last, args in call_spans(code):
        m = next((m for m in _ARG_RE.finditer(args)
                  if m.group(1) not in safe), None)
        if m and not is_waived(raw, first, last):
            hits.append((first + 1, f"{m.group(1)} printed unescaped",
                         raw[first].strip()))
    return hits


def main() -> int:
    src = Path(__file__).resolve().parent.parent / "src"
    files = sorted(p for p in src.glob("*.c") if p.name not in SKIP_FILES)
    total = 0

    for path in files:
        for line, why, text in check_file(path):
            print(f"{path}:{line}: {why}\n    {text}", file=sys.stderr)
            total += 1

    if total:
        print(f"\nlint-escape: {total} unescaped path print(s). Use "
              f"declare_display_path(), or waive with 'escape-ok: <why>'.",
              file=sys.stderr)
        return 1
    print(f"lint-escape: {len(files)} files clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
