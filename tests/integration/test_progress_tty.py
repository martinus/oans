"""The live progress block must not strand rows in the scrollback (#179).

The block owns the bottom of the screen and is redrawn in place by moving the
cursor up ``drawn_lines`` rows, so everything else has to be printed through
``progress_printf()``, which erases the block first. A plain ``printf`` moves
the cursor down without ``drawn_lines`` following, and the next redraw homes
*into* the block: the rows above the landing point are stranded on screen and
the message that caused it is erased.

Only reproducible on a tty, which is why the rest of the suite (pipes) cannot
see it. This runs oans under a real pty and replays the byte stream through a
small ANSI emulator to get the screen a user would actually see.
"""

import fcntl
import os
import pty
import re
import select
import struct
import termios
import unittest

from harness import DUPEREMOVE, DuperemoveTest, requires_reflink

COLS, ROWS = 100, 30

# A worker-slot row: two spaces, the slot number, two spaces, the status word.
WORKER_ROW = re.compile(r"^ {2}\d+ {2}(idle|hashing|mapping|commit|wait lock|deduping)\b")

_CSI = re.compile(rb"\x1b\[([0-9;?]*)([A-Za-z])")
_ANSI_TEXT = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]")


def _run_in_pty(argv, env=None):
    """Run argv on a COLS x ROWS pty; return its raw output bytes."""
    pid, fd = pty.fork()
    if pid == 0:                                  # child
        try:
            if env is None:
                os.execvp(argv[0], argv)
            else:
                os.execvpe(argv[0], argv, env)
        finally:                                  # execvp raises, never returns
            os._exit(127)
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", ROWS, COLS, 0, 0))
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


def _worker_rows(data, name):
    """Every worker row naming `name` that was ever drawn, in order.

    _render() rebuilds the *final* screen, and the block is wiped before exit -
    so a row that only existed mid-run has to be read out of the raw stream.
    """
    text = _ANSI_TEXT.sub("", data.decode("utf-8", "replace"))
    return [ln.strip() for ln in re.split(r"[\r\n]", text)
            if name in ln and "hashing" in ln]


def _render(data):
    """Replay an ANSI stream and return every line the user saw, scrollback first.

    Handles exactly what oans emits: CR, LF, cursor-up (``ESC[nA``),
    erase-below (``ESC[J``), erase-in-line (``ESC[K``) and line wrapping. Cursor
    show/hide and SGR colors are matched and ignored, as is any other CSI.
    """
    grid = [[" "] * COLS for _ in range(ROWS)]
    scrollback = []
    cy = cx = 0

    def newline():
        nonlocal cy
        cy += 1
        if cy >= ROWS:
            scrollback.append("".join(grid[0]).rstrip())
            grid.pop(0)
            grid.append([" "] * COLS)
            cy = ROWS - 1

    i, n = 0, len(data)
    while i < n:
        if data[i:i + 1] == b"\x1b":
            m = _CSI.match(data, i)
            if not m:
                i += 1
                continue
            params, cmd = m.group(1), m.group(2)
            arg = int(params) if params.isdigit() else 0
            if cmd == b"A":
                cy = max(0, cy - max(arg, 1))
            elif cmd in (b"J", b"K") and arg == 0:
                grid[cy][cx:] = [" "] * (COLS - cx)
                if cmd == b"J":
                    for y in range(cy + 1, ROWS):
                        grid[y] = [" "] * COLS
            i = m.end()
            continue

        ch = data[i:i + 1]
        if ch == b"\r":
            cx = 0
            i += 1
            continue
        if ch == b"\n":
            cx = 0
            newline()
            i += 1
            continue

        # One UTF-8 character (the block draws braille bar cells and ticks).
        b0 = data[i]
        width = 1 if b0 < 0x80 else 2 if b0 < 0xE0 else 3 if b0 < 0xF0 else 4
        try:
            char = data[i:i + width].decode()
        except UnicodeDecodeError:
            char = "?"
        # Wrap on write, not on reaching the edge: a line of exactly COLS
        # characters followed by a newline occupies one row, as on a real
        # terminal (the long routed error messages depend on this).
        if cx >= COLS:
            cx = 0
            newline()
        grid[cy][cx] = char
        cx += 1
        i += width

    return scrollback + ["".join(row).rstrip() for row in grid]


@requires_reflink
@unittest.skipIf(os.geteuid() == 0,
                 "running as root defeats the chmod 000 unreadable files")
class ProgressTtyTest(DuperemoveTest):
    def test_block_leaves_nothing_behind(self):
        """Nothing of the block survives the run, and no message is eaten.

        Two halves of one screen, so one run answers both. ``pdedupe_end()``
        wipes the block and leaves only the ticked stage line, so any surviving
        "  1  idle" is by definition a row a redraw homed past -- an invariant
        that holds for an unrouted print in *any* phase, not just the scan/dedupe
        gap that regressed. The skip report is the message that gap's desync
        erased.
        """
        # The two unreadable files are what makes the run print in the gap:
        # each fails open() in csum_whole_file and lands in the walk's
        # permission-denied bucket, which report_scan_skips() summarises there.
        for i in range(8):
            self.mkdup(f"tree/a{i}.bin", f"tree/b{i}.bin", 256 * 1024)
        for i in range(2):
            os.chmod(self.mkrand(f"tree/locked/{i}.bin", 64 * 1024), 0)
        tree = os.path.join(self.work, "tree")

        screen = _render(_run_in_pty(
            [DUPEREMOVE, "-dr", "--hashfile", self.hf, tree]))
        shown = "\n  ".join(screen)

        stranded = [ln for ln in screen if WORKER_ROW.match(ln)]
        self.assertEqual([], stranded,
                         f"worker rows left on screen:\n  {chr(10).join(stranded)}"
                         f"\n\nfull screen:\n  {shown}")
        self.assertTrue(
            any("2 permission denied" in ln for ln in screen),
            f"the scan-skip report was erased by the block:\n  {shown}")

    # Depends on hashing outlasting one REDRAW_MS (100 ms) tick, so the row
    # exists at a redraw. Kept CPU-bound (4K blocks, partial mode) rather than
    # sized in bytes: a faster disk shrinks an I/O-bound run but not this one.
    # If it ever goes quiet, raise SIZE rather than deleting the assertion.
    RESUME_SIZE = 256 * 1024 * 1024
    RESUME_CKPT = 64 * 1024 * 1024

    def test_resumed_file_shows_its_real_size_and_progress(self):
        """A resumed file must look resumed, not restarted (#159).

        Its row reports the whole file, with the bytes an earlier run already
        hashed credited up front. Reporting only the remainder is what this
        pins: the row then restarts near 0% *and* understates the file - an
        18.4 GiB movie resumed at 3 GiB drew as "0.2 GiB/15.4 GiB (1%)", which
        is indistinguishable from the resume having silently failed, and was
        reported as exactly that.

        The run-wide totals are a separate matter and deliberately do count
        only this run's work, so this asserts on the per-file row alone.
        """
        opts = ["-b", "4096", "--dedupe-options=partial", "--io-threads=1"]
        env = {"DUPEREMOVE_CHECKPOINT_BYTES": str(self.RESUME_CKPT)}
        self.mkrand("tree/movie.bin", self.RESUME_SIZE)
        self.sync()
        tree = os.path.join(self.work, "tree")

        # Stop after one checkpoint, as an interrupted run would.
        self.dm("-r", tree, *opts, env=dict(env, DUPEREMOVE_CHECKPOINT_STOP="1"))
        self.assertDmOk("interrupted scan")
        stopped_at = self.hf_scalar("select loff from scan_checkpoints")
        self.assertEqual(self.RESUME_CKPT, stopped_at, "unexpected resume point")

        rows = _worker_rows(_run_in_pty(
            [DUPEREMOVE, "-r", "--hashfile", self.hf, tree] + opts,
            env=dict(os.environ, **env)), "movie.bin")
        self.assertTrue(rows, "the resumed file never appeared in the block; "
                              "see RESUME_SIZE above")

        # 256.0 MiB is the file. 192.0 MiB - what is left after the
        # checkpoint - is the bug: the remainder drawn as if it were the whole.
        bad = [r for r in rows if "/256.0 MiB" not in r]
        self.assertEqual([], bad, "a resumed row reported something other than "
                                  "the file's real size:\n  " + "\n  ".join(bad))

        # ... and it picks up from the checkpoint rather than starting over.
        pct = re.search(r"\((\d+\.\d+)%\)", rows[0])
        self.assertTrue(pct, f"no percentage in {rows[0]!r}")
        self.assertGreaterEqual(
            float(pct.group(1)), 100.0 * stopped_at / self.RESUME_SIZE,
            f"the resumed row restarted below its checkpoint:\n  {rows[0]}")


if __name__ == "__main__":
    unittest.main()
