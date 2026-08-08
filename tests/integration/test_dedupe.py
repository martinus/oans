"""Dedupe phase: identical files share storage, data is preserved, second run
is a no-op. Requires a reflink-capable filesystem."""

import os
from harness import DuperemoveTest, phys_extents, requires_reflink

MiB = 1 << 20


@requires_reflink
class DedupeTest(DuperemoveTest):
    # test_reclaimed_excludes_what_was_already_shared turns on the physical
    # layout a reflink-plus-rewrite leaves behind; see DuperemoveTest.serial.
    serial = True

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

    def test_reports_honest_reclaimed(self):
        # The human summary reports the disk actually freed: deduping a 1 MiB
        # pair keeps one copy and frees the other, so 1.0 MiB across 1 group —
        # not the 2 MiB fiemap "net change in shared extents".
        self.mkdup("tree/a", "tree/b", MiB)
        self.sync()
        self.dm("-rd", self.path("tree"), quiet=False)  # full Summary block
        self.assertDmOk()
        self.assertReclaimed("1.0 MiB", 1)

    def test_reclaimed_excludes_what_was_already_shared(self):
        """Half the copy already sits on the target's extents: half is freed.

        FIDEDUPERANGE reports the whole compared length as deduped whether or
        not the range already shared that storage, so crediting the kernel's
        figure counted the shared half as freed too - and a run that freed
        nothing could still claim gigabytes (#187).

        Built by reflinking and then rewriting the second half, so exactly
        1 MiB of the 2 MiB copy is duplicated. Layout-sensitive, hence serial;
        the precondition is checked rather than assumed.
        """
        data = os.urandom(2 * MiB)
        a = self.write("tree/a", data)
        self.sync()
        b = self.reflink("tree/a", "tree/b")
        with open(b, "r+b") as f:
            f.seek(MiB)
            f.write(data[MiB:])
        self.sync()

        pa, pb = phys_extents(a), phys_extents(b)
        if not (pa & pb) or not (pb - pa):
            self.skipTest(f"no half-shared layout here: a={sorted(pa)} "
                          f"b={sorted(pb)}")

        # a is the least fragmented, so pick_dedupe_target() makes it the
        # target and b the destination whose unshared half gets counted.
        self.dedupe(self.path("tree"))
        self.assertDmOk()
        self.assertEqual(MiB, self.reclaimed_bytes(),
                         "only the half that was still duplicated was freed")

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
