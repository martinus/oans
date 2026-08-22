"""#227: draining the dedupe pipeline must not land in the middle of a load.

--dedupe-options=partial has to reap every in-flight batch before the
block-hash search runs, because that search walks the GLOBAL filerec list. The
drain used to sit between the current batch's extent load and its push -- but a
batch takes its filerec refs only at push time, so reaping there freed a
filerec the just-loaded groups still pointed at, and push_results() walked into
it. The victim is a group that spans two generation windows: its anchor member
(the stable target, min file id) is loaded again by every later window, so the
same filerec is referenced by both the in-flight batch and the open one.

The tree here is built to force exactly that shape: one extent-level group
whose members are spread over many small generations, so consecutive windows
share the anchor. DUPEREMOVE_DEDUPE_DELAY_MS keeps the previous batch in flight
while the next one loads -- without it a test-sized batch finishes first and the
window never opens.

Requires btrfs: the setup relies on an fsync-forced extent boundary between the
unique head and the shared tail, the same reason test_extent_dedupe.py does.
"""

import os
from harness import DuperemoveTest, requires_btrfs

MiB = 1 << 20

# Small generations (-B) plus a small pass stride give many windows over few
# files, so batches overlap and an anchor is reloaded window after window.
BATCH_SIZE = "2"
FILES_PER_PASS = "2"
FILES = 12


@requires_btrfs
class PartialCrossWindowTest(DuperemoveTest):
    # The head/tail split relies on an fsync-forced extent boundary: see
    # DuperemoveTest.serial.
    serial = True

    def _mkfile(self, rel, head, tail):
        """A unique head, an fsync to force an extent boundary, shared tail."""
        p = self.path(rel)
        with open(p, "wb") as f:
            f.write(head)
            f.flush()
            os.fsync(f.fileno())
            f.write(tail)
        return p

    def test_a_cross_window_group_survives_the_partial_mode_drain(self):
        tail = os.urandom(2 * MiB)
        for i in range(FILES):
            self._mkfile(f"f{i:02d}.bin", os.urandom(2 * MiB), tail)
        self.sync()

        self.dedupe(self.work, "--dedupe-options=partial",
                    "-B", BATCH_SIZE, "--io-threads=2",
                    env={"DUPEREMOVE_FILES_PER_PASS": FILES_PER_PASS,
                         "DUPEREMOVE_DEDUPE_DELAY_MS": "200"})
        # A use-after-free shows up as a signal (SIGABRT from the invariant
        # assert, or SIGSEGV / an ASAN abort in a sanitizer build), which
        # assertDmOk reports as a failure rather than reading the partial
        # output as a clean run.
        self.assertDmOk("partial-mode dedupe across generation windows failed")

        # The run has to have actually deduped, or it proves nothing: an empty
        # extent pass never loads a cross-window anchor in the first place.
        for i in range(1, FILES):
            self.assertShared(self.path("f00.bin"), self.path(f"f{i:02d}.bin"),
                              "the shared tail was not deduped, so the "
                              "cross-window load under test never ran")
