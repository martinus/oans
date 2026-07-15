"""--min-filesize skips regular files below the threshold; the default (1) only
skips empty files."""

from harness import DuperemoveTest


class MinFilesizeTest(DuperemoveTest):
    def _scan_count(self, *extra):
        self.write("tree/tiny", b"x" * 500)
        self.mkrand("tree/big", 200000)
        self.write("tree/empty", b"")
        self.scan(self.path("tree"), *extra)
        self.assertDmOk()
        return self.hf_count("files")

    def test_default_skips_only_empty(self):
        # tiny + big recorded, empty skipped
        self.assertEqual(2, self._scan_count())

    def test_skips_below_threshold(self):
        # only big (200K) survives a 1K floor
        self.assertEqual(1, self._scan_count("--min-filesize", "1K"))

    def test_dash_m_alias(self):
        self.assertEqual(1, self._scan_count("-m", "1K"))
