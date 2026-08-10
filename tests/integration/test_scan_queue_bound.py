"""The scan's two work queues are bounded, and bounding them changes nothing (#208).

The walk runs ahead of the single __scan_file() consumer, and the consumer ahead
of the csum pool. Both queues used to be unbounded, and on a large tree that is
where the memory went - measured on 1.2M files, the walk queue peaked at 828k
items and the csum queue at 1.04M, together far more than seen_inodes and every
SQLite page cache put together.

What has to be pinned here is not the sizes (a suite-sized tree never reaches
them) but the two properties a bound could break: the hashfile must come out
identical, and a full queue must never wedge the scan. DUPEREMOVE_QUEUE_MAX
sets the cap; 0 restores the old unbounded behaviour, which is what the A/B
above was run against.
"""

import os

from harness import DuperemoveTest

KiB = 1 << 10


class ScanQueueBoundTest(DuperemoveTest):
    FILES = 400

    def build_tree(self):
        # Mixed sizes: the csum queue is ordered largest-first, so a bound that
        # mishandled the buckets would show up as a wrong or missing row.
        for i in range(self.FILES):
            self.write(f"tree/f{i:04d}.bin", os.urandom(4 * KiB + (i % 7) * KiB))
        self.sync()
        return self.path("tree")

    def fingerprints(self):
        return (self.files_fingerprint(), self.extents_fingerprint(),
                self.blocks_fingerprint())

    def _scan(self, tree, cap=None):
        env = {"DUPEREMOVE_QUEUE_MAX": str(cap)} if cap is not None else None
        self.dm("-r", tree, "-b", "4096", "--dedupe-options=partial", env=env)
        self.assertDmOk(f"scan with queue cap {cap}")
        return self.fingerprints()

    def test_a_tiny_cap_produces_the_same_hashfile(self):
        """A cap of 1 keeps both queues permanently full - the pathological
        case for the backpressure - and must still finish, with the same rows."""
        tree = self.build_tree()

        unbounded = self._scan(tree, cap=0)
        self.assertEqual(self.FILES, self.hf_count("files"))

        for cap in (1, 2, 8):
            self.drop_hashfile()
            self.assertEqual(unbounded, self._scan(tree, cap=cap),
                             f"queue cap {cap} changed the hashfile")

    def test_the_default_matches_the_unbounded_scan(self):
        tree = self.build_tree()

        default = self._scan(tree)
        self.drop_hashfile()
        self.assertEqual(default, self._scan(tree, cap=0))

    def test_one_worker_and_a_full_queue_still_completes(self):
        """A single csum worker with a cap of 1: every push waits for the one
        worker to take the previous file. If the wake-up were missed this hangs
        rather than fails, so the suite timeout is the real assertion."""
        tree = self.build_tree()

        self.dm("-r", "--io-threads=1", tree,
                env={"DUPEREMOVE_QUEUE_MAX": "1"})
        self.assertDmOk()
        self.assertEqual(self.FILES, self.hf_count("files"))
