"""Dedupe passes merge many scan generations (see DEDUPE_FILES_PER_PASS).

A tiny --batchsize seals a dedupe generation every few files; the pass loop
must still dedupe every group exactly once - including pairs whose members
land in different generations - and a rerun must net zero. Requires a
reflink-capable filesystem.
"""

import os
from harness import DuperemoveTest, requires_reflink


@requires_reflink
class PassStrideTest(DuperemoveTest):
    def test_pairs_across_generations(self):
        # 30 pairs; names interleave so a pair's two members usually end up
        # in different 4-file scan batches (= different generations).
        pairs = []
        for i in range(30):
            data = os.urandom(8192)
            a = self.write(f"tree/a{i:02}", data)
            b = self.write(f"tree/z{i:02}", data)
            pairs.append((a, b))
        self.sync()

        self.dedupe(self.path("tree"), "-B", "4")
        self.assertDmOk()
        self.sync()

        for a, b in pairs:
            self.assertShared(a, b, f"{os.path.basename(a)} pair deduped")

        # a rerun over the same (fully deduped) tree must be a no-op
        self.dedupe(self.path("tree"), "-B", "4")
        self.assertDmOk()
        self.assertEqual(self.net_change() or 0, 0, "no-op rerun nets zero")
