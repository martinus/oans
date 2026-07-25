#!/usr/bin/env bash
#
# Install the packages named in one or more .github/apt/*.txt lists, reusing
# .debs cached by a previous run where possible.
#
# .github/actions/apt-deps restores $APT_CACHE_DIR first, keyed on the runner
# image and the hash of these same lists - so a cache hit means this exact set
# was already resolved on this exact image, and dpkg can install it straight
# from disk. That skips `apt-get update`, which is where most of the 15-37s
# this replaces was going.
set -euo pipefail

if [ $# -eq 0 ]; then
    echo "apt-install: usage: apt-install.sh <package-list-file>..." >&2
    exit 1
fi

cache="${APT_CACHE_DIR:-$HOME/.apt-archives}"
mapfile -t pkgs < <(sed -e 's/#.*//' -e '/^[[:space:]]*$/d' "$@")
if [ ${#pkgs[@]} -eq 0 ]; then
    echo "apt-install: no packages listed in $*" >&2
    exit 1
fi

all_present() {
    local installed
    installed=$(dpkg-query -W -f='${Status}\n' "${pkgs[@]}" 2>/dev/null |
                grep -c '^install ok installed$') || true
    [ "$installed" -eq ${#pkgs[@]} ]
}

install_from_apt() {
    sudo apt-get update
    # apt drops privileges to _apt to download, so that user needs the archive
    # dir; without this it warns and refetches as root. It also insists on a
    # partial/ subdir in any archive dir it writes to.
    sudo mkdir -p "$cache/partial"
    sudo chown _apt:root "$cache" "$cache/partial"
    sudo apt-get install -y --no-install-recommends \
        -o Dir::Cache::archives="$cache" "${pkgs[@]}"
    # ... and actions/cache saves as the runner user, which cannot read those.
    sudo chown -R "$(id -u):$(id -g)" "$cache"
}

shopt -s nullglob
debs=("$cache"/*.deb)
if [ ${#debs[@]} -gt 0 ]; then
    echo "apt-install: cache hit, installing ${#debs[@]} cached packages"
    # dpkg succeeding is not the question - a partial cache can install cleanly
    # and still leave a package the build needs missing, which would surface far
    # away as a confusing compile error. Ask directly instead.
    if sudo dpkg --install --skip-same-version "${debs[@]}" && all_present; then
        exit 0
    fi
    echo "apt-install: cached packages did not satisfy the set, using apt-get" >&2
    # Only here can a half-applied dpkg run have left packages unconfigured,
    # and only after `apt-get update` are the lists present to repair it.
    sudo apt-get update
    sudo apt-get install -y -f
fi

install_from_apt
