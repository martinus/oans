#!/usr/bin/env python3
"""bench-dedupe.py — measure the oans DEDUPE phase on a larger-than-RAM tree.

`scripts/bench.py` benchmarks the scan/hash phase (`-rq`, non-destructive, a
matrix of synthetic profiles). It deliberately can't measure dedupe: dedupe
*mutates* the tree (so every timed round needs a fresh unshared copy) and its
signature cost only shows up when the working set is larger than RAM. This is
that companion harness — the one that produces the numbers in
`docs/benchmarks.md`'s "Larger-than-RAM dedupe" section.

It builds N non-reflinked copies of a source tree (so dedupe has real work),
then times two phases per binary, cold (`drop_caches`) and inside a
memory-capped cgroup scope, so the just-hashed data is evicted and the
`FIDEDUPERANGE` byte-compare pays the cold re-read — faithfully reproducing a
dataset larger than RAM on a big-RAM box:

  * hash    — `<bin> -rq TREE`               (in-memory, no hashfile, no dedupe)
  * dedupe  — `<bin> -dr --hashfile HF TREE` (the real workload)

Runs are interleaved in a randomized order each round; between dedupe runs the
non-anchor copies are restored (`rm` + `cp --reflink=never`) and the hashfile is
deleted, so every run starts from the same fully-unshared tree. Reports
wall/user/sys/RSS/disk-read as median/mean/min/max.

With --verify it also confirms the binaries reach byte-identical sharing
(`btrfs filesystem du -s`: same Exclusive / Set-shared).

Cold caches need the passwordless `sudo -n tee /proc/sys/vm/drop_caches` that
`bench.py` already relies on; without it the harness runs warm and says so. The
memory cap uses a transient `systemd-run --user --scope` (cgroup v2 memory
controller must be delegated to the user slice).

Examples:
  # oans vs the fork's upstream baseline (built from a git ref), the doc setup:
  scripts/bench-dedupe.py --baseline build:897a222 --source ~/git/linux \\
      --copies 2 --cap 4G --rounds 10 --verify

  # oans alone, quick end-to-end plumbing check (uncapped, one round):
  scripts/bench-dedupe.py --source ~/git/linux/kernel --cap none --rounds 1
"""
import argparse
import os
import random
import shutil
import statistics
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path

# Reuse bench.py's cold-cache / reflink / file-count helpers, don't re-implement.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bench import cold_supported, count_files, drop_caches, _require_reflink_fs  # noqa: E402

REPO = Path(__file__).resolve().parent.parent
# oans builds `oans`; the fork's upstream base (build:897a222) builds `duperemove`.
BINARY_NAMES = ("oans", "duperemove")


# --------------------------------------------------------------------------- #
# Binaries: oans, plus an optional comparison baseline (a path, or build:REF).
# --------------------------------------------------------------------------- #
def resolve_baseline(spec: str, workdir: Path) -> str:
    """A path to an existing binary, or 'build:REF' to build one from a git ref
    in a cached detached worktree (needs the build toolchain; source devenv on
    the dev box first)."""
    if not spec.startswith("build:"):
        p = Path(os.path.expanduser(spec)).resolve()
        if not p.exists():
            sys.exit(f"--baseline: no such binary: {p}")
        return str(p)

    ref = spec[len("build:"):]
    wt = workdir / f"baseline-{ref}"
    def built_binary():
        return next((wt / n for n in BINARY_NAMES if (wt / n).exists()), None)

    binary = built_binary()
    if binary is None:
        if not wt.exists():
            subprocess.run(["git", "worktree", "add", "--detach", str(wt), ref],
                           cwd=REPO, check=True)
        print(f"building baseline {ref} (needs the build toolchain)...", file=sys.stderr)
        if subprocess.run(["make", "-j", str(os.cpu_count() or 4)], cwd=wt).returncode:
            sys.exit(f"baseline build failed in {wt}; build it yourself and pass "
                     "--baseline PATH instead of build:REF")
        binary = built_binary()
        if binary is None:
            sys.exit(f"baseline built but no {'/'.join(BINARY_NAMES)} binary in {wt}")
    return str(binary.resolve())


# --------------------------------------------------------------------------- #
# Dataset: N non-reflinked copies of a source tree. copy0 is the dedupe anchor
# (stays put); copy1..N-1 are restored (unshared) before every dedupe round.
# --------------------------------------------------------------------------- #
def build_tree(source: Path, tree: Path, copies: int) -> None:
    tree.mkdir(parents=True, exist_ok=True)
    for i in range(copies):
        dst = tree / f"copy{i}"
        if not dst.exists():
            # --reflink=never is essential: on btrfs cp reflinks by default, which
            # would pre-share the copies and leave dedupe nothing to do.
            subprocess.run(["cp", "-a", "--reflink=never", str(source), str(dst)],
                           check=True)
    subprocess.run(["sync"])


def restore(source: Path, tree: Path, copies: int) -> None:
    for i in range(1, copies):
        shutil.rmtree(tree / f"copy{i}", ignore_errors=True)
    subprocess.run(["sync"])
    for i in range(1, copies):
        subprocess.run(["cp", "-a", "--reflink=never", str(source), str(tree / f"copy{i}")],
                       check=True)
    subprocess.run(["sync"])


def dir_bytes(path: Path) -> int:
    out = subprocess.check_output(["du", "-sb", str(path)]).decode()
    return int(out.split()[0])


def parse_size(s: str) -> int:
    s = s.strip()
    mult = {"K": 1024, "M": 1024**2, "G": 1024**3, "T": 1024**4}
    return int(float(s[:-1]) * mult[s[-1].upper()]) if s[-1].upper() in mult else int(s)


# --------------------------------------------------------------------------- #
# One timed run: cold drop_caches, optional memory-capped scope, /usr/bin/time.
# --------------------------------------------------------------------------- #
@dataclass
class Sample:
    wall: float
    user: float
    sys: float
    rss_mib: float
    read_gib: float
    hf_mib: float = 0.0


def timed(binary: str, args: list[str], cap: str | None, cold: bool,
          logpath: Path) -> tuple[int, Sample]:
    if cold:
        drop_caches()
    tf = tempfile.NamedTemporaryFile(delete=False, suffix=".time")
    tf.close()
    # %e wall %U user %S sys %M max-RSS(KiB) %I fs-inputs(512B blocks).
    inner = ["/usr/bin/time", "-o", tf.name, "-f", "%e %U %S %M %I", binary, *args]
    if cap and cap.lower() != "none":
        cmd = ["systemd-run", "--user", "--scope", "--quiet",
               "-p", f"MemoryMax={cap}", "-p", "MemorySwapMax=0", "--", *inner]
    else:
        cmd = inner
    with open(logpath, "w") as log:
        rc = subprocess.run(cmd, stdout=log, stderr=log).returncode
    line = Path(tf.name).read_text().strip().splitlines()[-1]
    os.unlink(tf.name)
    wall, user, sysc, rss_kib, inputs = line.split()
    return rc, Sample(float(wall), float(user), float(sysc),
                      float(rss_kib) / 1024.0, float(inputs) * 512 / 2**30)


# --------------------------------------------------------------------------- #
# Phases.
# --------------------------------------------------------------------------- #
def run_phase(bins: list[tuple[str, str]], *, dedupe: bool, tree: Path,
              source: Path, copies: int, hf: Path, cap: str | None, cold: bool,
              rounds: int, logdir: Path) -> dict[str, list[Sample]]:
    """One phase. `dedupe` picks everything that differs between the two: the
    args, whether to restore the tree + drop the hashfile per run, and whether
    a hashfile size is reported. The hash phase is in-memory and non-destructive."""
    name = "dedupe" if dedupe else "hash"
    args = ["-dr", f"--hashfile={hf}", str(tree)] if dedupe else ["-rq", str(tree)]
    results: dict[str, list[Sample]] = {label: [] for label, _ in bins}
    order = list(range(len(bins)))
    for rnd in range(1, rounds + 1):
        random.shuffle(order)
        for idx in order:
            label, binary = bins[idx]
            if dedupe:
                restore(source, tree, copies)
                for suf in ("", "-wal", "-shm"):
                    Path(str(hf) + suf).unlink(missing_ok=True)
            log = logdir / f"{name}_{label}_{rnd}.log"
            rc, s = timed(binary, args, cap, cold, log)
            if dedupe and hf.exists():
                s.hf_mib = hf.stat().st_size / 2**20
            if rc != 0:
                print(f"  !! {label} round {rnd} exited {rc} (see {log})", file=sys.stderr)
            results[label].append(s)
            print(f"[{name}] r{rnd:2d} {label:12s} wall={s.wall:7.2f}s "
                  f"rss={s.rss_mib:6.1f}MiB read={s.read_gib:5.1f}GiB"
                  + (f" hf={s.hf_mib:.1f}MiB" if dedupe else ""))
    return results


def summarize(name: str, results: dict[str, list[Sample]], use_hashfile: bool) -> None:
    print(f"\n### {name} (median of {len(next(iter(results.values())))})")
    cols = f"{'binary':<14}{'wall':>8}{'mean':>8}{'min':>8}{'max':>8}{'user':>7}{'sys':>7}{'RSS':>8}{'read':>8}"
    if use_hashfile:
        cols += f"{'hf MiB':>8}"
    print(cols)
    for label, ss in results.items():
        w = [s.wall for s in ss]
        row = (f"{label:<14}{statistics.median(w):>8.2f}{statistics.mean(w):>8.2f}"
               f"{min(w):>8.2f}{max(w):>8.2f}"
               f"{statistics.mean(s.user for s in ss):>7.2f}"
               f"{statistics.mean(s.sys for s in ss):>7.2f}"
               f"{statistics.median(s.rss_mib for s in ss):>8.0f}"
               f"{statistics.median(s.read_gib for s in ss):>8.1f}")
        if use_hashfile:
            row += f"{statistics.median(s.hf_mib for s in ss):>8.1f}"
        print(row)


# --------------------------------------------------------------------------- #
# Byte-identical-sharing verification (btrfs only; xfs has no cheap equivalent).
# --------------------------------------------------------------------------- #
def verify_sharing(bins: list[tuple[str, str]], *, tree: Path, source: Path,
                   copies: int, hf: Path) -> None:
    if subprocess.check_output(["stat", "-f", "-c", "%T", str(tree)]).decode().strip() != "btrfs":
        print("\n(skipping --verify: needs btrfs `filesystem du -s`)", file=sys.stderr)
        return
    print("\n=== byte-identical sharing (btrfs filesystem du -s --raw) ===")
    shared = {}
    for label, binary in bins:
        restore(source, tree, copies)
        for suf in ("", "-wal", "-shm"):
            Path(str(hf) + suf).unlink(missing_ok=True)
        subprocess.run([binary, "-dr", f"--hashfile={hf}", str(tree)],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        subprocess.run(["sync"])
        # --raw for exact bytes: Total, Exclusive, Set-shared.
        total, excl, sh = (int(x) for x in subprocess.check_output(
            ["btrfs", "filesystem", "du", "-s", "--raw", str(tree)]
        ).decode().strip().splitlines()[-1].split()[:3])
        shared[label] = sh
        print(f"  {label:12s} total {total/2**30:6.2f} GiB  "
              f"exclusive {excl/2**20:8.1f} MiB  set-shared {sh/2**30:6.2f} GiB")
    # A tool that deduped everything leaves ~0 exclusive; compare the shared
    # bytes across binaries with a small tolerance, since btrfs extent packing
    # jitters the last few KiB run-to-run even for identical dedupe decisions.
    lo, hi = min(shared.values()), max(shared.values())
    ok = hi == 0 or (hi - lo) <= 0.001 * hi   # within 0.1%
    print("  => " + ("IDENTICAL sharing across binaries (within 0.1%)" if ok
                     else f"DIFFERENT sharing — investigate! ({lo} vs {hi} bytes)"))


# --------------------------------------------------------------------------- #
def main() -> None:
    ap = argparse.ArgumentParser(
        description="Benchmark the oans dedupe phase on a larger-than-RAM tree.",
        formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    ap.add_argument("--oans", default=str(REPO / "oans"),
                    help="oans binary under test (default: ./oans in the repo)")
    ap.add_argument("--baseline",
                    help="comparison binary: a path, or build:REF to build a git ref "
                         "(e.g. build:897a222 = upstream duperemove, the fork base). "
                         "Omit to benchmark oans alone.")
    ap.add_argument("--source", default="~/git/linux",
                    help="source tree to duplicate (default: ~/git/linux)")
    ap.add_argument("--copies", type=int, default=2,
                    help="non-reflinked copies of the source (default: 2)")
    ap.add_argument("--cap", default="4G",
                    help="cgroup memory cap per run, e.g. 4G, or 'none' (default: 4G)")
    ap.add_argument("-r", "--rounds", type=int, default=10, help="rounds per phase (default: 10)")
    ap.add_argument("--phases", default="hash,dedupe",
                    help="comma list of phases to run (default: hash,dedupe)")
    ap.add_argument("--warm", dest="cold", action="store_false",
                    help="run warm (default: cold if drop_caches is available)")
    ap.add_argument("--verify", action="store_true",
                    help="after timing, verify byte-identical sharing (btrfs)")
    ap.add_argument("--workdir", default="~/.oans-bench-dedupe",
                    help="where the tree, hashfile and baseline build live")
    ap.add_argument("--clean", action="store_true",
                    help="remove the generated tree (and built baselines) on exit")
    args = ap.parse_args()

    workdir = Path(os.path.expanduser(args.workdir)).resolve()
    workdir.mkdir(parents=True, exist_ok=True)
    _require_reflink_fs(workdir)  # never /tmp (tmpfs) — dedupe would be a silent no-op

    source = Path(os.path.expanduser(args.source)).resolve()
    if not source.is_dir():
        sys.exit(f"--source: not a directory: {source}")
    oans = Path(os.path.expanduser(args.oans)).resolve()
    if not oans.exists():
        sys.exit(f"--oans: no such binary: {oans} (run `make` first?)")

    bins: list[tuple[str, str]] = [("oans", str(oans))]
    if args.baseline:
        bins.append(("baseline", resolve_baseline(args.baseline, workdir)))

    cold = args.cold and cold_supported()
    if args.cold and not cold:
        print("note: cold requested but drop_caches isn't passwordless; running WARM",
              file=sys.stderr)

    tree = workdir / "tree"
    logdir = workdir / "logs"
    logdir.mkdir(exist_ok=True)
    hf = workdir / "bench.hash"

    print(f"# bench-dedupe  cap={args.cap}  rounds={args.rounds}  cold={cold}")
    for label, b in bins:
        print(f"#   {label}: {b}")
    print(f"# building {args.copies} non-reflinked copies of {source} in {tree} ...")
    build_tree(source, tree, args.copies)
    tb, cap_b = dir_bytes(tree), (parse_size(args.cap) if args.cap.lower() != "none" else 0)
    print(f"# dataset: {tb / 2**30:.1f} GiB, {count_files(tree)} files")
    if cap_b and tb <= cap_b:
        print(f"# WARNING: dataset ({tb/2**30:.1f} GiB) <= cap ({cap_b/2**30:.1f} GiB): "
              "not larger-than-RAM, the cold re-read won't show", file=sys.stderr)

    phases = args.phases.split(",")
    t0 = time.time()
    common = dict(tree=tree, source=source, copies=args.copies, hf=hf,
                  cap=args.cap, cold=cold, rounds=args.rounds, logdir=logdir)
    if "hash" in phases:
        print("\n== PHASE: hash only (in-memory) ==")
        r = run_phase(bins, dedupe=False, **common)
        summarize("PHASE hash-only", r, use_hashfile=False)
    if "dedupe" in phases:
        print("\n== PHASE: hash + dedupe ==")
        r = run_phase(bins, dedupe=True, **common)
        summarize("PHASE hash+dedupe", r, use_hashfile=True)

    if args.verify:
        verify_sharing(bins, tree=tree, source=source, copies=args.copies, hf=hf)

    print(f"\ntotal {(time.time() - t0) / 60:.1f} min", file=sys.stderr)

    if args.clean:
        shutil.rmtree(tree, ignore_errors=True)
        for label, b in bins:
            wt = Path(b).parent
            if wt.parent == workdir and wt.name.startswith("baseline-"):
                subprocess.run(["git", "worktree", "remove", "--force", str(wt)], cwd=REPO)
        print("cleaned workdir tree/baselines", file=sys.stderr)


if __name__ == "__main__":
    main()
