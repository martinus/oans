"""Exclude patterns: literal paths and globs are skipped during listing."""

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
        """A relative --exclude whose cwd-expansion exceeds PATH_MAX works.

        An exclude pattern is only ever matched with fnmatch()/strcmp(); it is
        never passed to a syscall, so PATH_MAX has no business bounding it. It
        used to: add_exclude_pattern() built the expansion in a fixed buffer and
        bailed with "cannot prepend cwd to ...", which meant you could not
        exclude the deep subtrees #117 had just made scannable -- and excluding
        one is the natural workaround for the remaining over-PATH_MAX root
        limit.
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
