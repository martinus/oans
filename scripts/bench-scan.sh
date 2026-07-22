#!/usr/bin/env bash
# A/B scan-makespan benchmark for the hashing phase.
#
# Interleaves two oans binaries over one synthetic tree, fresh hashfile each run,
# and reports mean / min / max wall + user CPU per binary. Built to compare scan
# scheduling and hashing changes (e.g. the largest-file-first work queue): a
# scheduling win shows up as lower and steadier wall on a size-varied tree at
# roughly unchanged user CPU; a wasted-work change shows up in user CPU.
#
# Reuses scripts/demo/setup.sh + gen.py to build a reproducible tree of distinct,
# incompressible files. Runs `oans -r` (scan + hash, NO -d, so it is
# non-destructive and repeatable) with DEMO_DUP_GROUPS=0 so the dedupe /
# find_dupes phase is trivial and hashing dominates the wall time.
#
#   scripts/bench-scan.sh <work-dir on btrfs/xfs> <oans-A> <oans-B> [profile] [rounds] [io-threads]
#
# Profiles (dataset generated once and reused; delete <work-dir>/tree to rebuild):
#   mixed    (default) ~2000 files, exponential sizes up to 128 MiB -- the common
#                      many-small-few-large case; use it to check for regressions.
#   bigfile            mixed background + one deliberately huge file
#                      (BENCH_BIG_MB, default 4096) -- the adversarial idle-tail
#                      case that only largest-first scheduling avoids.
#
# Fewer threads make an idle tail more visible: for the bigfile profile try
# io-threads=2. No root needed -- runs warm (the page cache is primed before
# timing), so cold drop_caches is not required.
#
# Example A/B (baseline from master vs the working build):
#   git worktree add /tmp/oans-base origin/master && make -C /tmp/oans-base -j
#   scripts/bench-scan.sh ~/bench /tmp/oans-base/oans ./oans bigfile 6 2
set -euo pipefail

WORK="${1:?usage: bench-scan.sh <work-dir on btrfs/xfs> <oans-A> <oans-B> [profile] [rounds] [io-threads]}"
A="$(readlink -f "${2:?need path to oans binary A}")"
B="$(readlink -f "${3:?need path to oans binary B}")"
PROFILE="${4:-mixed}"
ROUNDS="${5:-5}"
IO="${6:-8}"
BIG_MB="${BENCH_BIG_MB:-4096}"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

case "$PROFILE" in
  mixed|bigfile) : ;;
  *) echo "unknown profile '$PROFILE' (use mixed|bigfile)" >&2; exit 1 ;;
esac

# 1) Build the tree once and reuse it across runs/binaries for a fair comparison.
#    DROP_CACHES=0: we prime the cache ourselves and time warm runs.
if [ ! -d "$WORK/tree" ]; then
  env DEMO_UNIQUE=2000 DEMO_DUP_GROUPS=0 DEMO_MEAN_KB=1024 \
      DEMO_MIN_KB=16 DEMO_MAX_KB=131072 DROP_CACHES=0 \
      bash "$here/demo/setup.sh" "$WORK" >&2
  if [ "$PROFILE" = bigfile ]; then
    echo "adding one ${BIG_MB} MiB file (bigfile profile) ..." >&2
    printf '%d\t%s\n' "$((BIG_MB * 1024 * 1024))" "$WORK/tree/huge.bin" > "$WORK/.bigjob"
    python3 "$here/demo/gen.py" "$WORK/.bigjob"
    rm -f "$WORK/.bigjob"
  fi
fi

nfiles=$(find "$WORK/tree" -type f | wc -l)
echo "tree: $(du -sh "$WORK/tree" | cut -f1) in $nfiles files; io-threads=$IO; $ROUNDS rounds" >&2
echo "  A = $A" >&2
echo "  B = $B" >&2

# 2) Prime the page cache so the first timed run is not a cold outlier.
find "$WORK/tree" -type f -exec cat {} + > /dev/null

# 3) Interleave A and B every round (cancels drift); fresh hashfile per run.
DB="$WORK/scan-bench.db"
times="$WORK/.scan.times"
: > "$times"
labels=(A B)
bins=("$A" "$B")
for ((r = 1; r <= ROUNDS; r++)); do
  for i in 0 1; do
    rm -f "$DB"*
    # "<label> <wall> <user> <sys>"
    /usr/bin/time -f "${labels[$i]} %e %U %S" \
      "${bins[$i]}" -rq --io-threads="$IO" --cpu-threads="$IO" \
      --hashfile="$DB" "$WORK/tree" >/dev/null 2>>"$times"
  done
done
rm -f "$DB"*

# 4) Aggregate mean / min / max wall + mean user/sys per binary.
echo
awk '
  { n[$1]++; sw[$1]+=$2; su[$1]+=$3; ss[$1]+=$4;
    if (mnw[$1]=="" || $2 < mnw[$1]) mnw[$1]=$2;
    if ($2 > mxw[$1]) mxw[$1]=$2; }
  END {
    printf "%-3s %8s %8s %8s   %8s %8s\n", "bin", "wall", "min", "max", "user", "sys";
    for (b in n)
      printf "%-3s %8.2f %8.2f %8.2f   %8.2f %8.2f\n",
             b, sw[b]/n[b], mnw[b], mxw[b], su[b]/n[b], ss[b]/n[b];
  }' "$times"
rm -f "$times"
