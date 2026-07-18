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
# Override any location if your setup differs, e.g.
#   OANS_DEVROOT=/opt/oans-shim source scripts/devenv.sh
#
# The pkg-config shim itself - headers/libs for uuid, libbsd and xxhash under
# $OANS_DEVROOT, referenced by the two .pc files in $OANS_DEVROOT/pc - is a
# local, uncommitted artifact; recreate it there if /tmp was cleared.

: "${OANS_DEVROOT:=/tmp/devroot}"             # pkg-config header/lib shim
: "${OANS_TEST_DIR:=$HOME/.itest-scratch}"    # reflink-capable test scratch dir
: "${OANS_PANDOC_BIN:=/tmp/pandoc-3.6.4/bin}" # static pandoc for `make doc`

[ -d "$OANS_DEVROOT/pc" ] &&
	export PKG_CONFIG_PATH="$OANS_DEVROOT/pc${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
[ -d "$OANS_TEST_DIR" ] &&
	export DUPEREMOVE_TEST_DIR="$OANS_TEST_DIR"
[ -d "$OANS_PANDOC_BIN" ] &&
	export PATH="$OANS_PANDOC_BIN:$PATH"

# Sourced, not executed: end on success so a failed [ -d ] test above does not
# leave $? non-zero in the caller's shell.
:
