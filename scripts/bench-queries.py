#!/usr/bin/env python3
"""bench-queries.py - time the dedupe-phase SQL against large hashfiles.

The dedupe phase starts with one estimate (dbfile_count_dupe_work) and then
loads one pass of duplicates at a time (the GET_DUPLICATE_* loaders). On a
small hashfile all of that takes milliseconds, so bench.py and bench-dedupe.py
cannot see it. On a 10 GB hashfile it took over an hour (#260). This tool runs
exactly those queries, taken from src/dbfile.c, against large synthetic
hashfiles, and reports two numbers for each:

  - wall time, and
  - temp writes: the bytes this process writes while the query runs. The
    hashfile is opened read-only, so that is SQLite's temp files. On a fast
    disk they cost little time; on a hard disk they were the whole problem.

It compares two versions of the SQL - `--base REF` (default origin/master)
against the working tree - and checks that both give the same result:

  - an estimate must return the same (groups, bytes),
  - a loader must return the same members in the same order within each group,
    because the first member of a group is its dedupe target.

    scripts/bench-queries.py                       # all profiles, all cases
    scripts/bench-queries.py -p fragmented -q count-extents
    scripts/bench-queries.py --scale 0.1           # small and quick
    scripts/bench-queries.py --base HEAD~1 --rounds 3

The hashfiles are generated once and cached in --workdir. Each profile is one
shape a real hashfile can have; add one to PROFILES and nothing else changes.
The cases are the three kinds of run: a first scan (everything is new), half
of the hashfile new, and one new generation (a scheduled run).

Needs: python3, git and a C preprocessor (cc -E) to expand the SQL macros. The
numbers are from the SQLite that Python links, which may not be the one oans
links; the tool prints its version.
"""

import argparse
import ast
import collections
import os
import random
import re
import sqlite3
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SRC = "src/dbfile.c"

# process_duplicates(): DEDUPE_FILES_PER_PASS / the default --batchsize.
GENERATION = 1024
PASS_GENERATIONS = 64 * 1024 // GENERATION

# Queries: name -> (C expression that yields the SQL, how to compare results).
# "count" compares the result row; a loader compares the member sequence of
# each group, keyed by the columns that make the group.
LOADERS = {
    "load-files": ("GET_DUPLICATE_FILES", lambda r: (r[2], r[1])),
    "load-extents": ("GET_DUPLICATE_EXTENTS", lambda r: (r[0], r[3])),
    "load-blocks": ("GET_DUPLICATE_BLOCKS", lambda r: r[0]),
}
COUNTS = {
    # (expression for seq_lo > 0, expression for seq_lo == 0), as in
    # dbfile_count_dupe_work(). Newest spelling first: the first pair that
    # expands in a version is the one it runs, so an older --base still works.
    "count-files": [("COUNT_FILES_WORK_SINCE", "COUNT_FILES_WORK_ALL"),
                    ("COUNT_FILES_WORK(FILES_GROUP_IS_NEW)",
                     'COUNT_FILES_WORK("1")')],
    "count-extents": [("COUNT_EXTENTS_WORK_SINCE", "COUNT_EXTENTS_WORK_ALL"),
                      ("COUNT_EXTENTS_WORK(EXTENTS_GROUP_IS_NEW)",
                       'COUNT_EXTENTS_WORK("1")')],
}
QUERIES = list(COUNTS) + list(LOADERS)
CASES = ["first", "half", "one"]
SCHEMA = ["CREATE_TABLE_FILES", "CREATE_TABLE_EXTENTS", "CREATE_TABLE_BLOCKS",
          "CREATE_FILES_DEDUPESEQ_INDEX", "CREATE_SEARCH_INDEXES"]


# --------------------------------------------------------------------------- #
# SQL, taken from the C source.
# --------------------------------------------------------------------------- #
def expand(source, exprs):
    """Expand C string macros with the preprocessor.

    Takes the file's string macros (a #define whose body holds a string
    literal, continuation lines included), appends `const char *Q = <expr>;`,
    and reads the concatenated string literals back out of `cc -E`, once per
    expression. Only those #defines are kept, so no header or code is
    needed; the other macros are left out because some of them use `#` in a
    way the preprocessor rejects when it sees them without their context.
    Returns {expr: sql or None}.
    """
    defs = [d for d in re.findall(r"^[ \t]*(#define(?:[^\n]*\\\n)*[^\n]*)",
                                  source, re.M)
            if '"' in d and "#" not in d[len("#define"):]]
    body = "\n".join(defs)
    cc = os.environ.get("CC", "cc")
    res = {}
    # One expression per run: an expression written for another version can
    # call a macro with the wrong number of arguments, and that must not
    # spoil the others.
    for e in exprs:
        out = subprocess.run([cc, "-E", "-P", "-x", "c", "-"],
                             input=f"{body}\nconst char *Q = {e};",
                             capture_output=True, text=True).stdout
        m = re.search(r"const char \*Q = (.*);", out, re.S)
        expr = m.group(1) if m else ""
        lit = r'"((?:[^"\\]|\\.)*)"'
        # Anything but string literals left over - a macro this version lacks,
        # or one the filter above dropped - means the SQL is not all there.
        if not expr.strip() or re.sub(lit, "", expr).strip():
            res[e] = None
            continue
        res[e] = "".join(ast.literal_eval('"%s"' % l)
                         for l in re.findall(lit, expr))
    return res


def sql_for(source):
    exprs = [LOADERS[q][0] for q in LOADERS] + SCHEMA + \
            [e for pairs in COUNTS.values() for pair in pairs for e in pair]
    return expand(source, exprs)


def source_at(ref):
    if ref is None:
        return (REPO / SRC).read_text()
    return subprocess.run(["git", "-C", str(REPO), "show", f"{ref}:{SRC}"],
                          check=True, capture_output=True, text=True).stdout


# --------------------------------------------------------------------------- #
# Hashfiles. Each generator writes the rows a scan would: files in
# generations of GENERATION, then extents and blocks, then the indexes.
# --------------------------------------------------------------------------- #
def gen_blocks(add, n, rnd):
    """Many blocks per file (--dedupe-options=partial): 60 blocks, 1 extent.

    A fifth of the files share a whole-file digest with others, a fifth of the
    blocks come from a shared pool, so block groups span many files and many
    generations.
    """
    fpool = [rnd.randbytes(16) for _ in range(max(1, n // 10))]
    bpool = [rnd.randbytes(16) for _ in range(max(1, n * 3))]
    for i in range(1, n + 1):
        whole = rnd.random() < 0.2
        add.file(i, rnd.choice(fpool) if whole else rnd.randbytes(16), 60, 1)
        add.extent(rnd.randbytes(16), i, 0, 60 * 4096)
        for b in range(60):
            d = rnd.choice(bpool) if rnd.random() < 0.2 else rnd.randbytes(16)
            add.block(d, i, b * 4096)


def gen_fragmented(add, n, rnd):
    """Many extents per file: 60 extents of 4 KiB, 30% from a shared pool.

    This is the shape a long-deduped tree grows into (see bench.py's
    `fragmented` profile), and the one where per-extent costs show.
    """
    fpool = [rnd.randbytes(16) for _ in range(max(1, n // 10))]
    epool = [rnd.randbytes(16) for _ in range(max(1, n * 3))]
    for i in range(1, n + 1):
        whole = rnd.random() < 0.2
        add.file(i, rnd.choice(fpool) if whole else rnd.randbytes(16), 60, 60)
        for e in range(60):
            d = rnd.choice(epool) if rnd.random() < 0.3 else rnd.randbytes(16)
            add.extent(d, i, e * 4096, 4096)


def gen_snapshots(add, n, rnd):
    """The second half of the files are copies of the first half.

    Each copy has the same 30 extents as its original except the first, so
    whole files differ and every extent group spans the two halves - the
    shape of a tree next to its snapshot, and of every incremental run over
    one.
    """
    half = n // 2
    first = {}
    for i in range(1, n + 1):
        add.file(i, rnd.randbytes(16), 30, 30)
        for e in range(30):
            if i > half and e:
                d = first[(i - half, e)]
            else:
                d = rnd.randbytes(16)
                if i <= half:
                    first[(i, e)] = d
            add.extent(d, i, e * 4096, 4096)


def gen_manyfiles(add, n, rnd):
    """Many small files: one extent each, a tenth in duplicate groups.

    A source tree or a mail spool. The count of files, not extents, is what
    grows here: the #260 report had 1.6M of them. Copies of a group are spread
    over random generations, as an incremental scan finds them.
    """
    i = 0
    while i < n:
        if rnd.random() < 0.04:
            dg, ed = rnd.randbytes(16), rnd.randbytes(16)
            for _ in range(rnd.randint(2, 4)):
                i += 1
                add.file(i, dg, 1, 1, seq=rnd.randint(1, n // GENERATION + 1))
                add.extent(ed, i, 0, 4096)
        else:
            i += 1
            add.file(i, rnd.randbytes(16), 1, 1)
            add.extent(rnd.randbytes(16), i, 0, 4096)


# name -> (generator, files at --scale 1)
PROFILES = {
    "blocks": (gen_blocks, 100_000),
    "fragmented": (gen_fragmented, 64_000),
    "snapshots": (gen_snapshots, 128_000),
    "manyfiles": (gen_manyfiles, 2_000_000),
}
GEN_VERSION = 1   # bump when a generator changes, so cached hashfiles are rebuilt


class Rows:
    """Buffers rows and writes them in large batches."""

    def __init__(self, db):
        self.db, self.f, self.e, self.b = db, [], [], []

    def file(self, i, digest, blocks, extents, seq=None):
        seq = seq if seq is not None else (i - 1) // GENERATION + 1
        self.f.append((i, f"/t/f{i}", i, i, 5, blocks * 4096, 0, seq,
                       digest, 0, extents))
        self.flush(100_000)

    def extent(self, digest, fileid, loff, length):
        self.e.append((digest, fileid, loff, fileid * 4096 + loff, length))
        self.flush(100_000)

    def block(self, digest, fileid, loff):
        self.b.append((digest, fileid, loff))
        self.flush(100_000)

    def flush(self, limit=0):
        if len(self.f) > limit:
            self.db.executemany(
                "insert into files (id, filename, path_hash, ino, subvol, size,"
                " mtime, dedupe_seq, digest, flags, nr_extents)"
                " values (?,?,?,?,?,?,?,?,?,?,?)", self.f)
            self.f = []
        if len(self.e) > limit:
            self.db.executemany("insert into extents (digest, fileid, loff,"
                                " poff, len) values (?,?,?,?,?)", self.e)
            self.e = []
        if len(self.b) > limit:
            self.db.executemany("insert into blocks (digest, fileid, loff)"
                                " values (?,?,?)", self.b)
            self.b = []


def build(path, profile, scale, schema):
    gen, n = PROFILES[profile]
    n = max(GENERATION * 4, int(n * scale))
    tmp = path.with_suffix(".tmp")
    tmp.unlink(missing_ok=True)
    db = sqlite3.connect(tmp)
    db.execute("pragma journal_mode=off")
    db.execute("pragma synchronous=off")
    for s in SCHEMA[:3]:                        # the three tables
        db.executescript(schema[s])
    db.execute("alter table files add column nr_extents integer not null default 0")
    rows = Rows(db)
    gen(rows, n, random.Random(1))
    rows.flush()
    db.commit()
    # The index oans keeps from the start, then the ones it builds after the
    # scan (dbfile_create_search_indexes), then statistics as they would be:
    # oans never runs ANALYZE, so neither does this.
    db.executescript(schema["CREATE_FILES_DEDUPESEQ_INDEX"])
    db.executescript(schema["CREATE_SEARCH_INDEXES"])
    db.commit()
    db.close()
    tmp.rename(path)


# --------------------------------------------------------------------------- #
# Measuring.
# --------------------------------------------------------------------------- #
def written():
    with open("/proc/self/io") as f:
        for line in f:
            if line.startswith("wchar"):
                return int(line.split()[1])
    return 0


def run(db, sql, args):
    w0, t0 = written(), time.perf_counter()
    rows = db.execute(sql, args).fetchall()
    return time.perf_counter() - t0, written() - w0, rows


def windows(first, last):
    lo = first
    while lo < last:
        yield lo, min(lo + PASS_GENERATIONS, last)
        lo += PASS_GENERATIONS


def measure(db, query, sql, first, last):
    """One query over one case. Returns (seconds, temp bytes, result)."""
    if query in COUNTS:
        t, w, rows = run(db, sql[1 if first == 0 else 0], (first,))
        return t, w, tuple(rows[0])
    key = LOADERS[query][1]
    total_t = total_w = 0
    groups = []
    # ?3, where it is used, is where the dedupe phase started (#272).
    extra = (first,) if "?3" in sql else ()
    for lo, hi in windows(first, last):
        t, w, rows = run(db, sql, (lo, hi) + extra)
        total_t, total_w = total_t + t, total_w + w
        g = collections.defaultdict(list)
        for r in rows:
            g[key(r)].append(r)
        groups.append(dict(g))
    return total_t, total_w, groups


def describe(result, query):
    if query in COUNTS:
        return f"{result[0]} groups"
    return f"{sum(len(m) for g in result for m in g.values())} rows"


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-p", "--profile", action="append", choices=PROFILES,
                    help="hashfile shape (repeatable; default: all)")
    ap.add_argument("-q", "--query", action="append", choices=QUERIES,
                    help="query to time (repeatable; default: all)")
    ap.add_argument("-c", "--case", action="append",
                    choices=CASES,
                    help="first scan, half new, or one new generation "
                         "(repeatable; default: all)")
    ap.add_argument("--base", default="origin/master",
                    help="git ref to compare the working tree with "
                         "(default: origin/master)")
    ap.add_argument("--scale", type=float, default=1.0,
                    help="multiply every profile's file count (default: 1)")
    ap.add_argument("-r", "--rounds", type=int, default=1,
                    help="runs per query; the minimum is reported (default: 1)")
    ap.add_argument("--workdir", default="~/.cache/oans-bench-queries",
                    help="where generated hashfiles are kept")
    args = ap.parse_args()

    base_sql = sql_for(source_at(args.base))
    new_sql = sql_for(source_at(None))
    workdir = Path(os.path.expanduser(args.workdir))
    workdir.mkdir(parents=True, exist_ok=True)
    print(f"# SQLite {sqlite3.sqlite_version} (Python's), base {args.base} "
          f"vs working tree, scale {args.scale}")

    def sql(table, q):
        if q in COUNTS:
            for pair in COUNTS[q]:
                if all(table[e] for e in pair):
                    return tuple(table[e] for e in pair)
            return None
        return table[LOADERS[q][0]]

    cases = args.case or CASES
    differ = False
    hdr = (f"{'profile':11} {'query':14} {'case':6} "
           f"{'base s':>8} {'temp MiB':>9} {'new s':>8} {'temp MiB':>9}  result")
    for profile in args.profile or list(PROFILES):
        path = workdir / f"{profile}-x{args.scale:g}-v{GEN_VERSION}.db"
        if not path.exists():
            print(f"# building {path.name} ...", flush=True)
            t0 = time.time()
            build(path, profile, args.scale, new_sql)
            print(f"# built in {time.time() - t0:.0f} s, "
                  f"{path.stat().st_size >> 20} MiB", flush=True)
        # Read the file once, so the side that runs first does not pay for a
        # cold page cache: that alone once made one query 20.8 s against 0.3 s.
        with open(path, "rb") as f:
            while f.read(1 << 24):
                pass
        db = sqlite3.connect(f"file:{path}?mode=ro", uri=True)
        db.execute("pragma cache_size = -65536")   # as dbfile_set_modes()
        last = db.execute("select max(dedupe_seq) from files").fetchone()[0]
        start = dict(zip(CASES, (0, last // 2, last - 1)))
        print(f"\n{hdr}")
        for q in args.query or QUERIES:
            b, n = sql(base_sql, q), sql(new_sql, q)
            if not b or not n:
                print(f"{profile:11} {q:14} (not in one of the two versions)")
                continue
            # The same SQL on both sides is timed once: a loader nobody
            # changed would otherwise cost a full second run for "same".
            sides = (("new", n),) if b == n else (("base", b), ("new", n))
            for case in cases:
                best = {}
                for _ in range(args.rounds):
                    for side, s in sides:
                        r = measure(db, q, s, start[case], last)
                        if side not in best or r[0] < best[side][0]:
                            best[side] = r
                if b == n:
                    base, verdict = "        =          =", "SQL unchanged"
                else:
                    same = best["base"][2] == best["new"][2]
                    differ |= not same
                    base = f"{best['base'][0]:8.2f} {best['base'][1] >> 20:9}"
                    verdict = "same" if same else "DIFFERENT"
                print(f"{profile:11} {q:14} {case:6} {base} "
                      f"{best['new'][0]:8.2f} {best['new'][1] >> 20:9}  "
                      f"{describe(best['new'][2], q)}, {verdict}", flush=True)
        db.close()
    if differ:
        print("\nDIFFERENT: the working tree's SQL returns other results "
              "than the base's.")
        sys.exit(1)


if __name__ == "__main__":
    main()
