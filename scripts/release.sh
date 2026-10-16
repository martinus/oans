#!/usr/bin/env bash
#
# release.sh - cut an oans release, following the convention in CLAUDE.md.
#
# The version lives only in git tags (the Makefile derives it from
# `git describe`), plus a human-facing string in the two man pages. A release is
# therefore: bump those two strings, land them on master via a PR, then tag
# master and publish a GitHub release. Merging a PR is a human decision, so this
# is two phases:
#
#   scripts/release.sh prepare X.Y.Z        # bump man pages, verify, open the PR
#   <review & merge the "Bump version to X.Y.Z" PR>
#   scripts/release.sh publish X.Y.Z [NOTES_FILE]   # tag master, create the release
#
# publish refuses to run until that PR is on master. With no NOTES_FILE it seeds
# the release body from the commit log since the previous tag (edit on GitHub
# after); pass a file to supply your own grouped notes.
#
# Honors PKG_CONFIG_PATH / DUPEREMOVE_TEST_DIR just like verify.sh (nothing
# machine-specific is baked in). Needs: git, an authenticated gh, and the build
# toolchain. gh always targets the fork (see $REPO).
#
# Every git operation goes to the remote whose URL names $REPO, not to
# `origin`: in a contributor's clone `origin` is a personal fork, which holds
# no tags, so `git describe` found no previous release there and the notes
# silently covered the whole history.
set -euo pipefail

REPO=martinus/oans
MAN8=docs/man/oans.8
MANMD=docs/man/oans.md

cd "$(dirname "$0")/.."
die() { echo "release: $*" >&2; exit 1; }

# The remote that is $REPO: github.com:OWNER/NAME or github.com/OWNER/NAME,
# with or without a trailing .git.
canonical_remote() {
	local r url
	for r in $(git remote); do
		url=$(git remote get-url "$r")
		url=${url%.git}
		if [[ $url == *"github.com:$REPO" || $url == *"github.com/$REPO" ]]; then
			printf '%s' "$r"
			return 0
		fi
	done
	die "no git remote points at $REPO (git remote add upstream git@github.com:$REPO.git)"
}

# Accept X.Y.Z or vX.Y.Z, always print the bare X.Y.Z; reject anything else.
normalize_version() {
	local v=${1#v}
	[[ $v =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "version must be X.Y.Z (got '$1')"
	printf '%s' "$v"
}

cmd=${1:-}
case "$cmd" in
prepare)
	[[ $# -eq 2 ]] || die "usage: release.sh prepare X.Y.Z"
	ver=$(normalize_version "$2")

	[[ -z "$(git status --porcelain)" ]] || die "working tree is not clean"
	remote=$(canonical_remote)
	git fetch -q --tags "$remote"
	git rev-parse -q --verify "refs/tags/v$ver" >/dev/null 2>&1 \
		&& die "tag v$ver already exists"

	git checkout -q -B "release/v$ver" "$remote/master"

	# Bump the two man-page version strings. Fail loudly if the expected
	# pattern is missing (format drift) rather than silently producing no change.
	grep -Eq '"oans [0-9]+\.[0-9]+\.[0-9]+"' "$MAN8" || die "no version string in $MAN8"
	grep -Eq '^footer: oans [0-9]+\.[0-9]+\.[0-9]+' "$MANMD" || die "no footer version in $MANMD"
	sed -i -E "s/\"oans [0-9]+\.[0-9]+\.[0-9]+\"/\"oans $ver\"/" "$MAN8"
	sed -i -E "s/^footer: oans [0-9]+\.[0-9]+\.[0-9]+/footer: oans $ver/" "$MANMD"
	git diff --quiet -- "$MAN8" "$MANMD" \
		&& die "man pages are already at $ver - nothing to bump"

	echo "==> verifying (build + tests + valgrind smoke)"
	bash scripts/verify.sh

	git commit -q -m "Bump version to $ver" -- "$MAN8" "$MANMD"
	git push -q -u "$remote" "release/v$ver"
	gh pr create --repo "$REPO" --base master --head "release/v$ver" \
		--title "Bump version to $ver" \
		--body "Release prep for oans $ver. Bumps the man-page version string; full notes go on the GitHub release."
	echo
	echo "Prepared v$ver. Review & merge the PR above, then:"
	echo "    scripts/release.sh publish $ver [NOTES_FILE]"
	;;
publish)
	[[ $# -ge 2 && $# -le 3 ]] || die "usage: release.sh publish X.Y.Z [NOTES_FILE]"
	ver=$(normalize_version "$2")
	notes_file=${3:-}
	[[ -z "$notes_file" || -f "$notes_file" ]] || die "notes file '$notes_file' not found"

	remote=$(canonical_remote)
	git fetch -q --tags "$remote"
	git rev-parse -q --verify "refs/tags/v$ver" >/dev/null 2>&1 \
		&& die "tag v$ver already exists"
	# The bump must already be on master, i.e. the prepare PR was merged.
	git show "$remote/master:$MANMD" | grep -q "^footer: oans $ver$" \
		|| die "$remote/master is not at $ver yet - merge the 'Bump version to $ver' PR first"

	git checkout -q --detach "$remote/master"

	# The previous release, for the notes. Not finding one means the tags are
	# missing, not that this is the first release: the notes would then list
	# every commit since the fork.
	prev=$(git describe --tags --abbrev=0 --match 'v[0-9]*' HEAD 2>/dev/null) \
		|| die "no previous v* tag reachable from $remote/master"

	# Resolve the notes body once (the supplied file, or a skeleton from the
	# commit log since the previous tag); the tag annotation and the GitHub
	# release share it. HEAD is the commit v$ver will point at, and the tag
	# doesn't exist yet, so `git describe HEAD` finds the previous release.
	if [[ -z "$notes_file" ]]; then
		notes_file=$(mktemp)
		trap 'rm -f "$notes_file"' EXIT
		{ echo "## Changes"; echo
		  git log --no-merges --pretty='- %s' "$prev..HEAD"; } > "$notes_file"
	fi

	git tag -a "v$ver" -F <(printf 'oans %s\n\n' "$ver"; cat "$notes_file")
	git push -q "$remote" "v$ver"
	# --verify-tag: if the push did not land, gh would otherwise create a
	# lightweight tag of its own on the default branch.
	gh release create "v$ver" --repo "$REPO" --verify-tag --title "oans v$ver" --notes-file "$notes_file"
	echo "Published https://github.com/$REPO/releases/tag/v$ver"
	;;
*)
	die "usage: release.sh {prepare|publish} X.Y.Z [NOTES_FILE]"
	;;
esac
