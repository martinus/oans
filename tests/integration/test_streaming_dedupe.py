"""Streaming dedupe pipeline (Stage 2).

The dedupe phase runs one persistent thread pool while a bounded producer loads
the next generation batch as the current one dedupes (at most two batches in
flight). dedupe_seq advances in generation order as batches complete, so an
interrupt leaves only fully-processed generations marked. These tests drive many
groups across many generations (small DUPEREMOVE_FILES_PER_PASS), and check that
everything still converges, the durable watermark lands on the last generation,
a rerun is a no-op, and the in-memory (no-hashfile) path works too.
"""

import os
from harness import (DuperemoveTest, requires_reflink, phys_extents,
                     files_share, BTRFS)

MiB = 1 << 20


@requires_reflink
class StreamingDedupeTest(DuperemoveTest):
    # Extent-layout sensitive: see DuperemoveTest.serial.
    serial = True

    # tiny passes + small batchsize => work spans many generation batches, so
    # the producer/watermark and cross-batch anchor paths are all exercised.
    ENV = {"DUPEREMOVE_FILES_PER_PASS": "8"}
    BOPT = ("-B", "4")

    def _max_file_seq(self):
        return self.hf_scalar("select max(dedupe_seq) from files")

    def _config_seq(self):
        return self.hf_scalar(
            "select keyval from config where keyname='dedupe_sequence'")

    def test_many_groups_across_generations(self):
        pairs = []
        for i in range(40):
            d = os.urandom(20000 + i)  # distinct sizes => distinct groups
            a = self.write(f"tree/a{i:02d}", d)
            b = self.write(f"tree/b{i:02d}", d)
            pairs.append((a, b))
        self.sync()

        self.dm("-rd", *self.BOPT, self.path("tree"), env=self.ENV)
        self.assertDmOk()
        self.sync()

        for a, b in pairs:
            self.assertTrue(files_share(a, b),
                            f"{os.path.basename(a)} pair deduped")

        # The watermark advanced all the way to the last scanned generation.
        self.assertEqual(self._config_seq(), self._max_file_seq(),
                         "dedupe_seq watermark reached the final generation")

        # Rerun is a no-op.
        self.dm("-rd", *self.BOPT, self.path("tree"), env=self.ENV)
        self.assertDmOk()
        self.assertNoNewSharing()

    def test_cross_window_group_converges_to_one_extent(self):
        # 120 identical copies whose members land in many different generation
        # windows must still converge onto a single physical extent (the
        # cross-batch anchor path), not one cluster per batch.
        data = os.urandom(96 * 1024)
        paths = [self.write(f"tree/c{i:03d}", data) for i in range(120)]
        self.sync()

        self.dm("-rd", *self.BOPT, self.path("tree"), env=self.ENV)
        self.assertDmOk()
        self.sync()

        allphys = set()
        for p in paths:
            allphys |= phys_extents(p)
        self.assertEqual(len(allphys), 1,
                         f"all copies converge to one extent, got {len(allphys)}")
        self.assertEqual(self._config_seq(), self._max_file_seq())

    def test_whole_file_and_extent_passes_stream_together(self):
        # A whole-file triple plus an independent extent-only pair in the same
        # run: both passes feed the one pool without a drain between them.
        t = os.urandom(2 * MiB)
        wf = [self.write(f"tree/w{i}", t) for i in range(3)]
        # extent-only sharers: distinct heads, shared tail (btrfs boundary)
        tail = os.urandom(2 * MiB)
        ext = []
        for name in ("x", "y"):
            p = self.path(f"tree/{name}")
            with open(p, "wb") as f:
                f.write(os.urandom(64 * 1024))
                f.flush()
                os.fsync(f.fileno())
                f.write(tail)
            ext.append(p)
        self.sync()

        self.dm("-rd", *self.BOPT, self.path("tree"), env=self.ENV)
        self.assertDmOk()
        self.sync()

        self.assertShared(wf[0], wf[1], "whole-file triple deduped")
        self.assertShared(wf[1], wf[2], "whole-file triple deduped")
        # The shared-tail extent boundary only forms under btrfs copy-on-write.
        if BTRFS:
            self.assertShared(ext[0], ext[1], "extent tail deduped")

    def test_partial_mode_streams(self):
        # --dedupe-options=partial: the block-hash search walks the GLOBAL
        # filerec list, so the producer drains prior batches before running it
        # (partial mode gives up cross-batch pipelining). Many small windows
        # exercise that drain path; everything must still converge and a rerun
        # must be a no-op.
        pairs = []
        for i in range(24):
            d = os.urandom(200000 + i)
            a = self.write(f"tree/a{i:02d}", d)
            b = self.write(f"tree/b{i:02d}", d)
            pairs.append((a, b))
        self.sync()

        opts = ("-rd", "--dedupe-options=partial", *self.BOPT)
        self.dm(*opts, self.path("tree"), env=self.ENV)
        self.assertDmOk()
        self.sync()

        # partial was actually active: block hashes were stored
        self.assertGreater(
            self.hf_scalar("select count(*) from blocks"), 0,
            "block hashes stored under --dedupe-options=partial")
        for a, b in pairs:
            self.assertTrue(files_share(a, b),
                            f"{os.path.basename(a)} pair deduped")
        self.assertEqual(self._config_seq(), self._max_file_seq())

        self.dm(*opts, self.path("tree"), env=self.ENV)
        self.assertDmOk()
        self.assertNoNewSharing()

    def test_partial_search_waits_for_its_workers(self):
        """find_additional_dedupe() must not return while its pool still holds
        filerecs the producer is about to free (#123).

        The block-hash search runs on a persistent pool, and the producer reaps
        the previous batches right afterwards -- which frees their filerecs. The
        search used to rely on psearch_join() to wait, but that is a *progress*
        concern and no-ops during the dedupe phase, so nothing waited: a worker
        could still be dereferencing a filerec that free_batch() had freed. The
        window is microseconds wide, so it reproduced in only a few percent of
        runs (and only under valgrind/ASAN, since a plain build reads the freed
        memory silently).

        DUPEREMOVE_SEARCH_DELAY_MS holds every worker back so the producer wins
        the race every time, and free_batch() asserts the search is idle. On the
        unfixed code this aborts on the first reap; fixed, it just runs slower.
        """
        pairs = []
        for i in range(24):
            d = os.urandom(200000 + i)
            a = self.write(f"tree/a{i:02d}", d)
            b = self.write(f"tree/b{i:02d}", d)
            pairs.append((a, b))
        self.sync()

        env = dict(self.ENV, DUPEREMOVE_SEARCH_DELAY_MS="40")
        self.dm("-rd", "--dedupe-options=partial", *self.BOPT,
                self.path("tree"), env=env)
        self.assertDmOk()   # SIGABRT here => the search returned too early
        self.sync()

        # The run still has to do its job, not just avoid the abort.
        self.assertGreater(
            self.hf_scalar("select count(*) from blocks"), 0,
            "block hashes stored under --dedupe-options=partial")
        for a, b in pairs:
            self.assertTrue(files_share(a, b),
                            f"{os.path.basename(a)} pair deduped")

    def test_in_memory_dedupe_streams(self):
        # No --hashfile: the producer shares the in-memory handle and serializes
        # loads with the write lock. Dedupe must still happen on disk.
        pairs = []
        for i in range(12):
            d = os.urandom(30000 + i)
            a = self.write(f"tree/a{i}", d)
            b = self.write(f"tree/b{i}", d)
            pairs.append((a, b))
        self.sync()

        self.dm("-rd", *self.BOPT, self.path("tree"), env=self.ENV,
                hashfile=False)
        self.assertDmOk()
        self.sync()
        for a, b in pairs:
            self.assertTrue(files_share(a, b),
                            f"{os.path.basename(a)} deduped in-memory run")
