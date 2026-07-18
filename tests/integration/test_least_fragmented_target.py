"""Whole-file dedupe should target the least-fragmented copy, so the other
copies don't inherit a bad on-disk layout (and the dedupe stays cheap).

Requires btrfs: the setup builds a fragmented file by overwriting alternate
blocks in place, which only splits into many extents under copy-on-write.
"""

import os
from harness import DuperemoveTest, requires_btrfs, fiemap_extents

MiB = 1 << 20


@requires_btrfs
class LeastFragmentedTargetTest(DuperemoveTest):
    def _fragment(self, rel, content):
        """Write content, then rewrite alternate 4K blocks in place (COW) so the
        file ends up split across many physical extents."""
        p = self.path(rel)
        with open(p, "wb") as f:
            f.write(content)
            f.flush()
            os.fsync(f.fileno())
        fd = os.open(p, os.O_WRONLY)
        try:
            for off in range(0, len(content), 8192):
                os.pwrite(fd, content[off:off + 4096], off)
                os.fsync(fd)
        finally:
            os.close(fd)
        return p

    def test_dedupe_prefers_contiguous_target(self):
        content = os.urandom(8 * MiB)
        # Several fragmented copies plus one clean, contiguous copy.
        frags = [self._fragment(f"tree/frag{i}", content) for i in range(3)]
        contig = self.write("tree/contig", content)
        self.sync()

        self.assertGreater(len(fiemap_extents(frags[0])), 100, "setup: frag is fragmented")
        self.assertLessEqual(len(fiemap_extents(contig)), 2, "setup: contig is ~one extent")

        self.dedupe(self.path("tree"))
        self.assertDmOk()
        self.sync()

        # Every copy should now share the contiguous target's layout, not have
        # the fragmented layout spread onto it.
        self.assertShared(contig, frags[0])
        for p in frags + [contig]:
            self.assertLessEqual(
                len(fiemap_extents(p)), 4,
                f"{os.path.basename(p)} should be contiguous after dedupe")
