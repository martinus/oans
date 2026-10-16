#!/usr/bin/env bash
#
# verify.sh - one-shot pre-PR gate. Runs, and stops at the first failure:
#
#   1. build oans and the unit suite from scratch with WERROR=1, so a warning
#      anywhere fails it
#   2. lint, C unit tests and the Python integration suite (make check)
#   3. a valgrind scan + dedupe + bare-replay smoke that must reclaim something
#
# Exits non-zero on the first failure, zero when everything passes.
#
# Honors the usual environment knobs (nothing repo-specific is baked in):
#   PKG_CONFIG_PATH      extra pkg-config search path (e.g. a local dev shim)
#   DUPEREMOVE_TEST_DIR  scratch dir for the integration suite and the valgrind
#                        smoke; dedupe needs a reflink fs (btrfs/xfs). Defaults
#                        to the harness default (.itest-scratch in the repo).
#
set -euo pipefail

cd "$(dirname "$0")/.."
step() { printf '\n=== %s ===\n' "$1"; }

# -B: an object that is already built prints nothing, so an incremental build
# could only ever see the warnings of the files that changed. test-build: the
# unit suite's translation units were otherwise first compiled by `make check`
# below, where nothing looked for warnings at all. The grep stays for what
# -Werror does not cover (the linker).
step "build (make -B WERROR=1 oans test-build)"
out=$(make -B -j"$(nproc)" WERROR=1 oans test-build 2>&1) || { echo "$out"; exit 1; }
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
	# The same default as tests/integration/harness.py, so the smoke runs
	# wherever the suite just did - not in /tmp, which is tmpfs on most
	# distros and which oans correctly refuses.
	scratch_root=${DUPEREMOVE_TEST_DIR:-$PWD/.itest-scratch}
	mkdir -p "$scratch_root"
	scratch=$(mktemp -d "$scratch_root/verify-smoke.XXXXXX")
	trap 'rm -rf "$scratch"' EXIT

	# The smoke deduplicates, so the scratch has to be reflink-capable. Check
	# up front: oans would refuse the tree, and a refusal looks like a smoke
	# that merely ended.
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
	# A real duplicate for dedupe to act on. Not cp: coreutils 9 goes through
	# copy_file_range(), which btrfs implements as a clone, so `b` already
	# shared `a`'s extent and the dedupe had nothing to do.
	dd if="$scratch/a" of="$scratch/b" bs=1M status=none
	hf="$scratch/hf.db"
	vg() {
		valgrind -q --leak-check=full --error-exitcode=42 \
			--suppressions=tests/valgrind.supp "$@"
	}
	# Keep the output instead of discarding it: on failure it is the only
	# explanation of what went wrong, and swallowing it is what made the tmpfs
	# case above so hard to read.
	smoke() {
		if ! out=$(vg "$@" 2>&1); then
			echo "$out" >&2
			echo "verify: valgrind smoke failed: $*" >&2
			exit 1
		fi
	}
	smoke ./oans -rd --hashfile="$hf" "$scratch"
	# A smoke that dedupes nothing runs none of the dedupe path it is here to
	# check, and still exits 0.
	if ! grep -Eq 'Reclaimed +[0-9.]*[1-9][0-9.]* [KMGTPE]?i?B across [1-9]' <<<"$out"; then
		echo "$out" >&2
		echo "verify: the valgrind smoke deduplicated nothing" >&2
		exit 1
	fi
	if command -v filefrag >/dev/null 2>&1 &&
	   ! filefrag -v "$scratch/b" | grep -qw shared; then
		echo "verify: the smoke reported a dedupe, but $scratch/b shares nothing" >&2
		exit 1
	fi
	smoke ./oans --hashfile="$hf"		# bare replay of the stored config
	echo "ok"
fi

printf '\nALL CHECKS PASSED\n'
