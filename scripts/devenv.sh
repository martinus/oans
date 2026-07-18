#!/usr/bin/env bash
#
# devenv.sh - SOURCE this once per shell to set up the local dev-box environment
# for building and testing oans on a sandbox that can't install the -devel
# packages system-wide:
#
#   source scripts/devenv.sh
#   make -j"$(nproc)" && scripts/verify.sh
#
# Every knob is set only if its target exists, so sourcing this is a harmless
# no-op on a normal machine (where the README's dnf/apt deps cover everything).
#
# The pkg-config shim itself - headers/libs for uuid, libbsd and xxhash under
# /tmp/devroot, referenced by the two .pc files in /tmp/devroot/pc - is a local,
# uncommitted artifact; recreate it there if /tmp was cleared.

# Header/lib shim so pkg-config finds uuid, libbsd, xxhash.
[ -d /tmp/devroot/pc ] &&
	export PKG_CONFIG_PATH="/tmp/devroot/pc${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"

# Integration-test scratch dir; must be reflink-capable (btrfs/xfs).
[ -d "$HOME/.itest-scratch" ] &&
	export DUPEREMOVE_TEST_DIR="$HOME/.itest-scratch"

# Static pandoc for `make doc` (man-page regeneration).
[ -d /tmp/pandoc-3.6.4/bin ] &&
	export PATH="/tmp/pandoc-3.6.4/bin:$PATH"

# Sourced, not executed: end on a success so a failed [ -d ] test above does not
# leave $? non-zero in the caller's shell.
:
