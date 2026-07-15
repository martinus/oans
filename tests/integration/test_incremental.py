"""Incremental rescan: reusing a hashfile across runs."""

import os
from harness import DuperemoveTest


class IncrementalTest(DuperemoveTest):
    def test_rescan_unchanged_is_stable(self):
        self.mkrand("tree/a", 40000)
        self.mkdup("tree/x", "tree/y", 20000)
        self.scan(self.path("tree"))
        self.assertDmOk()
        fp, ep = self.files_fingerprint(), self.extents_fingerprint()
        self.scan(self.path("tree"))                 # second run, same hashfile
        self.assertDmOk()
        self.assertEqual(fp, self.files_fingerprint(), "unchanged rescan keeps files")
        self.assertEqual(ep, self.extents_fingerprint(), "unchanged rescan keeps extents")

    def test_rescan_picks_up_changed_content(self):
        self.mkrand("tree/a", 40000)
        self.scan(self.path("tree"))
        before = self.hf_scalar("select quote(digest) from files where filename like '%/a'")
        self.mkrand("tree/a", 40000)                 # rewrite with new content
        os.utime(self.path("tree/a"), (1893456000, 1893456000))  # bump mtime
        self.scan(self.path("tree"))
        self.assertDmOk()
        after = self.hf_scalar("select quote(digest) from files where filename like '%/a'")
        self.assertNotEqual(before, after, "changed content -> new digest")
        self.assertEqual(1, self.hf_count("files"), "still one row for the file")

    def test_rescan_adds_new_file(self):
        self.mkrand("tree/a", 10000)
        self.scan(self.path("tree"))
        self.assertEqual(1, self.hf_count("files"))
        self.mkrand("tree/b", 10000)
        self.scan(self.path("tree"))
        self.assertDmOk()
        self.assertEqual(2, self.hf_count("files"), "new file added on rescan")

    def test_rescan_retains_deleted_file_row(self):
        # duperemove's prune only removes NULL-digest rows (interrupted-scan
        # leftovers); a file deleted from disk keeps its digest and is simply
        # never revisited, so its row survives (use -R to drop it). This matches
        # the documented behavior and upstream; the test pins it so a change in
        # either direction is noticed.
        self.mkrand("tree/a", 10000)
        self.mkrand("tree/b", 10000)
        self.scan(self.path("tree"))
        self.assertEqual(2, self.hf_count("files"))
        os.remove(self.path("tree/b"))
        self.scan(self.path("tree"))
        self.assertDmOk()
        self.assertEqual(2, self.hf_count("files"), "deleted file's row retained")
        self.assertEqual(
            1, self.hf_scalar("select count(*) from files where filename like '%/b'"),
            "the deleted file's row is still present")

    def test_rescan_prunes_null_digest_rows(self):
        # The NULL-digest prune does fire: a stale half-scanned row is cleaned up.
        self.mkrand("tree/a", 10000)
        self.scan(self.path("tree"))
        ghost = self.path("tree/ghost")
        with __import__("sqlite3").connect(self.hf) as con:
            cols = [r[1] for r in con.execute("pragma table_info(files)")]
            # Newer schemas carry a NOT NULL path_hash; a real interrupted scan
            # sets it at insert time (only the digest is filled in later).
            extra_c = ", path_hash" if "path_hash" in cols else ""
            extra_v = ", 987654321" if "path_hash" in cols else ""
            con.execute(
                "insert into files (filename, ino, subvol, size, mtime, dedupe_seq, "
                f"digest, flags{extra_c}) values (?, 999999, 0, 123, 0, 0, NULL, 0{extra_v})",
                (ghost,))
        self.assertEqual(2, self.hf_count("files"), "stale row inserted")
        self.scan(self.path("tree"))                 # prune runs before scanning
        self.assertDmOk()
        self.assertEqual(
            0, self.hf_scalar("select count(*) from files where filename like '%/ghost'"),
            "NULL-digest stale row pruned")
