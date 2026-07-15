"""Extent-level dedupe: files that are not whole-file duplicates but share a
region. This drives the extent pass end to end - including fiemap_scan_extent()
and fiemap_count_shared(), which the whole-file dedupe tests never reach - and
checks it shares the region, preserves data, and is idempotent.

(It cannot pin fiemap_scan_extent()'s exact post-dedupe offset: that value is
only a hint for the "already deduped" check, and the kernel byte-verifies every
dedupe, so a wrong hint changes neither data nor sharing. This guards the path
against crashes/errors and against sharing/data regressions.)

Requires a reflink-capable filesystem.
"""

import os
from harness import DuperemoveTest, requires_reflink

MiB = 1 << 20


@requires_reflink
class ExtentDedupeTest(DuperemoveTest):
    def _mkfile(self, rel, head, tail):
        """head, an fsync to force an extent boundary, then the shared tail."""
        p = self.path(rel)
        with open(p, "wb") as f:
            f.write(head)
            f.flush()
            os.fsync(f.fileno())
            f.write(tail)
        return p

    def test_shared_tail_is_deduped(self):
        # Distinct heads, identical tail -> not whole-file dupes, so only the
        # extent pass can share the tail.
        tail = os.urandom(4 * MiB)
        a = self._mkfile("tree/a", os.urandom(64 * 1024), tail)
        b = self._mkfile("tree/b", os.urandom(64 * 1024), tail)
        self.sync()

        self.assertNotShared(a, b, "independent before dedupe")
        before = self.tree_digest(self.path("tree"))

        self.dedupe(self.path("tree"))
        self.assertDmOk()
        self.sync()

        self.assertShared(a, b, "shared tail deduped via the extent pass")
        self.assertEqual(before, self.tree_digest(self.path("tree")),
                         "dedupe preserved file contents")

        # Extent dedupe is idempotent: a second pass adds no new sharing.
        self.dedupe(self.path("tree"))
        self.assertDmOk()
        self.assertNoNewSharing()
