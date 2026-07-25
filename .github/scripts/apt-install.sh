#!/bin/bash
#
# Install a package list from .github/apt/, reusing .debs cached by a previous
# run where possible.
#
# The workflow restores $APT_CACHE_DIR with actions/cache before calling this,
# keyed on the runner image *and* the hash of the list file - so a cache hit
# means the exact same packages were resolved on the exact same image. That is
# what makes the fast path safe: the cache holds every .deb apt downloaded last
# time, including dependencies, so dpkg can install them straight from disk
# with no `apt-get update` and no network at all.
#
# apt-get was taking 15-37s per job, which is most of the wall time of the
# short legs (build (xfs, gcc) spent 28s of 33s here) and ~14% of the valgrind
# leg that sets CI's critical path.
#
# Any doubt falls back to plain apt-get: a miss, a stale cache, or a dpkg that
# will not take the set. The fallback is the old behaviour exactly, so the
# worst case is the speed we had before, never a broken job.
set -euo pipefail

list="${1:?usage: apt-install.sh <package-list-file>}"
cache="${APT_CACHE_DIR:-$HOME/.apt-archives}"

# apt insists on a partial/ subdir inside any archives dir it writes to.
mkdir -p "$cache/partial"

mapfile -t pkgs < <(sed -e 's/#.*//' -e '/^[[:space:]]*$/d' "$list")
if [ ${#pkgs[@]} -eq 0 ]; then
    echo "no packages listed in $list" >&2
    exit 1
fi

all_present() {
    local pkg
    for pkg in "${pkgs[@]}"; do
        dpkg-query -W -f='${Status}' "$pkg" 2>/dev/null \
            | grep -q "^install ok installed$" || return 1
    done
}

install_from_apt() {
    sudo apt-get update
    # -f first: a failed fast path can leave packages unconfigured, and this is
    # the only place with the package lists needed to repair that.
    sudo apt-get install -y -f
    # apt drops privileges to _apt to download, so that user needs the archive
    # dir; without this it warns and falls back to fetching as root.
    sudo chown -R _apt:root "$cache"
    sudo apt-get install -y --no-install-recommends \
        -o Dir::Cache::archives="$cache" "${pkgs[@]}"
    # ... and actions/cache saves as the runner user, which cannot read those.
    sudo chown -R "$(id -u):$(id -g)" "$cache"
}

if compgen -G "$cache/*.deb" >/dev/null; then
    count=$(find "$cache" -maxdepth 1 -name '*.deb' | wc -l)
    echo "apt cache hit: installing $count cached packages"
    # dpkg succeeding is not the question - a partial cache can install cleanly
    # and still leave a package the build needs missing, which would surface far
    # away as a confusing compile error. Ask directly instead.
    if sudo dpkg --install --skip-same-version "$cache"/*.deb && all_present; then
        exit 0
    fi
    echo "cached packages did not satisfy the set; falling back to apt-get" >&2
fi

install_from_apt

if ! all_present; then
    echo "packages still missing after apt-get; see the list above" >&2
    exit 1
fi
