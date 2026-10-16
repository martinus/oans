"""Partial mode must not carry a window's unmatched files into the next one.

The block-hash loader creates a filerec for every file with a duplicate block.
A file the search then matches to nothing ends up in no group, so no batch
takes a ref on it and reaping never frees it: it stayed on the global filerec
list for the rest of the run, and every later window's search walked it again,
one get_nondupe_extents query each. O(windows^2) queries, and a list that grew
with the tree.

Each file here repeats one block inside itself and shares nothing with any
other file: a duplicate block, but one the search skips (a file is not deduped
against itself by default). So every file is such a leftover, and many small
windows make the growth plain in the per-window debug line.
"""

import os
import re
from harness import DuperemoveTest, requires_reflink

KiB = 1 << 10
BLOCK = 128 * KiB
FILES = 12
FILES_PER_PASS = "2"

_SEARCHED_RE = re.compile(
    r"find_additional_dedupe: searched (\d+) of (\d+) filerecs")


@requires_reflink
class PartialStaleFilerecsTest(DuperemoveTest):

    def test_unmatched_files_do_not_pile_up_across_windows(self):
        for i in range(FILES):
            rep = os.urandom(BLOCK)
            self.write(f"f{i:02d}.bin",
                       os.urandom(BLOCK) + rep + os.urandom(BLOCK) + rep)
        self.sync()

        self.dm("-rd", self.work, "--dedupe-options=partial", "--debug",
                "-B", "1",
                env={"DUPEREMOVE_FILES_PER_PASS": FILES_PER_PASS})
        self.assertDmOk("partial-mode dedupe failed")

        windows = [(int(s), int(n)) for s, n in _SEARCHED_RE.findall(self.out)]
        # The run has to have searched window after window, or it proves
        # nothing: with one window there is no later search to walk leftovers.
        self.assertGreaterEqual(len(windows), FILES // 2,
                                f"expected a search per window: {windows}")

        bound = int(FILES_PER_PASS)
        for searched, total in windows:
            self.assertLessEqual(
                total, bound,
                f"a window's search saw {total} filerecs, more than the "
                f"{bound} files a window loads: earlier windows' unmatched "
                f"files were left on the list ({windows})")
            self.assertLessEqual(searched, bound, windows)
        self.assertLessEqual(sum(s for s, _ in windows), FILES, windows)
