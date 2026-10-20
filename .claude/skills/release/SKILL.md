---
name: release
description: Cuts an oans release in two phases with scripts/release.sh, a version-bump PR and then the tag and GitHub release. Use when the user asks to release, tag or publish a new oans version, or asks what the next version number should be.
argument-hint: "X.Y.Z"
---

# Releasing

- **A release is just a tag**: the version is `git describe --tags`
  (Makefile), no `VERSION` file.
- `scripts/release.sh` has two phases; merging the bump PR is a human step:

```sh
scripts/release.sh prepare X.Y.Z          # bump man pages, run verify.sh, open the PR
#   → review & merge the "Bump version to X.Y.Z" PR
scripts/release.sh publish X.Y.Z [NOTES]  # tag the merged master + create the GH release
```

- `prepare` refuses on a dirty tree or an existing tag. `publish` refuses
  until the bump is on `origin/master`, then tags and releases; without a
  `NOTES` file it seeds notes from the commit log.
- Publishing fires `.github/workflows/release.yml`: it builds and attaches
  `oans-X.Y.Z-linux-x86_64.tar.gz` + `.sha256` (built on Ubuntu 22.04 for a
  low glibc floor). `workflow_dispatch` with a `tag` input backfills assets
  for an existing release.
- **Incremental semver: feature batches are a minor bump** (`1.1.1`→`1.2.0`):
  the CLI is a superset of duperemove's and hashfiles auto-rebuild. Major only
  for a real break.
