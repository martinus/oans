"""Files whose absolute path exceeds PATH_MAX are hashed and deduplicated, not
skipped (issue #117, the follow-through to #108/#115).

A single open()/statx()/stat() argument longer than PATH_MAX (4096) is rejected
by the kernel with ENAMETOOLONG, so oans reaches such a file by opening a
reachable ancestor and openat-walking the rest (src/longpath.c). These tests
build over-PATH_MAX trees on a reflink fs and check the whole life cycle: scan
(hash), dedupe, no-op rescan, and stat-based prune. Both shapes are covered:

  * a *long filename* in an in-range directory (parent <= PATH_MAX, leaf over) —
    one openat away; and
  * a *deep directory chain* whose parent path is itself over PATH_MAX — the
    multi-level openat chain (this is what #108's reproducer built).

The trees are built with incremental relative chdir()s, and the leaf files are
reopened for verification through a dir fd captured at the bottom, because the
test process cannot name them with an absolute path either.
"""

import os

from harness import DuperemoveTest, requires_reflink

# The kernel PATH_MAX on Linux; a single path component maxes out at NAME_MAX
# (255). Chaining 255-char directories steps the absolute path in ~256-byte
# jumps, so a handful of levels clears 4096.
PATH_MAX = 4096
NAME_MAX = 255


class LongPathTest(DuperemoveTest):
    # -- builders ----------------------------------------------------------

    def _descend_deep(self, deep_parent):
        """chdir down a chain of 255-char dirs under a fresh 'deep' root.

        If deep_parent, keep going until the *directory* path itself exceeds
        PATH_MAX (the leaf is then reached via a multi-level openat chain);
        otherwise stop while the directory stays in range so only the final
        255-char filename tips the leaf over PATH_MAX (one openat away). Leaves
        the process cwd at the leaf directory; returns (top, dir_len).
        """
        top = self.path("deep")           # short, in-range root to point oans at
        os.makedirs(top, exist_ok=True)
        cwd = os.getcwd()
        self.addCleanup(os.chdir, cwd)    # never leave the test in the deep tree
        os.chdir(top)

        comp = "d" * NAME_MAX
        cur_len = len(top)
        # deep_parent: descend until the dir path itself exceeds PATH_MAX.
        # else: stop one component short, so only the 255-char filename tips over.
        limit = PATH_MAX if deep_parent else PATH_MAX - NAME_MAX - 1
        while cur_len <= limit:
            os.mkdir(comp)
            os.chdir(comp)
            cur_len += 1 + NAME_MAX
        return top, cur_len

    def _make_victims(self, victims, deep_parent=True):
        """Build a deep dir and create the given {name: bytes} victim files in
        it. Returns (top, dir_len, deep_fd) where deep_fd is an fd to the leaf
        directory (for reopening the over-PATH_MAX victims relatively). Restores
        the process cwd before returning."""
        orig = os.getcwd()
        top, dir_len = self._descend_deep(deep_parent)   # cwd now at the leaf
        for name, content in victims.items():
            assert len(name) <= NAME_MAX
            with open(name, "wb") as f:
                f.write(content)
        deep_fd = os.open(".", os.O_RDONLY | os.O_DIRECTORY)
        self.addCleanup(os.close, deep_fd)
        os.chdir(orig)                    # leave the tree; oans uses abs paths
        return top, dir_len, deep_fd

    def _hf_has(self, victim):
        return self.hf_scalar(
            "select count(*) from files where filename like ?",
            (f"%{victim}",))

    # -- scan / hash -------------------------------------------------------

    @requires_reflink
    def test_deep_chain_is_hashed(self):
        """A file under a >PATH_MAX directory chain is hashed, not skipped."""
        victim = "v" * NAME_MAX
        top, dir_len, _ = self._make_victims(
            {victim: b"over-the-limit contents\n"}, deep_parent=True)
        self.assertGreater(dir_len, PATH_MAX)   # parent itself over PATH_MAX

        self.scan(top)
        self.assertDmOk()
        self.assertNotIn("exceeds PATH_MAX", self.out,
                         "the over-PATH_MAX file must now be hashed, not skipped")
        self.assertEqual(1, self._hf_has(victim),
                         "the deep file is stored in the hashfile")

    @requires_reflink
    def test_long_basename_in_range_dir(self):
        """A long *filename* in an in-range directory (single openat) hashes."""
        victim = "v" * NAME_MAX
        top, dir_len, _ = self._make_victims(
            {victim: b"long basename contents\n"}, deep_parent=False)
        self.assertLessEqual(dir_len, PATH_MAX)          # parent reachable
        self.assertGreater(dir_len + 1 + NAME_MAX, PATH_MAX)  # leaf over

        self.scan(top)
        self.assertDmOk()
        self.assertNotIn("exceeds PATH_MAX", self.out)
        self.assertEqual(1, self._hf_has(victim))

    # -- dedupe ------------------------------------------------------------

    @requires_reflink
    def test_deep_chain_dedupe(self):
        """Two identical over-PATH_MAX files deduplicate (share storage)."""
        a, b = "a" * NAME_MAX, "b" * NAME_MAX
        data = os.urandom(128 * 1024)
        top, _, deep_fd = self._make_victims({a: data, b: data},
                                             deep_parent=True)
        self.sync()
        self.assertNotShared(a, b, "independently written files start unshared",
                             dir_fd=deep_fd)

        self.dedupe(top)
        self.assertDmOk()
        self.assertNotIn("exceeds PATH_MAX", self.out)
        self.sync()
        self.assertShared(
            a, b,
            "identical over-PATH_MAX files must share storage after dedupe",
            dir_fd=deep_fd)

    @requires_reflink
    def test_deep_branch_does_not_derail_siblings(self):
        """An ordinary duplicate pair still dedupes when a >PATH_MAX branch
        sits beside it in the same scan.

        The deep branch is the risky one: anything that makes the walk bail
        early on it (an ENAMETOOLONG that aborts a directory, a probe that
        rejects a subtree) would silently cost the *rest* of the tree too. Every
        other test here scans a tree made only of long paths, so none of them
        would notice. Scan the whole work dir, not just `top`.
        """
        deep_victim = "v" * NAME_MAX
        self._make_victims({deep_victim: b"deep contents\n"}, deep_parent=True)
        a, b = self.mkdup("pair/a.bin", "pair/b.bin", 128 * 1024)

        self.dedupe(self.work)
        self.assertDmOk()
        self.assertShared(a, b,
                          "in-range duplicates must dedupe despite an "
                          "over-PATH_MAX sibling branch")
        self.assertEqual(1, self._hf_has(deep_victim),
                         "the deep file is hashed in the same run")

    # -- no-op rescan ------------------------------------------------------

    @requires_reflink
    def test_deep_chain_noop_rescan(self):
        """Rescanning a deep tree changes nothing (identity invariant)."""
        victim = "v" * NAME_MAX
        top, _, _ = self._make_victims({victim: b"stable\n"}, deep_parent=True)

        self.scan(top)
        self.assertDmOk()
        files_fp, extents_fp = self.files_fingerprint(), self.extents_fingerprint()
        n_files = self.hf_count("files")

        self.scan(top)
        self.assertDmOk()
        self.assertEqual(n_files, self.hf_count("files"))
        self.assertEqual(files_fp, self.files_fingerprint())
        self.assertEqual(extents_fp, self.extents_fingerprint())

    # -- prune -------------------------------------------------------------

    @requires_reflink
    def test_deep_chain_prune_is_selective(self):
        """Deleting one over-PATH_MAX file prunes only its row; a long-path
        sibling is kept (a naive stat() would ENAMETOOLONG and prune both)."""
        gone, kept = "g" * NAME_MAX, "k" * NAME_MAX
        top, _, deep_fd = self._make_victims(
            {gone: b"delete me\n", kept: b"keep me\n"}, deep_parent=True)

        self.scan(top)
        self.assertDmOk()
        self.assertEqual(1, self._hf_has(gone))
        self.assertEqual(1, self._hf_has(kept))

        os.unlink(gone, dir_fd=deep_fd)   # remove the deep file relatively

        self.scan(top)
        self.assertDmOk()
        self.assertEqual(0, self._hf_has(gone), "deleted long-path file is pruned")
        self.assertEqual(1, self._hf_has(kept), "surviving long-path file is kept")
