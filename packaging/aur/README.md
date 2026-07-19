# AUR packaging

Source `PKGBUILD`s for the Arch User Repository, kept in-tree so they version
alongside the code:

- [`oans/PKGBUILD`](oans/PKGBUILD) — stable, builds from the latest release tag.
- [`oans-git/PKGBUILD`](oans-git/PKGBUILD) — VCS package, builds from `master`.

Both install a `duperemove` compatibility symlink, so they `provides`/`conflicts`
with the `duperemove` package.

## Publishing to the AUR

The AUR expects each package in its own git repo containing `PKGBUILD` and a
generated `.SRCINFO`. To publish or update `oans`:

```sh
cd packaging/aur/oans
makepkg --printsrcinfo > .SRCINFO   # regenerate whenever PKGBUILD changes
# then push PKGBUILD + .SRCINFO to ssh://aur@aur.archlinux.org/oans.git
```

## On a new release

Bump `pkgver`, reset `pkgrel=1`, and refresh the checksum in
[`oans/PKGBUILD`](oans/PKGBUILD):

```sh
updpkgsums          # rewrites sha256sums from the release tarball
```

`oans-git` needs no checksum or manual `pkgver` bump — its `pkgver()` derives the
version from `git describe`.
