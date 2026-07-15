"""Dedupe phase: identical files share storage, data is preserved, second run
is a no-op. Requires a reflink-capable filesystem."""

import os
from harness import DuperemoveTest, requires_reflink

MiB = 1 << 20


@requires_reflink
class DedupeTest(DuperemoveTest):
    def test_shares_identical_files(self):
        a, b = self.mkdup("tree/a", "tree/b", MiB)
        self.sync()
        self.assertNotShared(a, b, "not shared before dedupe")

        before = self.tree_digest(self.path("tree"))
        self.dedupe(self.path("tree"))
        self.assertDmOk()
        self.sync()
        self.assertShared(a, b, "shared after dedupe")
        self.assertEqual(before, self.tree_digest(self.path("tree")),
                         "dedupe preserved file contents")

    def test_reports_net_change(self):
        self.mkdup("tree/a", "tree/b", MiB)
        self.sync()
        self.dedupe(self.path("tree"))
        self.assertDmOk()
        # Both copies become shared: net change counts the shared bytes on each.
        self.assertEqual(2 * MiB, self.net_change(), "net shared change for a 1 MiB pair")

    def test_is_idempotent(self):
        a, b = self.mkdup("tree/a", "tree/b", MiB)
        self.sync()
        self.dedupe(self.path("tree"))
        self.assertDmOk()
        self.sync()
        self.dedupe(self.path("tree"))               # second pass: nothing new
        self.assertDmOk()
        self.assertNoNewSharing()
        self.assertShared(a, b, "still shared after second run")

    def test_in_memory_hashfile_dedupes_without_lock_errors(self):
        # With no --hashfile duperemove uses an in-memory shared-cache db that
        # the listing reader, the batched writer and the csum workers all open
        # as separate connections. Shared-cache does table-level locking between
        # connections, so without read_uncommitted every write failed with
        # "Database error 6 ... database table is locked" and nothing was stored.
        a, b = self.mkdup("tree/a", "tree/b", MiB)
        self.sync()
        self.dm("-rd", self.path("tree"), hashfile=False)
        self.assertDmOk()
        self.sync()
        self.assertShared(a, b, "in-memory dedupe shared the pair")

    def test_leaves_distinct_files_alone(self):
        a = self.mkrand("tree/a", MiB)
        b = self.mkrand("tree/b", MiB)
        self.sync()
        self.dedupe(self.path("tree"))
        self.assertDmOk()
        self.sync()
        self.assertNotShared(a, b, "distinct files stay independent")

    def test_multiway_group(self):
        a = self.mkrand("tree/a", MiB)
        data = open(a, "rb").read()
        b = self.write("tree/b", data)
        c = self.write("tree/c", data)
        self.sync()
        before = self.tree_digest(self.path("tree"))
        self.dedupe(self.path("tree"))
        self.assertDmOk()
        self.sync()
        self.assertShared(a, b, "a<->b shared")
        self.assertShared(a, c, "a<->c shared")
        self.assertEqual(before, self.tree_digest(self.path("tree")), "contents preserved")
