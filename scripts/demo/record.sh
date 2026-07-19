#!/usr/bin/env bash
# One command to (re)record the oans demo GIF, fully automatically.
#
# It runs in the directory you start it from, so cd to a short scratch path for
# tidy-looking paths in --stats/--history, e.g.:
#
#   mkdir -p ~/d && cd ~/d && /path/to/oans/scripts/demo/record.sh   # -> ~/d/demo.gif
#
#   DEMO_DIR=/srv/demo record.sh   # or force the work dir explicitly
#   OUT=~/oans.gif        record.sh # choose the output path
#   KEEP=1                record.sh # keep the generated dataset
#
# Picks VHS if installed (highest quality), else asciinema + agg. The work dir
# must be on btrfs/xfs so the dedupe is real. No root needed — you dedupe files
# you own. WARNING: it creates/removes a ./tree and ./demo.hash* here, so run it
# from a scratch directory.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
WORK="${DEMO_DIR:-$PWD}"
OUT="${OUT:-$WORK/demo.gif}"
FONT="${FONT:-Hack}"
LOSSY="${DEMO_LOSSY:-60}"   # gifsicle --lossy level; higher = smaller GIF, more speckle

# Build oans if needed and expose it on PATH for the recording — without copying
# the binary into the work dir, so we never risk deleting your real ./oans.
if [ ! -x "$repo/oans" ]; then
  echo "building oans ..."; make -C "$repo" -j"$(nproc)"
fi
export OANS_BIN_DIR="$repo"

"$here/setup.sh" "$WORK"

if command -v vhs >/dev/null; then
  echo "recording with vhs ..."
  ( cd "$WORK" && vhs "$here/demo.tape" )   # writes demo.gif
  [ "$WORK/demo.gif" -ef "$OUT" ] 2>/dev/null || mv -f "$WORK/demo.gif" "$OUT"
elif command -v asciinema >/dev/null && command -v agg >/dev/null; then
  echo "recording with asciinema + agg ..."
  # Force v2 (agg can't read asciinema 3.x's default v3) and pin the size so the
  # GIF dimensions are deterministic even when run headless / in CI.
  ( cd "$WORK" && asciinema rec --overwrite -f asciicast-v2 --window-size 100x30 \
      --command "bash '$here/play.sh'" demo.cast )
  agg --font-family "$FONT" --font-size 22 --theme dracula --speed 1.0 \
      "$WORK/demo.cast" "$OUT"
else
  echo "error: need either 'vhs', or 'asciinema' + 'agg'." >&2
  echo "  Fedora:  sudo dnf install vhs ttyd      # ffmpeg already present" >&2
  echo "  or:      pipx install asciinema ; cargo install agg" >&2
  exit 1
fi

# Optimize the GIF: a quality-neutral ~30% shrink for flat terminal colours.
if command -v gifsicle >/dev/null; then
  gifsicle -O3 --lossy="$LOSSY" "$OUT" -o "$OUT.tmp" 2>/dev/null && mv -f "$OUT.tmp" "$OUT" || rm -f "$OUT.tmp"
else
  echo "note: install gifsicle to shrink the GIF (~30% smaller)." >&2
fi

# Tidy up the dataset (keep the gif).
if [ "${KEEP:-0}" != 1 ]; then
  rm -rf "$WORK/tree"; rm -f "$WORK"/demo.hash* "$WORK/demo.cast"
fi

echo "wrote $OUT ($(du -h "$OUT" | cut -f1))"
