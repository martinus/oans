#!/usr/bin/env bash
# A/B peak-RSS + wall-time benchmark for the scan / find_dupes path.
#
# Runs `oans -r` (scan + duplicate search, NO -d, so it is non-destructive and
# repeatable) against a synthetic tree, reporting peak RSS and wall time. Point
# it at two binaries (e.g. a baseline built from master and your working build)
# and interleave to compare — that is how the read-buffer and per-connection
# cache budgets were tuned.
#
#   scripts/bench-ram.sh <work-dir on btrfs/xfs> <oans-binary> [many|big] [io-threads]
#
# Profiles (dataset is generated once and reused):
#   many  ~250k small files, dup-heavy  -> exercises the find_dupes search pool
#   big   ~48 large files (~40 MiB each) -> exercises the per-thread read buffers
#
# Example A/B:
#   git worktree add /tmp/oans-base origin/master && make -C /tmp/oans-base
#   scripts/bench-ram.sh ~/bench /tmp/oans-base/oans many
#   scripts/bench-ram.sh ~/bench ./oans            many
set -euo pipefail

WORK="${1:?usage: bench-ram.sh <work-dir> <oans-binary> [many|big] [io-threads]}"
BIN="${2:?need path to an oans binary}"
PROFILE="${3:-many}"
IO="${4:-8}"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

case "$PROFILE" in
  many) gen=(DEMO_UNIQUE=50000 DEMO_DUP_GROUPS=50000 DEMO_COPIES=4 DEMO_MEAN_KB=8  DEMO_MAX_KB=64) ;;
  big)  gen=(DEMO_UNIQUE=48    DEMO_DUP_GROUPS=1     DEMO_COPIES=2 DEMO_MEAN_KB=40000 DEMO_MIN_KB=40000 DEMO_MAX_KB=40000) ;;
  *) echo "unknown profile '$PROFILE' (use many|big)" >&2; exit 1 ;;
esac

# Generate the tree once; reuse it across binaries/runs for a fair comparison.
if [ ! -d "$WORK/tree" ]; then
  env "${gen[@]}" DROP_CACHES=0 bash "$here/demo/setup.sh" "$WORK" >&2
fi

rm -f "$WORK"/h.db*
/usr/bin/time -v "$BIN" -r --io-threads="$IO" --cpu-threads="$IO" \
  --hashfile="$WORK/h.db" "$WORK/tree" 2>"$WORK/.time" >/dev/null
wall=$(awk -F'wall clock).*: ' '/Elapsed \(wall/{print $2}' "$WORK/.time")
rss=$(awk '/Maximum resident/{print $6}' "$WORK/.time")
printf '%-22s io=%s  wall=%-9s  peakRSS=%d MiB\n' "$(basename "$BIN") [$PROFILE]" "$IO" "$wall" "$((rss/1024))"
