#!/usr/bin/env python3
"""oans benchmark harness — the single entry point for scan/hash/dedupe timing.

This subsumes the older bench-*.sh scripts (bench-scan, bench-scan-cold,
bench-ram): one tool that generates reproducible trees, runs a MATRIX of
binaries x thread-settings x env-variants over one or more workload profiles,
cold or warm, interleaved across rounds, and prints wall / user / sys / peak-RSS
with mean/median/min/max.

    scripts/bench.py --help

Design (so future needs extend it without a rewrite):
  * PROFILES  — declarative workload definitions (tree shape + what it stresses).
                Add a key; nothing else changes. Sizes target ~10 s cold on NVMe.
  * a "cell"  — one point in the run matrix = (binary, io-threads, walk-threads,
                env-variant). The matrix is the cartesian product of whatever you
                pass; the label shows only the dimensions that actually vary.
  * metrics   — parsed from `/usr/bin/time -f`, so adding a metric is one token.
  * cold      — `sudo tee /proc/sys/vm/drop_caches` before every timed run (the
                dev box enables exactly this via sudoers); auto-falls back to warm.

Everything is non-destructive: runs use `-rq` (scan + find_dupes, no -d), so a
tree is generated once and reused across every run and binary.

Examples
  # Default: the 'realistic' profile, current ./oans, cold, 5 rounds.
  scripts/bench.py

  # A/B two binaries on two profiles.
  scripts/bench.py -p mixed,bigfile --bin base=/tmp/oans-master --bin new=./oans

  # Sweep walker threads (decoupled from the hashing pool) on a real tree.
  scripts/bench.py -p git --walk-threads 4,8,16,32

  # Sweep io-threads and also capture peak RSS.
  scripts/bench.py -p many --io-threads 2,4,8,16 --rss

  # Compare an env-gated code experiment against the baseline.
  scripts/bench.py -p git --variant pipe: --variant sep:DUPEREMOVE_SEPARATE_PHASES=1
"""
from __future__ import annotations

import argparse
import json
import math
import os
import random
import shutil
import statistics
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path

DEFAULT_IO = 8       # io-threads when the user doesn't sweep --io-threads
DROP_CACHES = "echo 3 | sudo -n tee /proc/sys/vm/drop_caches"


# --------------------------------------------------------------------------- #
# Workload profiles. Each is a tree shape; sizes target ~10 s cold on NVMe.
# `git` is special: it benchmarks an existing real tree (default ~/git) instead
# of generating one — the canonical 174k-file case.
# --------------------------------------------------------------------------- #
@dataclass
class Profile:
    note: str
    unique: int = 0          # unique files (hashed, never deduped)
    dup_groups: int = 0      # duplicated groups (the dedupe/find_dupes work)
    copies: int = 3          # files per duplicated group (incl. original)
    mean_kb: int = 1024      # exponential mean file size
    min_kb: int = 16
    max_kb: int = 65536
    big_mb: int = 0          # if >0, add one deliberately huge file of this size
    external: str | None = None   # benchmark this existing path, don't generate


PROFILES: dict[str, Profile] = {
    # The everyday regression profile: many unique files + a slice of duplicates,
    # so BOTH the hashing and the find_dupes/dedupe phases run. High file count
    # makes it btrfs-metadata-walk-bound, like a real source/data tree.
    "realistic": Profile(
        note="~65k files, exp sizes, 2k dup groups — hashing + find_dupes, walk-bound",
        unique=60000, dup_groups=2000, copies=3, mean_kb=128, min_kb=8, max_kb=131072),
    # Pure hashing, many-small-few-large, no duplicates. Data-bandwidth bound.
    "mixed": Profile(
        note="~8k files, exp sizes up to 512 MiB, no dups — pure hashing, bandwidth-bound",
        unique=8000, dup_groups=0, mean_kb=4096, min_kb=64, max_kb=524288),
    # Adversarial idle-tail: a mixed background plus one huge file. Largest-first
    # scheduling matters here; try --io-threads 2 to make any tail visible.
    "bigfile": Profile(
        note="mixed background + one 10 GiB file — largest-first idle-tail case",
        unique=2000, dup_groups=0, mean_kb=1024, min_kb=16, max_kb=131072, big_mb=10240),
    # Lots of tiny dup-heavy files: stresses the find_dupes search pool + walk.
    "many": Profile(
        note="~250k tiny dup-heavy files — find_dupes pool + metadata walk",
        unique=50000, dup_groups=50000, copies=4, mean_kb=8, min_kb=4, max_kb=64),
    # A few large files: stresses the per-thread read buffers (RSS profile).
    "big": Profile(
        note="~48 x 40 MiB files — per-thread read buffers; pair with --rss",
        unique=48, dup_groups=1, copies=2, mean_kb=40000, min_kb=40000, max_kb=40000),
    # Not generated: an existing real tree (override with --external PATH).
    "git": Profile(note="existing real tree (default ~/git)", external="~/git"),
}


# --------------------------------------------------------------------------- #
# Tree generation. Plans (path,size) in Python (exponential sizes, seeded ->
# reproducible), fills content in parallel via demo/gen.py workers, then makes
# duplicate copies with `cp --reflink=never`. A manifest records the params so a
# tree is regenerated only when its shape changes.
# --------------------------------------------------------------------------- #
HERE = Path(__file__).resolve().parent
GEN_PY = HERE / "demo" / "gen.py"


def _exp_size_kb(rng: random.Random, mean: int, lo: int, hi: int) -> int:
    u = max(rng.random(), 1e-9)
    s = int(-mean * math.log(u))
    s = max(lo, min(hi, s))
    return max(4, (s // 4) * 4)


def _require_reflink_fs(path: Path) -> None:
    fstype = subprocess.check_output(["stat", "-f", "-c", "%T", str(path)]).decode().strip()
    if fstype not in ("btrfs", "xfs"):
        sys.exit(f"error: {path} is on '{fstype}'; oans needs btrfs or xfs (reflink), "
                 "else dedupe is a silent no-op and timings are meaningless.")


def ensure_tree(name: str, prof: Profile, workdir: Path, seed: int = 1) -> Path:
    """Return the tree dir for a profile, generating it if missing/stale."""
    if prof.external:
        tree = Path(os.path.expanduser(prof.external)).resolve()
        if not tree.is_dir():
            sys.exit(f"error: external tree {tree} does not exist")
        _require_reflink_fs(tree)
        return tree

    key = {k: getattr(prof, k) for k in
           ("unique", "dup_groups", "copies", "mean_kb", "min_kb", "max_kb", "big_mb")}
    key["seed"] = seed
    root = workdir / name
    tree = root / "tree"
    manifest = root / "manifest.json"
    if tree.is_dir() and manifest.is_file() and json.loads(manifest.read_text()) == key:
        return tree
    if root.exists():
        shutil.rmtree(root)

    root.mkdir(parents=True)
    _require_reflink_fs(root)
    print(f"generating '{name}' tree ...", file=sys.stderr)

    rng = random.Random(seed)
    gen_jobs: list[tuple[int, str]] = []      # (bytes, path)
    copy_jobs: list[tuple[str, str]] = []     # (src, dst)
    for i in range(prof.unique):
        s = _exp_size_kb(rng, prof.mean_kb, prof.min_kb, prof.max_kb)
        gen_jobs.append((s * 1024, str(tree / "data" / f"set_{i // 300:04d}" / f"u_{i:06d}.bin")))
    for g in range(prof.dup_groups):
        s = _exp_size_kb(rng, prof.mean_kb, prof.min_kb, prof.max_kb)
        orig = tree / "dup" / f"group_{g:05d}" / "original.bin"
        gen_jobs.append((s * 1024, str(orig)))
        for c in range(1, prof.copies):
            copy_jobs.append((str(orig), str(orig.parent / f"copy_{c:02d}.bin")))
    if prof.big_mb:
        gen_jobs.append((prof.big_mb * 1024 * 1024, str(tree / "huge.bin")))

    # One mkdir per distinct directory (copies land in their group's dir, which
    # its original's gen_job already created — no separate pass needed).
    for d in {Path(p).parent for _, p in gen_jobs}:
        d.mkdir(parents=True, exist_ok=True)

    # Fill content in parallel: split the job list into nproc chunks; each chunk
    # goes to a gen.py worker (seeded per chunk filename -> independent streams).
    nworkers = os.cpu_count() or 4
    procs = []
    for ci in range(nworkers):
        chunk = gen_jobs[ci::nworkers]
        if not chunk:
            continue
        cf = root / f".chunk_{ci:02d}"
        cf.write_text("".join(f"{b}\t{p}\n" for b, p in chunk))
        procs.append(subprocess.Popen([sys.executable, str(GEN_PY), str(cf)]))
    for pr in procs:
        pr.wait()
    for cf in root.glob(".chunk_*"):
        cf.unlink()

    # Duplicate copies. --reflink=never is essential: on btrfs cp reflinks by
    # default, leaving oans nothing to reclaim.
    if copy_jobs:
        payload = "".join(f"{s}\0{d}\0" for s, d in copy_jobs).encode()
        subprocess.run(["xargs", "-0", "-n2", f"-P{nworkers}", "cp", "--reflink=never"],
                       input=payload, check=True)

    manifest.write_text(json.dumps(key))
    du = subprocess.check_output(["du", "-sh", str(tree)]).split()[0].decode()
    print(f"  {name}: {du} in {len(gen_jobs) + len(copy_jobs)} files", file=sys.stderr)
    return tree


def count_files(tree: Path) -> int:
    return sum(1 for p in tree.rglob("*") if p.is_file())


# --------------------------------------------------------------------------- #
# Running one command under /usr/bin/time; cold-cache handling.
# --------------------------------------------------------------------------- #
_COLD_OK: bool | None = None


def cold_supported() -> bool:
    global _COLD_OK
    if _COLD_OK is None:
        subprocess.run(["sync"])
        r = subprocess.run(DROP_CACHES, shell=True,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        _COLD_OK = r.returncode == 0
    return _COLD_OK


def drop_caches() -> None:
    subprocess.run(["sync"])
    subprocess.run(DROP_CACHES, shell=True, stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL, check=True)


@dataclass
class Sample:
    wall: float
    user: float
    sys: float
    rss_mib: float


def run_once(cmd: list[str], env: dict[str, str], cold: bool) -> Sample:
    if cold:
        drop_caches()
    # %e wall(s) %U user(s) %S sys(s) %M max-RSS(KiB) — one line, no parsing.
    r = subprocess.run(["/usr/bin/time", "-f", "%e %U %S %M"] + cmd,
                       env={**os.environ, **env},
                       stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    if r.returncode != 0:
        sys.exit(f"run failed ({r.returncode}):\n{r.stderr.decode()}")
    line = [ln for ln in r.stderr.decode().splitlines() if ln.strip()][-1]
    wall, user, sysc, rss_kib = (float(x) for x in line.split())
    return Sample(wall, user, sysc, rss_kib / 1024.0)


# --------------------------------------------------------------------------- #
# The run matrix: a "cell" is one binary+threads+env combination; its label
# shows only the dimensions that vary across the requested matrix.
# --------------------------------------------------------------------------- #
@dataclass
class Cell:
    label: str
    binary: str
    io_threads: int
    env: dict[str, str] = field(default_factory=dict)
    samples: list[Sample] = field(default_factory=list)

    def cmd(self, tree: Path, db: str) -> list[str]:
        return [self.binary, "-rq", f"--io-threads={self.io_threads}",
                f"--cpu-threads={self.io_threads}", f"--hashfile={db}", str(tree)]


def parse_bins(specs: list[str]) -> list[tuple[str, str]]:
    out = []
    for s in specs:
        label, path = s.split("=", 1) if "=" in s else (Path(s).name, s)
        out.append((label, str(Path(path).resolve())))
    return out


def parse_variants(specs: list[str]) -> list[tuple[str, dict[str, str]]]:
    out = []
    for s in specs:
        label, _, envstr = s.partition(":")
        env = {}
        for kv in filter(None, envstr.split(",")):
            k, _, v = kv.partition("=")
            env[k] = v
        out.append((label, env))
    return out


def build_cells(args) -> list[Cell]:
    bins = parse_bins(args.bin or ["./oans"])
    ios = [int(x) for x in args.io_threads.split(",")] if args.io_threads else [DEFAULT_IO]
    walks = [int(x) for x in args.walk_threads.split(",")] if args.walk_threads else [None]
    variants = parse_variants(args.variant) if args.variant else [("", {})]

    multi = {"bin": len(bins) > 1, "io": len(ios) > 1,
             "walk": len(walks) > 1, "var": len(variants) > 1}
    cells = []
    for blabel, bpath in bins:
        for io in ios:
            for wk in walks:
                for vlabel, venv in variants:
                    env = dict(venv)
                    if wk is not None:
                        env["DUPEREMOVE_WALK_THREADS"] = str(wk)
                    parts = ([blabel] if multi["bin"] else [])
                    if multi["var"] and vlabel:
                        parts.append(vlabel)
                    if multi["io"]:
                        parts.append(f"io={io}")
                    if multi["walk"]:
                        parts.append(f"walk={wk}")
                    cells.append(Cell(" ".join(parts) or blabel, bpath, io, env))
    return cells


# --------------------------------------------------------------------------- #
def run_profile(name: str, prof: Profile, cells: list[Cell], args) -> None:
    tree = ensure_tree(name, prof, Path(args.workdir).expanduser())
    cold = args.cold and cold_supported()
    if args.cold and not cold:
        print("note: cold requested but `sudo -n tee drop_caches` failed; running WARM",
              file=sys.stderr)
    db = str(Path(args.workdir).expanduser() / ".bench.db")
    for c in cells:
        c.samples.clear()

    print(f"\n=== profile '{name}': {prof.note}", file=sys.stderr)
    print(f"    tree={tree} files={count_files(tree)} mode={'COLD' if cold else 'warm'} "
          f"rounds={args.rounds}", file=sys.stderr)

    if not cold:  # prime cache once so the first warm run isn't a cold outlier
        subprocess.run(["bash", "-c", f"find {tree!s} -type f -exec cat {{}} + >/dev/null"])

    order = list(range(len(cells)))
    for r in range(1, args.rounds + 1):
        random.shuffle(order)  # randomize within-round order to spread drift/noise
        for idx in order:
            c = cells[idx]
            for f in Path(db).parent.glob(".bench.db*"):
                f.unlink()
            c.samples.append(run_once(c.cmd(tree, db), c.env, cold))
        print(f"    round {r}/{args.rounds} done", file=sys.stderr)
    for f in Path(db).parent.glob(".bench.db*"):
        f.unlink()

    # Report. Median is the headline (robust to the odd cold/swap outlier).
    w = max(len(c.label) for c in cells)
    hdr = f"{'cell':<{w}} {'wall_med':>9} {'wall_mean':>9} {'min':>7} {'max':>7} {'user':>7} {'sys':>7}"
    if args.rss:
        hdr += f" {'rssMiB':>8}"
    print("\n" + hdr)
    print("-" * len(hdr))
    for c in cells:
        wl = [s.wall for s in c.samples]
        row = (f"{c.label:<{w}} {statistics.median(wl):>9.2f} {statistics.mean(wl):>9.2f} "
               f"{min(wl):>7.2f} {max(wl):>7.2f} "
               f"{statistics.mean(s.user for s in c.samples):>7.2f} "
               f"{statistics.mean(s.sys for s in c.samples):>7.2f}")
        if args.rss:
            row += f" {max(s.rss_mib for s in c.samples):>8.0f}"
        print(row)


def main() -> None:
    ap = argparse.ArgumentParser(
        description="oans benchmark harness (subsumes bench-scan/bench-ram/bench-scan-cold).",
        formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    ap.add_argument("-p", "--profile", default="realistic",
                    help="comma-separated profiles (default realistic). "
                         f"choices: {', '.join(PROFILES)}")
    ap.add_argument("--bin", action="append",
                    help="binary as LABEL=PATH or PATH (repeatable; default ./oans)")
    ap.add_argument("--variant", action="append",
                    help="env variant LABEL:VAR=val,VAR2=val (repeatable), "
                         "e.g. sep:DUPEREMOVE_SEPARATE_PHASES=1")
    ap.add_argument("--io-threads", help="comma-separated io-threads to sweep (default: profile default)")
    ap.add_argument("--walk-threads", help="comma-separated DUPEREMOVE_WALK_THREADS to sweep (decoupled walkers)")
    ap.add_argument("-r", "--rounds", type=int, default=5, help="rounds (default 5)")
    ap.add_argument("--warm", dest="cold", action="store_false", help="run warm (default: cold if available)")
    ap.add_argument("--rss", action="store_true", help="also report peak RSS (MiB)")
    ap.add_argument("--workdir", default="~/.oans-bench", help="where trees + hashfile live")
    ap.add_argument("--external", help="path for the 'git' profile's external tree (default ~/git)")
    ap.add_argument("--list", action="store_true", help="list profiles and exit")
    ap.set_defaults(cold=True)
    args = ap.parse_args()

    if args.list:
        for n, p in PROFILES.items():
            print(f"  {n:<10} {p.note}")
        return
    if args.external:
        PROFILES["git"].external = args.external

    Path(args.workdir).expanduser().mkdir(parents=True, exist_ok=True)
    names = args.profile.split(",")
    for n in names:
        if n not in PROFILES:
            sys.exit(f"unknown profile '{n}' (choices: {', '.join(PROFILES)})")
    t0 = time.time()
    for n in names:
        run_profile(n, PROFILES[n], build_cells(args), args)
    print(f"\ntotal {time.time() - t0:.0f}s", file=sys.stderr)


if __name__ == "__main__":
    main()
