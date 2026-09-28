"""Regression for the FIDEDUPERANGE EINVAL bug (upstream issue #398).

The kernel rejects the whole ioctl with EINVAL when the source range extends
past the source file's end. That happens when a file's final shared extent is
recorded with fiemap's block-rounded length, which overshoots a file whose size
is not block-aligned. The fix clamps the request to the file's real size. These
need a reflink-capable filesystem.
"""

import os
import subprocess
from harness import (DuperemoveTest, requires_reflink, requires_btrfs,
                     phys_extents)


@requires_reflink
class EinvalTest(DuperemoveTest):
    # test_unaligned_shared_tail builds a *unique* head, a hole, then a *shared*
    # tail, so the tail is the only shareable region and has to land as its own
    # extent for the dedupe to have anything to do. That depends on btrfs
    # writeback placing the hole boundary where the setup intends, which
    # concurrent I/O from the rest of the suite perturbs -- the same reason
    # test_extent_order_independent and test_streaming_dedupe are held back.
    # Seen once on a CI runner (UBSAN leg, master @ e8e6a3d): assertShared
    # failed while the other seven legs passed on the same commit.
    serial = True

    def test_unaligned_shared_tail(self):
        # Unique aligned head, a hole, then an identical unaligned tail. The
        # tail is the shared extent whose block-rounded length overshoots EOF.
        head_a, head_b = os.urandom(262144), os.urandom(262144)
        tail = os.urandom(12345)
        a = self.make_sparse("tree/a", head_a, 65536, tail)
        b = self.make_sparse("tree/b", head_b, 65536, tail)
        self.sync()

        before = self.tree_digest(self.path("tree"))
        self.dedupe(self.path("tree"))
        self.assertDmOk()                            # specifically: no EINVAL
        self.assertNotIn("Invalid argument", self.out, "EINVAL must not appear")
        self.sync()
        self.assertShared(a, b, "the unaligned tail got shared")
        self.assertEqual(before, self.tree_digest(self.path("tree")),
                         "data preserved through clamp")

    def test_small_unaligned_whole_file(self):
        a, b = self.mkdup("tree/a", "tree/b", 196608 + 777)   # 192K + unaligned tail
        self.sync()
        before = self.tree_digest(self.path("tree"))
        self.dedupe(self.path("tree"))
        self.assertDmOk()
        self.assertNotIn("Invalid argument", self.out, "no EINVAL on unaligned whole file")
        self.sync()
        self.assertShared(a, b, "unaligned copies shared")
        self.assertEqual(before, self.tree_digest(self.path("tree")), "data preserved")

    # A sparse file with a *unique* head and a shared tail placed after a hole.


@requires_btrfs
class EinvalScopeTest(DuperemoveTest):
    """One EINVAL shortened every later request in the run (#280).

    A destination whose NODATASUM flag differs from the target's (a file
    copied out of a `chattr +C` directory) comes back EINVAL. oans then
    retries with whole blocks, for kernels that reject an unaligned length -
    but it kept that for the rest of the process, on every thread. btrfs
    shares a file's last partial block when both files end there, so every
    later pair kept its tail unshared, and the already-shared check, which
    compares whole blocks, called them done on every later run.
    """
    serial = True

    def test_an_einval_in_one_group_leaves_the_next_whole(self):
        big = os.urandom(1 << 20)       # the larger group is deduped first
        self.write("tree/a1", big)
        a2 = self.path("tree/a2")
        open(a2, "wb").close()
        if subprocess.run(["chattr", "+C", a2]).returncode:
            self.skipTest("chattr +C not supported here")
        with open(a2, "ab") as f:
            f.write(big)
        small = os.urandom(10000)
        b1 = self.write("tree/b1", small)
        b2 = self.write("tree/b2", small)
        self.sync()

        self.dm("-rdv", self.path("tree"), "--io-threads=1", quiet=False)
        self.assertEqual(0, self.rc, self.out)
        self.assertIn("Invalid argument", self.out, "setup: a2 was refused")
        self.sync()
        self.assertEqual(phys_extents(b1), phys_extents(b2),
                         "b2 shares all of b1, its last partial block too")

