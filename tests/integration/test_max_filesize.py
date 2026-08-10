"""--max-filesize skips regular files above the threshold (the mirror of
--min-filesize); the default (no limit) skips nothing.

The bound is exclusive on both sides - a file *at* the limit is scanned, the
same way --min-filesize keeps a file at exactly the floor.
"""

import os

from harness import DuperemoveTest


class MaxFilesizeTest(DuperemoveTest):
    def _seed(self):
        self.write("tree/small", b"x" * 500)
        self.mkrand("tree/huge", 200000)
        return self.path("tree")

    def _scan_paths(self, *extra):
        """Scan the seeded tree and return the basenames it stored."""
        self.scan(self._seed(), *extra)
        self.assertDmOk()
        return sorted(os.path.basename(r[0])
                      for r in self.hf_query("select filename from files"))

    def test_default_has_no_upper_bound(self):
        self.assertEqual(["huge", "small"], self._scan_paths())

    def test_skips_above_threshold(self):
        self.assertEqual(["small"], self._scan_paths("--max-filesize", "1K"))

    def test_suffix_is_parsed(self):
        # 1M is above both files, so the option is a no-op - which is the point:
        # it proves the suffix parsed rather than landing on some tiny value.
        self.assertEqual(["huge", "small"],
                         self._scan_paths("--max-filesize", "1M"))

    def test_boundary_is_exclusive(self):
        # A file of exactly SIZE bytes is kept; one byte more is not.
        self.write("t/at", b"y" * 4096)
        self.write("t/over", b"y" * 4097)
        self.scan(self.path("t"), "--max-filesize", "4096")
        self.assertDmOk()
        self.assertEqual(
            ["at"], sorted(os.path.basename(r[0])
                           for r in self.hf_query("select filename from files")))

    def test_below_min_filesize_is_rejected(self):
        self.scan(self._seed(), "--max-filesize", "1K", "--min-filesize", "2K")
        self.assertNotEqual(self.rc, 0)
        self.assertIn("--max-filesize", self.out)
        self.assertIn("--min-filesize", self.out)

    def test_zero_is_rejected(self):
        self.scan(self._seed(), "--max-filesize", "0")
        self.assertNotEqual(self.rc, 0)
        self.assertIn("--max-filesize", self.out)

    def test_the_skip_is_accounted_for(self):
        # Its own bucket, reported like --min-filesize's: a config choice, not
        # an error, so it must not land in the problem count.
        self.dm("-r", "--max-filesize=1K", "-v", self._seed(), quiet=False)
        self.assertDmOk()
        self.assertIn("above --max-filesize", self.out)

    def test_replay_applies_the_stored_limit(self):
        # Stored like --min-filesize, so a bare replay honours it: the second
        # run must not pick the big file up.
        d = self._seed()
        self.dm("-r", "--max-filesize=1K", d)
        self.assertDmOk()
        self.assertEqual(1, self.hf_count("files"))

        self.mkrand("tree/huge2", 200000)
        self.dm()                                  # bare replay
        self.assertDmOk()
        self.assertEqual(1, self.hf_count("files"))
