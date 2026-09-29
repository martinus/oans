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
import tempfile
import shutil
import sqlite3
import unittest

from harness import DUPEREMOVE, DuperemoveTest, requires_reflink

COLS, ROWS = 100, 30

# A worker-slot row: two spaces, the slot number, two spaces, the status word.
WORKER_ROW = re.compile(r"^ {2}\d+ {2}(idle|hashing|mapping|commit|wait lock|deduping)\b")

_CSI = re.compile(rb"\x1b\[([0-9;?]*)([A-Za-z])")
_ANSI_TEXT = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]")


def _run_in_pty(argv, env=None, stderr_path=None, cols=COLS, rows=ROWS):
    """Run argv on a COLS x ROWS pty; return its raw output bytes.

    With `stderr_path`, the child's stderr is redirected to that file while
    stdout stays on the pty - the `2>errors.log` case of #203.
    """
    pid, fd = pty.fork()
    if pid == 0:                                  # child
        try:
            if stderr_path is not None:
                efd = os.open(stderr_path,
                              os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o644)
                os.dup2(efd, 2)
                os.close(efd)
            os.execvpe(argv[0], argv, os.environ if env is None else env)
        finally:                                  # execvp raises, never returns
            os._exit(127)
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


def _worker_rows(data, name, status="hashing"):
    """Every worker row naming `name` drawn in `status`, in order.

    _render() rebuilds the *final* screen, and the block is wiped before exit -
    so a row that only existed mid-run has to be read out of the raw stream.

    Filtering on the status matters: the same slot also renders the file while
    mapping and committing, and those rows carry "(size: N)" instead of the
    done/total the callers here are asking about.
    """
    text = _ANSI_TEXT.sub("", data.decode("utf-8", "replace"))
    rows = []
    for ln in re.split(r"[\r\n]", text):
        m = WORKER_ROW.match(ln)
        if m and m.group(1) == status and name in ln:
            rows.append(ln.strip())
    return rows


def _render(data, cols=COLS, rows=ROWS):
    """Replay an ANSI stream and return every line the user saw, scrollback first.

    Handles exactly what oans emits: CR, LF, cursor-up (``ESC[nA``),
    erase-below (``ESC[J``), erase-in-line (``ESC[K``) and line wrapping. Cursor
    show/hide and SGR colors are matched and ignored, as is any other CSI.
    """
    COLS, ROWS = cols, rows
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

    def test_an_interrupted_scan_leaves_nothing_behind_either(self):
        """The same invariant, on the path that has no dedupe phase.

        The scan hands its block straight to the dedupe phase rather than
        wiping it (pscan_join(continues=true)), so whoever decides there is no
        such phase also owns wiping the block. An interrupted run stops before
        pdedupe_begin(), so if it still claimed the hand-off the worker rows
        would sit on screen for the rest of the session - #179 exactly, reached
        from the other side.
        """
        for i in range(8):
            self.mkdup(f"tree/a{i}.bin", f"tree/b{i}.bin", 256 * 1024)
        tree = os.path.join(self.work, "tree")

        # dict(os.environ, ...) rather than a bare dict: _run_in_pty() passes
        # this straight to execvpe(), which *replaces* the environment rather
        # than adding to it. A bare dict therefore drops TSAN_OPTIONS, so this
        # one process ran with no suppressions file while the other 248 had
        # one - and reported the GLib queue-node race the file exists to
        # filter, as a stranded worker row. See #236.
        screen = _render(_run_in_pty(
            [DUPEREMOVE, "-dr", "--hashfile", self.hf, tree],
            env=dict(os.environ, DUPEREMOVE_INTERRUPT_AFTER="4")))
        shown = "\n  ".join(screen)

        stranded = [ln for ln in screen if WORKER_ROW.match(ln)]
        self.assertEqual([], stranded,
                         f"worker rows left on screen after an interrupt:"
                         f"\n  {chr(10).join(stranded)}\n\nfull screen:\n  {shown}")

    def test_a_crafted_name_cannot_inject_ansi_into_the_block(self):
        """The worker rows are drawn straight to the tty (#202).

        The block's own ANSI is expected; what must never appear is a sequence
        that came out of a file name. ESC[2J would clear the user's screen.
        """
        d = os.path.join(self.work, "tree")
        os.makedirs(d, exist_ok=True)
        for side in (b"a-", b"b-"):
            with open(os.path.join(d.encode(),
                                   side + b"esc\x1b[2J\x1b[31mred.bin"),
                      "wb") as f:
                f.write(b"z" * (4 * 1024 * 1024))

        raw = _run_in_pty([DUPEREMOVE, "-dvr", "--hashfile", self.hf, d])
        self.assertNotIn(b"\x1b[2J", raw, "a file name's ESC[2J reached the tty")
        self.assertIn(b"esc\\x1b[2J", raw, "the name was not printed at all")
    def test_routed_errors_follow_the_redirected_stderr(self):
        """`2>errors.log` must catch errors raised while a block is live (#203).

        An unreadable file fails open() inside a csum worker, so its eprintf is
        raised mid-hash with the block on screen - precisely the messages that
        used to be forced onto stdout and vanish from the redirect.
        """
        for i in range(8):
            self.mkdup(f"tree/a{i}.bin", f"tree/b{i}.bin", 256 * 1024)
        for i in range(2):
            os.chmod(self.mkrand(f"tree/locked/{i}.bin", 64 * 1024), 0)
        tree = os.path.join(self.work, "tree")
        errlog = os.path.join(self.work, "errors.log")

        raw = _run_in_pty([DUPEREMOVE, "-dr", "--hashfile", self.hf, tree],
                          stderr_path=errlog)
        with open(errlog, encoding="utf-8", errors="replace") as f:
            errors = f.read()
        screen = "\n  ".join(_render(raw))

        for i in range(2):
            self.assertIn(f"{i}.bin", errors,
                          f"the open error never reached stderr:\n{errors}")
        self.assertIn("while opening file", errors)
        self.assertNotIn("while opening file", screen,
                         f"a redirected error was still printed to the tty:"
                         f"\n  {screen}")

        # The block still cleans up around the routed message, and stdout keeps
        # the report that is genuinely stdout's (the #179 invariant).
        stranded = [ln for ln in _render(raw) if WORKER_ROW.match(ln)]
        self.assertEqual([], stranded,
                         f"worker rows left on screen:\n  {screen}")
        self.assertIn("2 permission denied", screen)

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

        # The printer redraws every ~100 ms, and a fast runner can hash the
        # remaining 192 MiB in about that: then no frame shows the row at all
        # (seen twice on the xfs/clang leg). What this pins is what the row
        # says when it is drawn, so a run that drew none is repeated, from
        # the same checkpoint.
        con = sqlite3.connect(self.hf)       # all of it in the main file
        con.execute("pragma wal_checkpoint(truncate)")
        con.close()
        with open(self.hf, "rb") as f:
            snapshot = f.read()
        for _ in range(5):
            for side in ("-wal", "-shm"):
                if os.path.exists(self.hf + side):
                    os.unlink(self.hf + side)
            with open(self.hf, "wb") as f:
                f.write(snapshot)
            rows = _worker_rows(_run_in_pty(
                [DUPEREMOVE, "-r", "--hashfile", self.hf, tree] + opts,
                env=dict(os.environ, **env)), "movie.bin")
            if rows:
                break
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


    def test_a_failed_scan_gives_the_cursor_back(self):
        """#286: the scan hides the cursor and leaves its block for the dedupe
        phase. A scan that fails - here on a filesystem that cannot dedupe -
        never reaches that phase, and the shell was left with no cursor and
        the block still on screen."""
        other = "/dev/shm"
        if not os.path.isdir(other) or \
           os.stat(other).st_dev == os.stat(self.work).st_dev:
            self.skipTest("no second filesystem at /dev/shm")
        tree = tempfile.mkdtemp(prefix="oans-tty.", dir=other)
        self.addCleanup(shutil.rmtree, tree, True)
        for i in range(4):
            with open(os.path.join(tree, f"f{i}"), "wb") as f:
                f.write(os.urandom(64 * 1024))

        data = _run_in_pty([DUPEREMOVE, "-dr", "--hashfile", self.hf, tree])
        self.assertGreater(data.rfind(b"\x1b[?25h"), data.rfind(b"\x1b[?25l"),
                           "the cursor was left hidden")
        stranded = [ln for ln in _render(data) if WORKER_ROW.match(ln)]
        self.assertEqual([], stranded)

    def test_a_narrow_short_terminal_strands_nothing(self):
        """#286: only the worker rows were fitted to the width. The dedupe
        detail line runs past 80 columns between batches, and a block taller
        than the terminal scrolls; either way the redraw homes too few rows
        and leaves a stale one behind each time."""
        for i in range(8):
            self.mkdup(f"tree/a{i}.bin", f"tree/b{i}.bin", 256 * 1024)
        tree = os.path.join(self.work, "tree")
        env = dict(os.environ, DUPEREMOVE_FILES_PER_PASS="2",
                   DUPEREMOVE_DEDUPE_DELAY_MS="50")
        # Narrow and tall, where only the width can go wrong (the bar line
        # alone is ~66 columns); then short, where the height can.
        for cols, rows in ((50, 40), (60, 10)):
            self.drop_hashfile()
            data = _run_in_pty([DUPEREMOVE, "-dr", "--io-threads=8",
                                "--hashfile", self.hf, tree], env=env,
                               cols=cols, rows=rows)
            screen = _render(data, cols=cols, rows=rows)
            stranded = [ln for ln in screen if WORKER_ROW.match(ln)]
            self.assertEqual([], stranded,
                             f"{cols}x{rows}:\n" + "\n".join(screen))


class ProgressPipeTest(DuperemoveTest):
    def test_a_message_into_a_file_is_not_followed_by_the_block(self):
        """#286: every routed message redrew the whole block, even with no
        terminal to draw it on: 163 lines for a 30-file -v run."""
        for i in range(30):
            self.mkrand(f"tree/f{i}.tmp", 8000)
        self.mkrand("tree/keep", 8000)
        out = self.dm("-rv", "--exclude", "*.tmp", self.path("tree"),
                      quiet=False)
        self.assertEqual(0, self.rc, out)
        stage_lines = [ln for ln in out.splitlines()
                       if re.match(r"^\W*scanning\b", ln)]
        self.assertLess(len(stage_lines), 5, out)

    def test_the_search_bar_stays_off_a_pipe(self):
        """#286: the partial-mode search bar is redrawn with '\\r', and it was
        written into pipes, and under -q."""
        for i in range(4):
            self.mkdup(f"tree/a{i}", f"tree/b{i}", 64 * 1024)
        out = self.dm("-r", "--dedupe-options=partial", self.path("tree"),
                      text=False)
        self.assertEqual(0, self.rc, out)
        self.assertNotIn(b"\r[", out)


if __name__ == "__main__":
    unittest.main()
