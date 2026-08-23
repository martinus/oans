#!/usr/bin/env python3
"""Read a mutation run's --json report by function, and diff two of them.

CLAUDE.md says to read a survivor count by function and never as a total, and
every sweep in this tree has proved it: src/util.c came back 213 survivors of
413, which says nothing at all - grouped, it was 5-9% survival everywhere a
test existed and 85-94% in four pure functions that had no test, and only the
grouping tells an absent test from a weak one. The tool prints the total. This
prints what the total hides.

    scripts/mutate/report.py sweep.json                 # survivors by function
    scripts/mutate/report.py before.json after.json     # what a change moved

The diff exists because "the survivor count went down" is not the claim worth
making. What is worth making is which mutants went, and whether any arrived:
a run that kills six and loses two reports the same -4 as one that kills four.

Two traps this encodes, both of which were hit by hand before it existed:

  - A before/after pair whose source differs cannot be diffed per mutant. Line
    numbers move, so mapping one run's lines through the other's function map
    silently attributes survivors to whatever now sits at that line - which
    once produced a confident report of a regression in a function that had
    not been touched. When the mutant populations differ, this falls back to
    per-function counts and says so rather than guessing.
  - A function with no mutants at all is not a function with no survivors. It
    is usually a function the sweep never reached, and it is listed separately.
"""

import json
import sys
from pathlib import Path


def function_map(path):
    """Line number (1-based) -> enclosing function name, for kernel-style C.

    Keyed off `{` alone in column 0 rather than a regex over the declaration:
    a signature wrapped across lines (`storage_recommend_io_threads` is one)
    leaves its continuation looking nothing like a definition, and a regex that
    matches it anyway also matches half the `if` statements in the file.
    """
    lines = path.read_text().split("\n")
    names = ["<file scope>"] * (len(lines) + 1)
    i = 0
    while i < len(lines):
        if lines[i] != "{":
            i += 1
            continue
        # Walk back over the signature to the line that starts it.
        start = i - 1
        while start > 0:
            prev = lines[start - 1].strip()
            # `*/` and `*` stop it as surely as `;` does: without them the walk
            # runs up into the doc comment, and a comment mentioning a word
            # before a `(` becomes the function's name. read_rotational's
            # comment says "via the parent (\"..\"", and that is what the
            # first version of this reported for every one of its 36 mutants.
            if (prev == "" or prev.startswith(("#", "*", "/*"))
                    or prev.endswith((";", "}", "{", "*/"))):
                break
            start -= 1
        decl = " ".join(lines[start:i])
        name = None
        if "(" in decl:
            head = decl[: decl.index("(")].replace("*", " ").strip()
            if head:
                name = head.split()[-1]
        # Body runs to the next `}` in column 0.
        end = i + 1
        while end < len(lines) and lines[end] != "}":
            end += 1
        if name:
            for ln in range(start + 1, min(end, len(lines)) + 2):
                names[ln] = name
        i = end + 1
    return names


def load(p):
    d = json.loads(Path(p).read_text())
    src = d.get("environment", {}).get("file")
    if not src:
        sys.exit(f"{p}: no environment.file - not a mutation report?")
    return d, src


def grouped(results, names):
    """{function: [survived, total]}, in file order."""
    out = {}
    for m in results:
        ln = m.get("line", 0)
        fn = names[ln] if 0 < ln < len(names) else "<unknown line>"
        slot = out.setdefault(fn, [0, 0])
        slot[1] += 1
        if m["verdict"] == "survived":
            slot[0] += 1
    return out


def population(results):
    """The (line, name) multiset a run generated. Equal populations mean the
    two runs saw the same source, which is what makes a per-mutant diff sound."""
    return sorted((m.get("line", 0), m["name"]) for m in results)


def report_one(path):
    d, src = load(path)
    names = function_map(Path(src))
    g = grouped(d["results"], names)
    counts = d["counts"]
    total = sum(counts.values())
    print(f"{src}: {counts['survived']} survivors of {total}"
          f"  (caught {counts['caught']}, compiler {counts['compiler']},"
          f" hang {counts['hang']}, oom {counts['oom']})\n")
    print(f"  {'function':38s} {'survived':>9s} {'of':>5s}  {'kill rate':>9s}")
    for fn, (s, t) in sorted(g.items(), key=lambda kv: -kv[1][0]):
        # A function whose mutants are all `compiler` says nothing about tests.
        scored = t - sum(1 for m in d["results"]
                         if names[m.get("line", 0)] == fn
                         and m["verdict"] == "compiler")
        rate = f"{100 * (scored - s) // scored:d}%" if scored else "-"
        print(f"  {fn:38s} {s:9d} {t:5d}  {rate:>9s}")


def report_diff(before, after):
    db, sb = load(before)
    da, sa = load(after)
    if sb != sa:
        sys.exit(f"different files swept ({sb} vs {sa}) - nothing to compare")
    names = function_map(Path(sa))
    gb, ga = grouped(db["results"], names), grouped(da["results"], names)

    print(f"{sa}: {db['counts']['survived']} -> {da['counts']['survived']}"
          f" survivors\n")
    print(f"  {'function':38s} {'before':>7s} {'after':>7s} {'':>6s}")
    for fn in sorted(set(gb) | set(ga),
                     key=lambda f: -(gb.get(f, [0])[0] - ga.get(f, [0])[0])):
        b, a = gb.get(fn, [0, 0])[0], ga.get(fn, [0, 0])[0]
        if b == a == 0:
            continue
        mark = "" if b == a else ("  <-- worse" if a > b else "")
        print(f"  {fn:38s} {b:7d} {a:7d} {mark}")

    if population(db["results"]) != population(da["results"]):
        print("\nThe two runs generated different mutants, so the source"
              "\nchanged between them and per-mutant lines would be"
              "\nmisattributed. Per-function counts above are the whole answer.")
        return

    sb_ = {(m["line"], m["name"]) for m in db["results"] if m["verdict"] == "survived"}
    sa_ = {(m["line"], m["name"]) for m in da["results"] if m["verdict"] == "survived"}
    src_lines = Path(sa).read_text().split("\n")

    def show(title, keys):
        if not keys:
            return
        print(f"\n{title}:")
        for ln, nm in sorted(keys):
            text = src_lines[ln - 1].strip() if 0 < ln <= len(src_lines) else ""
            print(f"  {names[ln]:32s} {ln:5d}  {nm:16s} | {text[:44]}")

    show("killed by the change", sb_ - sa_)
    show("NEWLY SURVIVING - a test stopped covering these", sa_ - sb_)


def main(argv):
    if len(argv) == 2:
        report_one(argv[1])
    elif len(argv) == 3:
        report_diff(argv[1], argv[2])
    else:
        sys.exit(__doc__.strip().split("\n\n")[0] + "\n\n"
                 "usage: report.py <run.json> [<after.json>]")


if __name__ == "__main__":
    main(sys.argv)
