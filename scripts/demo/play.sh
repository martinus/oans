#!/usr/bin/env bash
# The oans demo SCREENPLAY — this is "what the GIF shows", top to bottom.
# Run it standalone to preview, or let record.sh record it (asciinema+agg path).
#
# Expects the current directory to be the work dir prepared by setup.sh, holding
# the freshly-built ./oans binary and the ./tree dataset.
set -u
# Use the oans exposed by record.sh (falls back to ./oans for standalone preview).
export PATH="${OANS_BIN_DIR:-$PWD}:$PATH"

# Echo a command as if it were typed (green prompt), pause, then run it. LEAD is
# the pause before running; GAP is the reading pause after it finishes.
run() {
  printf '\033[38;5;114m$\033[0m %s\n' "$*"
  sleep "${LEAD:-1.2}"
  "$@"
  echo
  sleep "${GAP:-4}"
}

clear

# 1) First run — hash the tree and deduplicate it. The star of the show:
#    the live progress bar (rate + ETA) and the honest "Reclaimed" summary.
run oans -dr --hashfile=demo.hash tree

# 2) Inspect the hashfile: files, hashes, duplication, reclaimable space.
run oans --stats --hashfile=demo.hash

# 3) The headline feature — a re-run replays the stored config and skips
#    everything already shared, so it finishes almost instantly.
run oans --hashfile=demo.hash

# 4) Space reclaimed over time.
run oans --history --hashfile=demo.hash

sleep 1
