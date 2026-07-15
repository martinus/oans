#!/usr/bin/env bash
#
# perf-profile.sh - profile a duperemove run with perf and print the report
# format the maintainers ask for: a self/leaf hotspot view, a caller/stack view
# (which also shows the syscall cost breakdown), and the profiled run's
# wall-clock. Copy the whole output back into the issue / chat.
#
# Usage:
#   scripts/perf-profile.sh [options] -- <duperemove args...>
#
# Examples:
#   # profile a rescan of ~/git into a throwaway hashfile (warm cache)
#   scripts/perf-profile.sh -- -dr --hashfile=/tmp/prof.db ~/git
#
#   # same but drop the page cache first, to measure a cold run (needs sudo)
#   scripts/perf-profile.sh --cold -- -dr --hashfile=/tmp/prof.db ~/git
#
# Options:
#   -b, --binary PATH  duperemove binary to profile (default: ./duperemove,
#                      then $PWD/duperemove, then whatever is in $PATH)
#   -c, --cold         drop caches (sync + drop_caches) before the profiled run;
#                      needs passwordless sudo or root. Implies no warmup.
#   -w, --no-warmup    skip the warmup run (default: warm up unless --cold)
#   -f, --freq HZ      perf sampling frequency (default: 999)
#   -o, --out FILE     also write the report to FILE
#   -h, --help         this help
#
# Notes:
#   * The profiled command usually mutates its hashfile, so point --hashfile at
#     a throwaway copy (see examples) rather than your real cache.
#   * perf needs to read symbols. If the report is all hex addresses, run:
#       sudo sysctl kernel.perf_event_paranoid=1 kernel.kptr_restrict=0
#   * The binary should be built normally (it already carries -g debug info).

set -euo pipefail

BINARY=""
COLD=0
WARMUP=1
FREQ=999
OUT=""

die() { echo "perf-profile: $*" >&2; exit 1; }

while [ $# -gt 0 ]; do
	case "$1" in
	-b|--binary)   BINARY="${2:?}"; shift 2 ;;
	-c|--cold)     COLD=1; WARMUP=0; shift ;;
	-w|--no-warmup) WARMUP=0; shift ;;
	-f|--freq)     FREQ="${2:?}"; shift 2 ;;
	-o|--out)      OUT="${2:?}"; shift 2 ;;
	-h|--help)     sed -n '2,40p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
	--)            shift; break ;;
	*)             die "unknown option '$1' (did you forget '--' before the duperemove args?)" ;;
	esac
done

[ $# -gt 0 ] || die "no duperemove arguments given; put them after '--'. See --help."

# Resolve the binary.
if [ -z "$BINARY" ]; then
	if [ -x ./duperemove ]; then BINARY=./duperemove
	elif command -v duperemove >/dev/null 2>&1; then BINARY=$(command -v duperemove)
	else die "no duperemove binary; build it or pass --binary PATH"
	fi
fi
[ -x "$BINARY" ] || die "not executable: $BINARY"

command -v perf >/dev/null 2>&1 || die "perf not found (install linux-perf / perf)"

# Warn early if perf almost certainly cannot read symbols.
paranoid=$(cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null || echo 3)
if [ "${paranoid:-3}" -gt 1 ] 2>/dev/null && [ "$(id -u)" -ne 0 ]; then
	echo "perf-profile: warning: kernel.perf_event_paranoid=$paranoid; kernel symbols" >&2
	echo "              may be missing. To fix: sudo sysctl kernel.perf_event_paranoid=1 kernel.kptr_restrict=0" >&2
fi

PERF_DATA=$(mktemp /tmp/duperemove-perf.XXXXXX.data)
cleanup() { rm -f "$PERF_DATA" "$PERF_DATA.old"; }
trap cleanup EXIT

drop_caches() {
	sync
	if [ "$(id -u)" -eq 0 ]; then
		echo 3 > /proc/sys/vm/drop_caches
	elif sudo -n true 2>/dev/null; then
		echo 3 | sudo -n tee /proc/sys/vm/drop_caches >/dev/null
	else
		die "--cold needs root or passwordless sudo to drop caches"
	fi
}

run() { "$BINARY" "$@" >/dev/null 2>&1 || true; }

echo "== duperemove perf profile =="
echo "binary : $BINARY ($("$BINARY" --version 2>/dev/null | head -1))"
echo "kernel : $(uname -sr)"
echo "cpus   : $(nproc)"
echo "args   : $*"
echo "mode   : $([ "$COLD" = 1 ] && echo cold || echo warm), freq ${FREQ}Hz"
echo

if [ "$WARMUP" = 1 ]; then
	echo "-- warmup run --" >&2
	run "$@"
fi
[ "$COLD" = 1 ] && drop_caches

echo "-- recording --" >&2
# The profiled command mutates its hashfile, so a second run would be a warm
# no-op - the recorded run is the only representative one. Time it directly
# (includes perf's sampling overhead, so treat it as a ballpark).
_t0=$(date +%s.%N)
perf record -g --call-graph dwarf -F "$FREQ" -o "$PERF_DATA" -- "$BINARY" "$@" >/dev/null 2>&1 || \
	die "perf record failed (permissions? see --help)"
RUN_ELAPSED=$(awk "BEGIN{printf \"%.2f\", $(date +%s.%N) - $_t0}")

emit() {
	# Best-effort reporting: head closing a pipe early raises SIGPIPE, which
	# would trip pipefail/errexit, so relax strict mode for this function.
	set +e +o pipefail

	echo
	echo "===== SELF (where CPU is actually spent) ====="
	perf report -i "$PERF_DATA" --stdio --no-children --percent-limit 0.5 2>/dev/null \
		| grep -vE '^#|^$' | head -40
	echo
	echo "===== CALLERS (cumulative, with stacks) ====="
	# The syscall cost breakdown (statx/openat/getdents/pread ...) is visible
	# here, so no separate perf-trace pass is needed.
	perf report -i "$PERF_DATA" --stdio -g graph,0.5,caller --percent-limit 1 2>/dev/null \
		| grep -vE '^#|^$' | head -120
	echo
	echo "===== TIMING ====="
	echo "profiled run: ${RUN_ELAPSED}s ($([ "$COLD" = 1 ] && echo cold || echo warm) run, includes perf sampling overhead)"
	echo "For a precise A/B wall-clock, 'time' the binary directly with a fresh"
	echo "hashfile each run (the command mutates its hashfile, so a re-run is a"
	echo "no-op)$([ "$COLD" = 1 ] && echo ", dropping caches before each cold run" || echo "")."
}

if [ -n "$OUT" ]; then
	emit | tee "$OUT"
	echo "(report also written to $OUT)" >&2
else
	emit
fi
