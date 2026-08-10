"""A dedupe group larger than one ioctl batch (upstream #392).

FIDEDUPERANGE takes at most MAX_DEDUPES_PER_IOCTL destinations, so a group with
more members than that is deduped in rounds: dedupe_extent_list() fills a batch,
runs it, closes every file, and reopens the target for the next round.

The member that next round starts with may be one that gets skipped - it cannot
be opened, it changed since the scan, or it already shares the target's extents.
Each of those paths runs the group's dedupe early only `if (ctxt && last)`, and
a round that has only just begun has no ctxt yet, so a skip there falls out of
the loop with the reopened target still on the open list. That tripped an
abort_on() and killed the whole run - not the group, the process - which is what
upstream #392 reports from a 17 TB backup tree.

Groups that big are the normal shape of a snapshot or backup tree, and a member
being skipped is routine, so this is worth a test even though it takes 121 files
to reach.
"""

import os
import unittest

from harness import DuperemoveTest, requires_reflink

# src/dedupe.h. A group of exactly this many *destinations* plus the target
# puts the last member at the start of round two, which is the case that used
# to abort: one more file and round two has a ctxt by the time it is reached.
MAX_DEDUPES_PER_IOCTL = 120

# Above btrfs max_inline, so every copy has a physical extent to dedupe.
SIZE = 64 << 10


@requires_reflink
class LargeGroupDedupeTest(DuperemoveTest):
    def _make_group(self, n):
        """n byte-identical files, independently written so none share yet."""
        data = os.urandom(SIZE)
        for i in range(n):
            self.write(f"tree/f{i:03d}.bin", data)
        self.sync()

    def _last_group_member(self):
        """The path of the member dedupe_extent_list() visits last.

        GET_DUPLICATE_FILES orders a group `is_target desc, f.id`, and the
        target is elected by fewest extents then lowest id - so with uniform
        files the last member is simply the highest id. Reading it back beats
        assuming a name, because ids follow the order files finish hashing.
        """
        return self.hf_scalar(
            "select filename from files order by id desc limit 1")

    @unittest.skipIf(os.geteuid() == 0,
                     "chmod 000 does not stop root from opening the file, so "
                     "the member under test would not be skipped")
    def test_a_group_spanning_two_batches_survives_a_skipped_last_member(self):
        self._make_group(MAX_DEDUPES_PER_IOCTL + 1)

        # Scan while everything is readable, so all 121 land in one group.
        self.dm("-r", "--io-threads=1", self.work)
        self.assertDmOk("scan")

        # Make exactly that member unopenable. Its row survives: pruning is
        # stat-based and the file is still there, so the dedupe phase still
        # loads it and only then finds it cannot be opened.
        victim = self._last_group_member()
        os.chmod(victim, 0)

        self.dm("-dr", "--io-threads=1", self.work, quiet=False)
        os.chmod(victim, 0o644)

        # The exit code is the assertion. assertDmOk() would also reject the
        # "Permission denied" line, which is the expected, reported outcome for
        # the member we made unreadable - what must not happen is the process
        # dying, and it printed a whole run's worth of output before it did, so
        # only the status distinguishes the two.
        self.assertEqual(0, self.rc,
                         "dedupe of a group spanning two ioctl batches died: "
                         + (f"signal {-self.rc}" if self.rc < 0
                            else f"exit {self.rc}"))

        # And it deduped rather than bailing out at the batch boundary: every
        # member but the target and the unreadable one is freed.
        self.assertEqual((MAX_DEDUPES_PER_IOCTL - 1) * SIZE,
                         self.reclaimed_bytes(),
                         "round one's members were not all deduped")
