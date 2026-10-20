---
name: release
description: Cuts an oans release in two phases with scripts/release.sh, a version-bump PR and then the tag and GitHub release. Use when the user asks to release, tag or publish a new oans version, or asks what the next version number should be.
argument-hint: "X.Y.Z"
---

# Releasing

The version is `git describe --tags` (Makefile) — **a release is just a tag**,
no `VERSION` file. **`scripts/release.sh`** encodes the convention in two phases
(merging the bump PR is a human step):

```sh
scripts/release.sh prepare X.Y.Z          # bump man pages, run verify.sh, open the PR
#   → review & merge the "Bump version to X.Y.Z" PR
scripts/release.sh publish X.Y.Z [NOTES]  # tag the merged master + create the GH release
```

`prepare` refuses on a dirty tree or existing tag; `publish` refuses until the
bump is on `origin/master`, then tags and releases (auto-seeds notes from the
commit log if no `NOTES` file). Publishing the release fires
`.github/workflows/release.yml`, which builds a prebuilt x86_64 tarball
(`oans-X.Y.Z-linux-x86_64.tar.gz` + `.sha256`, built on Ubuntu 22.04 for a low
glibc floor) and attaches it; `workflow_dispatch` with a `tag` input backfills
assets for an existing release.

- **Incremental semver.** The CLI is a superset of duperemove's and hashfiles
  auto-rebuild, so feature batches are backward-compatible → **minor** bumps
  (`1.1.1`→`1.2.0`); reserve major for a real break. (`1.2.0` was the first
  release carrying the fork's headline features.)
