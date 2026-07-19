#!/usr/bin/env bash
# Build a reproducible demo tree for the oans GIF.
#
# Most files are UNIQUE — they get hashed but never deduplicated — so the scan /
# hashing phase is the long, visible one. Only a small set of groups is
# duplicated (real copies, --reflink=never), so the dedupe phase has a little to
# do but stays short. Unique file sizes follow an exponential distribution (many
# small, a few large), seeded so the mix is reproducible.
#
# Generation is parallel and spawn-free: one awk pass plans every path+size, then
# a pool of gen.py workers fill many files each with pseudo-random bytes (no
# per-file `head`), and copies are made with a parallel `cp`. Finally the page
# cache is dropped so the recorded scan reads cold from disk.
#
#   usage: setup.sh <work-dir on btrfs or xfs>
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

DEST="${1:?usage: setup.sh <work-dir on btrfs or xfs>}"
UNIQUE="${DEMO_UNIQUE:-2700}"         # unique files: hashed, never deduped (long scan)
DUP_GROUPS="${DEMO_DUP_GROUPS:-100}"  # duplicated groups: the (short) dedupe work
COPIES="${DEMO_COPIES:-3}"            # files per duplicated group (incl. the original)
MEAN_KB="${DEMO_MEAN_KB:-2048}"       # mean file size, KiB (exponential)
MIN_KB="${DEMO_MIN_KB:-16}"           # clamp: smallest file
MAX_KB="${DEMO_MAX_KB:-65536}"        # clamp: largest file (64 MiB)
SEED="${DEMO_SEED:-1}"                # RNG seed -> reproducible size mix

mkdir -p "$DEST"
fstype=$(stat -f -c %T "$DEST")
case "$fstype" in
  btrfs|xfs) ;;
  *) echo "error: $DEST is on '$fstype'; oans needs btrfs or xfs (reflink) or dedupe is a silent no-op." >&2
     exit 1 ;;
esac

rm -rf "$DEST/tree"; rm -f "$DEST"/demo.hash* "$DEST"/.jobs_* "$DEST"/.chunk_* "$DEST"/.reclaim
mkdir -p "$DEST/tree/data" "$DEST/tree/dup"
nworkers=$(nproc 2>/dev/null || echo 4)

# 1) Plan everything in one awk pass. Sizes are exponential (s = -mean*ln(U)),
#    clamped and rounded to 4 KiB. Emit a "generate" list (bytes<TAB>path, one
#    per unique file and per group original) and a "copy" list (src<TAB>dst).
awk -v uniq="$UNIQUE" -v groups="$DUP_GROUPS" -v copies="$COPIES" \
    -v mean="$MEAN_KB" -v min="$MIN_KB" -v max="$MAX_KB" -v seed="$SEED" -v dest="$DEST" '
  function esize(   u, s) {
    u = rand(); if (u < 1e-9) u = 1e-9; s = -mean * log(u);
    if (s < min) s = min; if (s > max) s = max;
    s = int(s / 4) * 4; if (s < 4) s = 4; return s;
  }
  BEGIN {
    srand(seed);
    gen = dest "/.jobs_gen"; cpy = dest "/.jobs_copy";
    for (i = 1; i <= uniq; i++) {
      s = esize();
      printf "%d\t%s/tree/data/set_%03d/u_%04d.bin\n", s*1024, dest, int((i-1)/300), i > gen;
    }
    for (g = 1; g <= groups; g++) {
      s = esize();
      orig = sprintf("%s/tree/dup/group_%03d/original.bin", dest, g);
      printf "%d\t%s\n", s*1024, orig > gen;
      for (c = 1; c < copies; c++)
        printf "%s\t%s/tree/dup/group_%03d/copy_%02d.bin\n", orig, dest, g, c > cpy;
      reclaim += s * (copies - 1);
    }
    printf "%d\n", reclaim > (dest "/.reclaim");
  }'

# 2) Pre-create the directories the workers write into (spawn-free via xargs).
awk -v n="$(( (UNIQUE + 299) / 300 ))" -v dest="$DEST" \
  'BEGIN { for (i = 0; i < n; i++) printf "%s/tree/data/set_%03d\0", dest, i }' | xargs -0 mkdir -p
awk -v n="$DUP_GROUPS" -v dest="$DEST" \
  'BEGIN { for (g = 1; g <= n; g++) printf "%s/tree/dup/group_%03d\0", dest, g }' | xargs -0 mkdir -p

echo "Generating $UNIQUE unique files + $DUP_GROUPS groups ×${COPIES} (mean ${MEAN_KB} KiB) on ${nworkers} workers ..."

# 3) Generate the file content in parallel: split the gen list into nworkers
#    chunks and hand each to a gen.py worker (pseudo-random bytes; see gen.py).
split -n "l/$nworkers" "$DEST/.jobs_gen" "$DEST/.chunk_gen." 2>/dev/null \
  || cp "$DEST/.jobs_gen" "$DEST/.chunk_gen.aa"
for ch in "$DEST"/.chunk_gen.*; do python3 "$here/gen.py" "$ch" & done
wait

# 4) Make the duplicate copies in parallel. --reflink=never is essential: on
#    btrfs `cp` reflink-copies by default, leaving oans nothing to reclaim.
if [ -s "$DEST/.jobs_copy" ]; then
  tr '\t\n' '\0\0' < "$DEST/.jobs_copy" | xargs -0 -n2 -P"$nworkers" cp --reflink=never
fi

reclaim_kb=$(cat "$DEST/.reclaim")
rm -f "$DEST"/.jobs_gen "$DEST"/.jobs_copy "$DEST"/.chunk_gen.* "$DEST"/.reclaim

# 5) Drop the page cache so the recorded scan reads cold from disk (realistic,
#    and it makes the hashing phase visibly longer). Needs root; best-effort.
if [ "${DROP_CACHES:-1}" = 1 ]; then
  sync
  if ! { echo 3 > /proc/sys/vm/drop_caches; } 2>/dev/null; then
    sudo sh -c 'sync; echo 3 > /proc/sys/vm/drop_caches' 2>/dev/null \
      || echo "note: could not drop caches (need root); scan reads warm cache. Set DROP_CACHES=0 to skip." >&2
  fi
fi

printf 'tree: %s  (%d files = %d unique + %d groups x%d; ~%d MiB reclaimable across %d groups)\n' \
  "$(du -sh "$DEST/tree" | cut -f1)" \
  "$(( UNIQUE + DUP_GROUPS * COPIES ))" \
  "$UNIQUE" "$DUP_GROUPS" "$COPIES" \
  "$(( reclaim_kb / 1024 ))" "$DUP_GROUPS"
