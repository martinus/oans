"""The live progress block must not strand rows in the scrollback (#179).

The block is drawn at the bottom of the screen and redrawn in place by moving
the cursor up ``drawn_lines`` rows. Anything printed while it is on screen has
to be routed through ``pscan_printf()``, which erases the block first; a plain
``printf`` moves the cursor down without ``drawn_lines`` following, so the next
redraw homes *into* the block and leaves the rows it skipped on screen forever
-- and erases the message that caused it.

That only happens on a tty, which is why the rest of the suite (pipes) cannot
see it: these tests run oans under a real pty of a fixed size and replay the
byte stream through a small ANSI emulator to get the screen a user would see.

The window that regressed is between the scan and the dedupe phase: the scan
deliberately leaves its block up for the dedupe phase to keep drawing
(pscan_join(continues=True)) and no printer thread is alive across the gap, so
routing keyed on "is a printer running" took the raw branch there.
"""

import os
import pty
import re
import select
import struct
import sys
import termios
import fcntl
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import DUPEREMOVE, DuperemoveTest, requires_reflink  # noqa: E402

COLS, ROWS = 100, 30

# A worker-slot row: two spaces, the slot number, two spaces, the status word.
WORKER_ROW = re.compile(r"^ {2}\d+ {2}(idle|hashing|mapping|commit|wait lock|deduping)\b")

_CSI = re.compile(rb"\x1b\[([0-9;?]*)([A-Za-z])")


def _run_in_pty(argv, cols=COLS, rows=ROWS):
    """Run argv on a pty of the given size; return its raw output bytes."""
    pid, fd = pty.fork()
    if pid == 0:                                  # child
        os.execvp(argv[0], argv)
        os._exit(127)                             # unreachable on success
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
    chunks = []
    while True:
        if not select.select([fd], [], [], 60)[0]:
            break                                 # hung; let the assert report
        try:
            data = os.read(fd, 65536)
        except OSError:                           # EIO: the child closed the pty
            break
        if not data:
            break
        chunks.append(data)
    os.close(fd)
    os.waitpid(pid, 0)
    return b"".join(chunks)


def _render(data, cols=COLS, rows=ROWS):
    """Replay an ANSI stream and return every line the user saw, scrollback first.

    Handles just what the progress block emits: CR, LF, cursor up/down/left/right
    (ESC[nA..D), column set (ESC[nG), erase-below (ESC[J), erase-in-line (ESC[K)
    and autowrap. SGR colors and cursor show/hide are consumed and ignored.
    """
    grid = [[" "] * cols for _ in range(rows)]
    scrollback = []
    cy = cx = 0
    wrap_pending = False

    def newline():
        nonlocal cy, wrap_pending
        wrap_pending = False
        cy += 1
        if cy >= rows:
            scrollback.append("".join(grid[0]).rstrip())
            grid.pop(0)
            grid.append([" "] * cols)
            cy = rows - 1

    i, n = 0, len(data)
    while i < n:
        if data[i:i + 1] == b"\x1b":
            m = _CSI.match(data, i)
            if not m:
                i += 1
                continue
            nums = [int(x) for x in m.group(1).split(b";") if x.isdigit()]
            arg = nums[0] if nums else None
            cmd = m.group(2)
            if cmd == b"A":
                cy, wrap_pending = max(0, cy - (arg or 1)), False
            elif cmd == b"B":
                cy, wrap_pending = min(rows - 1, cy + (arg or 1)), False
            elif cmd == b"C":
                cx = min(cols - 1, cx + (arg or 1))
            elif cmd == b"D":
                cx = max(0, cx - (arg or 1))
            elif cmd == b"G":
                cx, wrap_pending = (arg or 1) - 1, False
            elif cmd == b"J" and (arg or 0) == 0:
                grid[cy][cx:] = [" "] * (cols - cx)
                for y in range(cy + 1, rows):
                    grid[y] = [" "] * cols
            elif cmd == b"K" and (arg or 0) == 0:
                grid[cy][cx:] = [" "] * (cols - cx)
            i = m.end()
            continue

        ch = data[i:i + 1]
        if ch == b"\r":
            cx, wrap_pending = 0, False
            i += 1
            continue
        if ch == b"\n":
            cx = 0
            newline()
            i += 1
            continue
        if ch == b"\b":
            cx, wrap_pending = max(0, cx - 1), False
            i += 1
            continue

        # One UTF-8 character (the block draws braille and box glyphs).
        b0 = data[i]
        width = 1 if b0 < 0x80 else 2 if b0 < 0xE0 else 3 if b0 < 0xF0 else 4
        try:
            char = data[i:i + width].decode()
        except UnicodeDecodeError:
            char = "?"
        if wrap_pending:
            cx = 0
            newline()
        grid[cy][cx] = char
        if cx + 1 >= cols:
            wrap_pending = True
        else:
            cx += 1
        i += width

    return scrollback + ["".join(row).rstrip() for row in grid]


@requires_reflink
@unittest.skipIf(os.geteuid() == 0,
                 "running as root defeats the chmod 000 unreadable files")
class ProgressTtyTest(DuperemoveTest):
    def build_tree(self):
        """A tree with duplicates to dedupe and two files the hasher cannot open.

        The unreadable pair is what makes the run print in the scan/dedupe gap:
        each one fails open() in csum_whole_file and lands in the walk's
        permission-denied skip bucket, which report_scan_skips() summarises
        there.
        """
        for i in range(8):
            self.mkdup(f"tree/a{i}.bin", f"tree/b{i}.bin", 256 * 1024)
        for i in range(2):
            path = self.mkrand(f"tree/locked/{i}.bin", 64 * 1024)
            os.chmod(path, 0)
        self.sync()
        return os.path.join(self.work, "tree")

    def dedupe_in_pty(self):
        tree = self.build_tree()
        raw = _run_in_pty([DUPEREMOVE, "-dr", "--hashfile", self.hf, tree])
        return _render(raw)

    def test_no_worker_rows_survive_the_run(self):
        """Once the run is over, every worker row must have been erased.

        pdedupe_end() wipes the block and leaves only the ticked stage line, so
        any surviving "  1  idle" is by definition a row a redraw homed past.
        """
        screen = self.dedupe_in_pty()
        stranded = [ln for ln in screen if WORKER_ROW.match(ln)]
        self.assertEqual([], stranded,
                         "worker rows left on screen:\n  " +
                         "\n  ".join(stranded) + "\n\nfull screen:\n  " +
                         "\n  ".join(screen))

    def test_scan_skip_report_survives_the_block(self):
        """The message printed in the gap must still be readable afterwards.

        The same desync ate it: the next redraw homed one row too low and its
        erase-below wiped the line that had just been printed below the block.
        """
        screen = self.dedupe_in_pty()
        self.assertTrue(
            any("2 permission denied" in ln for ln in screen),
            "the scan-skip report was erased by the progress block:\n  " +
            "\n  ".join(screen))


if __name__ == "__main__":
    unittest.main()
