# AUR packaging

Source `PKGBUILD`s for the Arch User Repository, kept in-tree so they version
alongside the code:

- [`oans/PKGBUILD`](oans/PKGBUILD) — stable, builds from the latest release tag.
- [`oans-git/PKGBUILD`](oans-git/PKGBUILD) — VCS package, builds from `master`.

Both install a `duperemove` compatibility symlink, so they `provides`/`conflicts`
with the `duperemove` package.

`.SRCINFO` is committed next to each `PKGBUILD` so the two never drift, and so
publishing is a copy rather than a regeneration on a machine that may not have
`makepkg`.

## First-time publishing

One-off, and it needs a human: an AUR account with an SSH key uploaded at
<https://aur.archlinux.org/account>.

```sh
git clone ssh://aur@aur.archlinux.org/oans.git aur-oans   # empty on first use
cp packaging/aur/oans/{PKGBUILD,.SRCINFO} aur-oans/
cd aur-oans && git add PKGBUILD .SRCINFO
git commit -m "Initial import: oans 1.11.1" && git push origin master
```

Repeat with `oans-git`. The push is what creates the package; there is no
separate "submit" step.

## On a new release

1. Bump `pkgver` and reset `pkgrel=1` in [`oans/PKGBUILD`](oans/PKGBUILD).
2. Refresh the checksum. On Arch that is `updpkgsums`; anywhere else:

   ```sh
   curl -sL https://github.com/martinus/oans/archive/refs/tags/vX.Y.Z.tar.gz | sha256sum
   ```

3. Mirror both into [`oans/.SRCINFO`](oans/.SRCINFO) (`pkgver`, `source`,
   `sha256sums`), or regenerate it with `makepkg --printsrcinfo > .SRCINFO`.
4. Copy both files into the AUR clone, commit, push.

`oans-git` needs none of this: no checksum, and its `pkgver()` derives the
version from `git describe` at build time.

## Why the PKGBUILD passes VERSION

The Makefile takes its version from `git describe`, which a release tarball has
no metadata for, so an unpatched build reports `oans unknown`. The `_make`
wrapper passes `VERSION="v$pkgver"` to **every** make invocation — not just
`build()`. The Makefile rebuilds when `VERSTRING` changes, so a `package()` that
omitted it would quietly rebuild the binary back to `unknown` after `build()` got
it right.
