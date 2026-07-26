"""--exclude: gitignore-style glob matching (see src/glob.h)."""

import os

from harness import DuperemoveTest


class ExcludeTest(DuperemoveTest):
    def test_exclude_literal_path(self):
        self.mkrand("tree/keep", 8000)
        self.mkrand("tree/drop", 8000)
        self.scan(self.path("tree"), "--exclude=" + self.path("tree/drop"))
        self.assertDmOk()
        self.assertEqual(1, self.hf_count("files"), "excluded file not recorded")
        self.assertEqual(
            0, self.hf_scalar("select count(*) from files where filename like '%/drop'"),
            "the dropped path is absent")
        self.assertEqual(
            1, self.hf_scalar("select count(*) from files where filename like '%/keep'"),
            "the kept path is present")

    def test_exclude_glob(self):
        self.mkrand("tree/a.log", 4000)
        self.mkrand("tree/b.log", 4000)
        self.mkrand("tree/c.txt", 4000)
        self.scan(self.path("tree"), "--exclude=" + self.path("tree/*.log"))
        self.assertDmOk()
        self.assertEqual(1, self.hf_count("files"), "only the non-.log file survives")
        self.assertEqual(
            1, self.hf_scalar("select count(*) from files where filename like '%.txt'"),
            "the .txt file is kept")

    def test_exclude_subtree(self):
        self.mkrand("tree/top", 4000)
        self.mkrand("tree/skip/x", 4000)
        self.mkrand("tree/skip/y", 4000)
        self.scan(self.path("tree"), "--exclude=" + self.path("tree/skip/*"))
        self.assertDmOk()
        self.assertEqual(1, self.hf_count("files"), "subtree contents excluded")

    def test_relative_exclude_longer_than_path_max(self):
        """A pattern longer than PATH_MAX is accepted, not rejected.

        A pattern is only ever matched against a path string, never passed to a
        syscall, so PATH_MAX has no business bounding it. It used to: the old
        add_exclude_pattern() resolved relative patterns against the cwd in a
        fixed buffer and bailed with "cannot prepend cwd to ...", so you could
        not exclude the deep subtrees #117 had just made scannable. Patterns are
        no longer cwd-resolved at all, but the length must still be fine.
        """
        deep = self.path("tree")
        comp = "c" * 200
        # A cwd long enough that cwd + pattern clears PATH_MAX, but short
        # enough that getcwd() itself still works.
        while len(deep) < 3000:
            deep = os.path.join(deep, comp)
        os.makedirs(os.path.join(deep, "drop"), exist_ok=True)
        os.makedirs(os.path.join(deep, "keep"), exist_ok=True)
        self.write(os.path.relpath(os.path.join(deep, "drop", "d.bin"),
                                   self.work), b"d" * 8192)
        self.write(os.path.relpath(os.path.join(deep, "keep", "k.bin"),
                                   self.work), b"k" * 8192)

        pattern = os.path.join("drop", "p" * 1400)  # expands past PATH_MAX
        cwd = os.getcwd()
        self.addCleanup(os.chdir, cwd)
        os.chdir(deep)
        self.dm("-r", "--exclude=" + pattern, ".")
        os.chdir(cwd)

        self.assertDmOk()
        self.assertNotIn("cannot prepend cwd", self.out)
        # The pattern does not match anything, so both files are still scanned;
        # the point is that oans accepted it instead of refusing to start.
        self.assertEqual(2, self.hf_count("files"))

    # --- gitignore semantics ---

    def test_bare_name_matches_at_any_depth(self):
        """The #147 case: `--exclude node_modules` used to match nothing.

        Patterns were fnmatch'd against the full path and relative ones were
        resolved against the cwd, so a bare name could only ever match one
        literal directory. Every NAS user's first instinct -- @eaDir,
        .snapshots, node_modules -- was a silent no-op.
        """
        self.mkrand("tree/keep.bin", 4000)
        self.mkrand("tree/node_modules/a.bin", 4000)
        self.mkrand("tree/deep/nested/node_modules/b.bin", 4000)
        self.scan(self.path("tree"), "--exclude=node_modules")
        self.assertDmOk()
        self.assertEqual(1, self.hf_count("files"),
                         "every node_modules at any depth is pruned")
        self.assertNotIn("matched nothing", self.out)

    def test_bare_name_matches_whole_components_only(self):
        self.mkrand("tree/node_modules/a.bin", 4000)
        self.mkrand("tree/node_modules_old/b.bin", 4000)
        self.mkrand("tree/xnode_modules/c.bin", 4000)
        self.scan(self.path("tree"), "--exclude=node_modules")
        self.assertDmOk()
        self.assertEqual(2, self.hf_count("files"),
                         "a substring of a component is not a match")

    def test_leading_slash_anchors_absolute(self):
        self.mkrand("tree/cache/a.bin", 4000)
        self.mkrand("tree/sub/cache/b.bin", 4000)
        self.scan(self.path("tree"), "--exclude=" + self.path("tree/cache"))
        self.assertDmOk()
        self.assertEqual(1, self.hf_count("files"),
                         "an absolute pattern anchors, so the nested cache stays")

    def test_interior_slash_matches_any_depth(self):
        self.mkrand("tree/keep.bin", 4000)
        self.mkrand("tree/Steam/temp/a.bin", 4000)
        self.mkrand("tree/deep/Steam/temp/b.bin", 4000)
        self.scan(self.path("tree"), "--exclude=Steam/temp")
        self.assertDmOk()
        self.assertEqual(1, self.hf_count("files"))

    def test_double_star_crosses_directories(self):
        self.mkrand("tree/a/t.bin", 4000)
        self.mkrand("tree/a/b/c/t.bin", 4000)
        self.mkrand("tree/a/keep.txt", 4000)
        self.scan(self.path("tree"), "--exclude=a/**/t.bin")
        self.assertDmOk()
        self.assertEqual(1, self.hf_count("files"), "only keep.txt survives")

    def test_single_star_does_not_cross_directories(self):
        self.mkrand("tree/a/x.bin", 4000)
        self.mkrand("tree/a/b/y.bin", 4000)
        self.scan(self.path("tree"), "--exclude=" + self.path("tree/a/*.bin"))
        self.assertDmOk()
        self.assertEqual(1, self.hf_count("files"),
                         "'*' stops at '/', so the nested file is kept")

    def test_character_class(self):
        for n in ("f1.log", "f2.log", "fx.log"):
            self.mkrand("tree/" + n, 4000)
        self.scan(self.path("tree"), "--exclude=f[0-9].log")
        self.assertDmOk()
        self.assertEqual(1, self.hf_count("files"), "only fx.log survives")

    def test_trailing_slash_matches_directories_only(self):
        self.mkrand("tree/cache/a.bin", 4000)      # a directory named cache
        self.write("tree/plain/cache", b"x" * 4000)  # a *file* named cache
        self.scan(self.path("tree"), "--exclude=cache/")
        self.assertDmOk()
        self.assertEqual(1, self.hf_count("files"),
                         "only the directory is pruned")
        self.assertEqual(
            1, self.hf_scalar(
                "select count(*) from files where filename like '%/plain/cache'"),
            "the file named cache is kept")

    def test_unmatched_pattern_warns(self):
        self.mkrand("tree/a.bin", 4000)
        self.scan(self.path("tree"), "--exclude=definitely-not-here")
        self.assertDmOk()
        self.assertEqual(1, self.hf_count("files"))
        self.assertIn('matched nothing', self.out,
                      "a pattern that matches nothing must say so (#147)")

    def test_malformed_pattern_is_rejected(self):
        self.mkrand("tree/a.bin", 4000)
        self.dm("-r", "--exclude=f[abc", self.path("tree"))
        self.assertNotEqual(0, self.rc, "an unterminated class is an error")
        self.assertIn("unterminated", self.out)
