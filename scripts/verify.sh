#!/usr/bin/env bash
#
# verify.sh - one-shot pre-PR gate. Runs, and stops at the first failure:
#
#   1. build (make -j) - any compiler warning is treated as a failure
#   2. C unit tests           (make test)
#   3. Python integration suite
#   4. a valgrind scan + dedupe + bare-replay smoke
#
# Exits non-zero on the first failure, zero when everything passes.
#
# Honors the usual environment knobs (nothing repo-specific is baked in):
#   PKG_CONFIG_PATH      extra pkg-config search path (e.g. a local dev shim)
#   DUPEREMOVE_TEST_DIR  scratch dir for the integration suite and the valgrind
#                        smoke; dedupe needs a reflink fs (btrfs/xfs). Defaults
#                        to the harness default (.itest-scratch next to the repo).
#
set -euo pipefail

cd "$(dirname "$0")/.."
step() { printf '\n=== %s ===\n' "$1"; }

step "build (make -j, warnings are failures)"
out=$(make -j"$(nproc)" 2>&1) || { echo "$out"; exit 1; }
echo "$out"
if echo "$out" | grep -iEq 'warning:|error:'; then
	echo "verify: build produced warnings/errors" >&2
	exit 1
fi

step "unit + integration tests (make check)"
make check

step "valgrind smoke (scan + dedupe + bare replay)"
if ! command -v valgrind >/dev/null 2>&1; then
	echo "valgrind not installed - skipping smoke"
else
	scratch=$(mktemp -d "${DUPEREMOVE_TEST_DIR:-${TMPDIR:-/tmp}}/verify-smoke.XXXXXX")
	trap 'rm -rf "$scratch"' EXIT

	# The smoke deduplicates, so the scratch has to be reflink-capable. With
	# DUPEREMOVE_TEST_DIR unset it lands in /tmp, which is tmpfs on most
	# distros: oans then correctly refuses the tree and exits 1 - but its
	# message went to the >/dev/null below, so this script stopped dead with no
	# output at all and looked like it had simply ended. Check up front instead.
	fstype=$(stat -f -c %T "$scratch" 2>/dev/null || echo unknown)
	case "$fstype" in
	btrfs|xfs) ;;
	*)
		echo "verify: scratch '$scratch' is on '$fstype', but the valgrind smoke" >&2
		echo "        deduplicates and needs btrfs or xfs. Point DUPEREMOVE_TEST_DIR" >&2
		echo "        at a reflink-capable directory, e.g.:" >&2
		echo "            DUPEREMOVE_TEST_DIR=\$HOME/.itest-scratch $0" >&2
		exit 1 ;;
	esac

	head -c 1048576 /dev/urandom > "$scratch/a"
	cp "$scratch/a" "$scratch/b"		# a real duplicate for dedupe to act on
	hf="$scratch/hf.db"
	vg() {
		valgrind -q --leak-check=full --error-exitcode=42 \
			--suppressions=tests/valgrind.supp "$@"
	}
	# Keep the output instead of discarding it: on failure it is the only
	# explanation of what went wrong, and swallowing it is what made the tmpfs
	# case above so hard to read.
	smoke() {
		local out
		if ! out=$(vg "$@" 2>&1); then
			echo "$out" >&2
			echo "verify: valgrind smoke failed: $*" >&2
			exit 1
		fi
	}
	smoke ./oans -rd --hashfile="$hf" "$scratch"
	smoke ./oans --hashfile="$hf"		# bare replay of the stored config
	echo "ok"
fi

printf '\nALL CHECKS PASSED\n'
