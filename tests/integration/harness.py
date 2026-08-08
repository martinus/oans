"""Shared harness for the oans integration tests.

These tests drive the built ``oans`` binary against a scratch directory
tree and assert on the resulting hashfile (a SQLite database) and on actual
on-disk extent sharing. That is coverage the C unit tests cannot reach: the
scan/dedupe pipeline, incremental rescans, rename and hardlink handling,
excludes, sparse files, and end-to-end data integrity - including the two bugs
that motivated the suite (the batched-writer hardlink corruption and the
FIDEDUPERANGE EINVAL rejection), both of which only reproduce end to end.

Configuration via environment:
  DUPEREMOVE           path to the binary under test (default: repo ./oans)
  DUPEREMOVE_TEST_DIR  where scratch trees are created; must be on a reflink-
                       capable fs for the dedupe tests to run (default:
                       <repo>/.itest-scratch)
"""

import ctypes
import fcntl
import hashlib
import json
import os
import re
import shutil
import sqlite3
import struct
import subprocess
import tempfile
import unittest

# --------------------------------------------------------------------------
# Locations
# --------------------------------------------------------------------------

_HERE = os.path.dirname(os.path.abspath(__file__))
REPO_DIR = os.path.abspath(os.path.join(_HERE, "..", ".."))
# Resolved now, while the cwd is still the repo: `make integration` passes a
# relative DUPEREMOVE=./oans, and a test that chdir()s (to exercise a relative
# --exclude, say) would otherwise fail to find the binary.
DUPEREMOVE = os.path.abspath(
    os.environ.get("DUPEREMOVE", os.path.join(REPO_DIR, "oans")))
TEST_ROOT = os.environ.get("DUPEREMOVE_TEST_DIR", os.path.join(REPO_DIR, ".itest-scratch"))

# oans exits 0 even after per-file failures, so we scan its output for
# these signatures rather than trusting the exit code.
_ERROR_RE = re.compile(
    r"Invalid argument|constraint failed|Database error|FAILURE|unable to|Error [0-9]",
    re.IGNORECASE,
)
_NET_CHANGE_RE = re.compile(r"net change in shared extents of:\s*(\d+)")
# The human summary line, e.g. "  Reclaimed      1.0 MiB across 1 group".
# The size is human_size()-rendered ("1.0 MiB", "512 B"), so assert on the
# rendered string; for exact bytes use reclaimed_bytes() below.
_RECLAIMED_RE = re.compile(r"Reclaimed\s+([\d.]+ [A-Za-z]+)\s+across\s+(\d+)\s+group")
_ALREADY_SHARED_RE = re.compile(r"Already shared (\d+) file")


# --------------------------------------------------------------------------
# FIEMAP - read a file's physical extents to detect reflink sharing.
#
# We ask the kernel directly (same mechanism filefrag uses) instead of parsing
# a CLI: two files "share storage" when they report a common physical extent.
# --------------------------------------------------------------------------

def _ioc(direction, typ, nr, size):
    # Linux _IOC encoding: dir(2) | size(14) | type(8) | nr(8).
    return (direction << 30) | (size << 16) | (typ << 8) | nr


_FIEMAP_HDR = struct.Struct("<QQLLLL")   # start,length,flags,mapped,count,reserved
_FIEMAP_EXT = struct.Struct("<QQQQQLLLL")  # logical,physical,length,resv64[2],flags,resv[3]
_FS_IOC_FIEMAP = _ioc(3, ord("f"), 11, _FIEMAP_HDR.size)  # _IOWR('f', 11, struct fiemap)

_FIEMAP_FLAG_SYNC = 0x0001
# Extent flags whose physical offset is not real storage we can compare.
_FIEMAP_EXTENT_UNKNOWN = 0x0002
_FIEMAP_EXTENT_DELALLOC = 0x0004
_FIEMAP_EXTENT_DATA_INLINE = 0x0200
_NO_PHYS = _FIEMAP_EXTENT_UNKNOWN | _FIEMAP_EXTENT_DELALLOC | _FIEMAP_EXTENT_DATA_INLINE


def fiemap_extents_fd(fd):
    """Like fiemap_extents(), but on an already-open fd. Lets callers reach a
    file whose absolute path exceeds PATH_MAX (opened via a dir_fd), #117."""
    # extent_count=0 makes the kernel report only the total extent count
    # (it never fills more than fm_extent_count, so a single sized guess can
    # silently truncate). Count first, then fetch exactly that many.
    buf = bytearray(_FIEMAP_HDR.size)
    _FIEMAP_HDR.pack_into(buf, 0, 0, 0xFFFFFFFFFFFFFFFF,
                          _FIEMAP_FLAG_SYNC, 0, 0, 0)
    fcntl.ioctl(fd, _FS_IOC_FIEMAP, buf, True)
    count = _FIEMAP_HDR.unpack_from(buf, 0)[3]
    if count == 0:
        return []

    buf = bytearray(_FIEMAP_HDR.size + count * _FIEMAP_EXT.size)
    _FIEMAP_HDR.pack_into(buf, 0, 0, 0xFFFFFFFFFFFFFFFF,
                          _FIEMAP_FLAG_SYNC, 0, count, 0)
    fcntl.ioctl(fd, _FS_IOC_FIEMAP, buf, True)
    mapped = _FIEMAP_HDR.unpack_from(buf, 0)[3]

    out = []
    for i in range(mapped):
        logical, physical, length, _r0, _r1, flags, *_ = \
            _FIEMAP_EXT.unpack_from(buf, _FIEMAP_HDR.size + i * _FIEMAP_EXT.size)
        out.append((logical, physical, length, flags))
    return out


def fiemap_extents(path, dir_fd=None):
    """Return [(logical, physical, length, flags), ...] for path's data extents.

    Holes are not returned by FIEMAP, so every entry is real data. Uses
    FIEMAP_FLAG_SYNC so results are stable right after writes. Pass dir_fd to
    reach a file whose absolute path exceeds PATH_MAX (#117) -- the test process
    cannot name those either.
    """
    fd = os.open(path, os.O_RDONLY, dir_fd=dir_fd)
    try:
        return fiemap_extents_fd(fd)
    finally:
        os.close(fd)


def phys_extents(path, dir_fd=None):
    """Set of physical start offsets of path's real (allocated) data extents."""
    return {phys for _log, phys, _len, flags in fiemap_extents(path, dir_fd)
            if not (flags & _NO_PHYS)}


def files_share(a, b, dir_fd=None):
    """True if a and b have at least one physical extent in common (reflinked)."""
    return bool(phys_extents(a, dir_fd) & phys_extents(b, dir_fd))


def physical_footprint(directory):
    """Bytes of distinct physical storage the files under `directory` reference.

    The union of [physical, physical+length) over every regular file's real
    extents, so storage that several files share is counted once. Reducing this
    number is dedupe's entire job, which makes the drop across a run the ground
    truth to check a reported "Reclaimed" against - arrived at from the kernel's
    extent maps, independently of anything oans computes.

    Only meaningful for incompressible data: on a compressed filesystem
    fe_length is the *logical* length while fe_physical is a disk address, so
    the ranges are not real disk ranges. Callers write os.urandom(), which btrfs
    detects as incompressible and stores raw even under compress=zstd.
    """
    ranges = []
    for root, _dirs, names in os.walk(directory):
        for name in names:
            p = os.path.join(root, name)
            if os.path.islink(p) or not os.path.isfile(p):
                continue
            ranges.extend((phys, phys + length)
                          for _log, phys, length, flags in fiemap_extents(p)
                          if not (flags & _NO_PHYS))

    total = 0
    start = end = None
    for lo, hi in sorted(ranges):
        if end is not None and lo <= end:
            end = max(end, hi)
            continue
        if end is not None:
            total += end - start
        start, end = lo, hi
    return total + (end - start if end is not None else 0)


# --------------------------------------------------------------------------
# Reflink support probe
# --------------------------------------------------------------------------

def reflink_supported(directory=TEST_ROOT):
    os.makedirs(directory, exist_ok=True)
    a = os.path.join(directory, ".reflink_probe_a")
    b = os.path.join(directory, ".reflink_probe_b")
    try:
        with open(a, "wb") as f:
            f.write(b"probe")
        rc = subprocess.run(["cp", "--reflink=always", a, b],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode
        return rc == 0
    finally:
        for p in (a, b):
            try:
                os.unlink(p)
            except FileNotFoundError:
                pass


# Evaluated once; dedupe tests are decorated with @requires_reflink.
REFLINK = reflink_supported()
requires_reflink = unittest.skipUnless(
    REFLINK, "scratch filesystem has no reflink support (needed for dedupe)")


# --------------------------------------------------------------------------
# Filesystem type probe
# --------------------------------------------------------------------------

def scratch_fstype(directory=TEST_ROOT):
    """Filesystem type of the scratch dir, per `stat -f` (e.g. 'btrfs', 'xfs')."""
    os.makedirs(directory, exist_ok=True)
    return subprocess.run(["stat", "-f", "-c", "%T", directory],
                          capture_output=True, text=True).stdout.strip()


# btrfs is copy-on-write, so an in-place overwrite of alternate blocks splits a
# file into many physical extents. xfs (even with reflink) overwrites in place
# and stays one extent, so tests that need to *build* a fragmented file can only
# run on btrfs. (btrfs is reflink-capable, so this implies @requires_reflink.)
BTRFS = scratch_fstype() == "btrfs"
requires_btrfs = unittest.skipUnless(
    BTRFS, "test needs btrfs (copy-on-write fragmentation)")


def btrfs_ok(*args):
    """Run `btrfs <args>` quietly; True on success."""
    return subprocess.run(["btrfs", *args], stdout=subprocess.DEVNULL,
                          stderr=subprocess.DEVNULL).returncode == 0


# --------------------------------------------------------------------------
# Base test case
# --------------------------------------------------------------------------

_libc = ctypes.CDLL("libc.so.6", use_errno=True)


def _settle_scratch():
    """syncfs() the scratch filesystem so FIEMAP sees real extents.

    A test builds files and hands them straight to a scan that asserts on their
    extents. Under delayed allocation those extents may not exist yet when oans
    maps the file - it correctly records none, and the test reads the shortfall
    as a scanner bug. Running the suite in parallel pushes writeback far enough
    behind to lose this routinely: test_sparse_file_scans failed 4 runs in 12,
    and test_hardlink_pair_does_not_empty_hashfile saw 381 of 401 extents.

    Once per oans invocation, not once per file: fsync()ing each created file
    costs a journal commit apiece and took the suite from 5.4s to 18s, where one
    syncfs() of the whole scratch is a single syscall for the same guarantee.
    """
    fd = os.open(TEST_ROOT, os.O_RDONLY)
    try:
        _libc.syncfs(fd)
    finally:
        os.close(fd)


class DuperemoveTest(unittest.TestCase):
    """Base class: a fresh scratch dir + hashfile per test, plus helpers."""

    # Set True on a class whose assertions depend on the *physical* extent
    # layout the kernel happens to produce - the fsync-forced extent boundary
    # trick, or fiemap counts. Concurrent I/O perturbs btrfs writeback enough
    # that the layout the setup intends is not the one it gets, so tests/run.py
    # holds these back and runs them one at a time after the pool drains.
    # Per-test scratch isolation is *not* what this is for; that already works.
    serial = False

    @classmethod
    def setUpClass(cls):
        if not (os.path.isfile(DUPEREMOVE) and os.access(DUPEREMOVE, os.X_OK)):
            raise unittest.SkipTest(f"oans binary not found: {DUPEREMOVE}")
        os.makedirs(TEST_ROOT, exist_ok=True)

    def setUp(self):
        self.work = tempfile.mkdtemp(prefix="work.", dir=TEST_ROOT)
        self.hf = os.path.join(self.work, "hashfile.db")
        self.out = ""       # combined output of the last dm() call
        self.rc = 0         # its exit code
        self._subvols = []  # created via subvol()/snapshot(), see tearDown

    def tearDown(self):
        # Innermost first: a snapshot must go before the subvolume it lives in.
        for path in reversed(self._subvols):
            btrfs_ok("subvolume", "delete", path)
        shutil.rmtree(self.work, ignore_errors=True)

    # -- running oans ------------------------------------------------

    def dm(self, *args, hashfile=True, stdin=None, env=None, quiet=True):
        """Run oans; capture combined output in self.out and code in self.rc.

        Pass stdin=<str> to feed the process on standard input (e.g. a "-"
        file list), or env={...} to add environment variables for this run.
        Runs with -q by default (terse output); pass quiet=False to get the
        full human summary block (e.g. to assert on the 'Reclaimed' line).
        """
        _settle_scratch()   # the tree must be on disk before oans maps it
        cmd = [DUPEREMOVE, "--io-threads=4"]
        if quiet:
            cmd.insert(1, "-q")
        if hashfile:
            cmd += ["--hashfile", self.hf]
        cmd += list(args)
        run_env = None
        if env:
            run_env = dict(os.environ, **env)
        proc = subprocess.run(cmd, input=stdin, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, text=True, env=run_env)
        self.out = proc.stdout
        self.rc = proc.returncode
        return self.out

    def scan(self, path, *extra):
        return self.dm("-r", path, *extra)

    def dedupe(self, path, *extra):
        return self.dm("-rd", path, *extra)

    def assertDmOk(self, msg=None):
        """Fail if the last run exited non-zero or printed an error signature.

        The exit code matters as much as the output: a crash (SIGABRT from an
        abort_on(), SIGSEGV) still prints everything up to the point it died, so
        an output-only check reads a fatal run as a clean one and the test only
        fails later -- on some downstream assertion, with a misleading message
        -- or not at all.
        """
        if self.rc != 0:
            how = (f"killed by signal {-self.rc}" if self.rc < 0
                   else f"exit status {self.rc}")
            tail = "\n    ".join(self.out.splitlines()[-15:])
            self.fail((msg or "oans failed") + f" ({how}):\n    " + tail)

        hits = [ln for ln in self.out.splitlines() if _ERROR_RE.search(ln)]
        if hits:
            detail = "\n    ".join(hits)
            self.fail((msg or "oans reported errors") + ":\n    " + detail)

    def net_change(self):
        """Last reported net-change value as int, or None if not printed.

        oans omits the line entirely when there was nothing to dedupe.
        """
        vals = _NET_CHANGE_RE.findall(self.out)
        return int(vals[-1]) if vals else None

    def assertNoNewSharing(self):
        """The last dedupe added no sharing (explicit 0, or no line at all)."""
        nc = self.net_change()
        self.assertIn(nc, (None, 0), f"expected no new sharing, got {nc}")

    def reclaimed_summary(self):
        """`(size_str, groups)` from the human 'Reclaimed' summary line, or None.

        size_str is the rendered human figure (e.g. '1.0 MiB') — the honest
        disk-freed amount, one physical copy kept per group. Matches both the
        full Summary block and the -q one-liner (#148); returns None when there
        was nothing to dedupe. For exact byte assertions use reclaimed_bytes().
        """
        m = _RECLAIMED_RE.search(self.out)
        return (m.group(1), int(m.group(2))) if m else None

    def reclaimed_bytes(self):
        """Exact bytes freed, from the hashfile's --json metrics.

        The human summary rounds ('1.0 MiB'); this is the figure to assert on.
        Runs oans again, so it needs a hashfile the previous run wrote to.
        """
        return json.loads(self.dm("--json"))["reclaimed_total_bytes"]

    def assertReclaimed(self, size_str, groups, msg=None):
        """Assert the summary reported `size_str` reclaimed across `groups`."""
        self.assertEqual((size_str, groups), self.reclaimed_summary(), msg)

    def assertReclaimedNothing(self, msg=None):
        """The last run freed nothing: '0 B', or no Reclaimed line at all."""
        summary = self.reclaimed_summary()
        self.assertTrue(summary is None or summary[0] == "0 B",
                        (msg or "expected nothing to be reclaimed") +
                        f", got {summary}")

    def already_shared(self):
        """Count from the 'Already shared N files skipped' summary line.

        Zero when the line is absent, which is also what oans prints when no
        destination was skipped.
        """
        m = _ALREADY_SHARED_RE.search(self.out)
        return int(m.group(1)) if m else 0

    # -- hashfile (SQLite) inspection --------------------------------------

    def hf_query(self, sql, params=()):
        # NB: `with sqlite3.connect(...)` commits but does NOT close the
        # connection - a lingering reader would hold a WAL lock and block a
        # later oans run (e.g. its VACUUM). Close it explicitly.
        con = sqlite3.connect(self.hf)
        try:
            return con.execute(sql, params).fetchall()
        finally:
            con.close()

    def hf_exec(self, sql, params=()):
        """Modify the hashfile behind oans's back, to set up a state that only
        arises from an event a test cannot stage (a hash-library upgrade, a
        defragmentation between runs)."""
        con = sqlite3.connect(self.hf)
        try:
            con.execute(sql, params)
            con.commit()
        finally:
            con.close()

    def hf_count(self, table):
        return self.hf_query(f"select count(*) from {table}")[0][0]

    def hf_scalar(self, sql, params=()):
        rows = self.hf_query(sql, params)
        return rows[0][0] if rows else None

    def files_fingerprint(self):
        """Stable digest over the files table identity columns."""
        rows = self.hf_query(
            "select filename, ino, subvol, size, quote(digest), flags "
            "from files order by filename")
        return hashlib.sha256(repr(rows).encode()).hexdigest()

    def extents_fingerprint(self):
        rows = self.hf_query(
            "select f.filename, e.loff, e.len, quote(e.digest) from extents e "
            "join files f on f.id = e.fileid order by f.filename, e.loff")
        return hashlib.sha256(repr(rows).encode()).hexdigest()

    # -- on-disk sharing ---------------------------------------------------

    def assertShared(self, a, b, msg=None, dir_fd=None):
        self.assertTrue(files_share(a, b, dir_fd),
                        msg or f"expected {a} and {b} to share storage")

    def assertNotShared(self, a, b, msg=None, dir_fd=None):
        self.assertFalse(files_share(a, b, dir_fd),
                         msg or f"expected {a} and {b} to be independent")

    # -- data integrity ----------------------------------------------------

    def tree_digest(self, directory):
        """Digest over every regular file's relative path + contents."""
        h = hashlib.sha256()
        for root, _dirs, files in os.walk(directory):
            for name in sorted(files):
                p = os.path.join(root, name)
                if not os.path.isfile(p) or os.path.islink(p):
                    continue
                h.update(os.path.relpath(p, directory).encode())
                h.update(b"\0")
                with open(p, "rb") as f:
                    while chunk := f.read(1 << 20):
                        h.update(chunk)
        return h.hexdigest()

    # -- test-data builders ------------------------------------------------

    def path(self, *parts):
        """Absolute path under the scratch dir, creating parent directories."""
        p = os.path.join(self.work, *parts)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        return p

    def sync(self):
        """Flush to disk so FIEMAP / on-disk sharing checks see settled state."""
        subprocess.run(["sync"])

    def write(self, relpath, data):
        p = self.path(relpath)
        with open(p, "wb") as f:
            f.write(data)
        return p

    def mkrand(self, relpath, size):
        return self.write(relpath, os.urandom(size))

    def mkdup(self, rel_a, rel_b, size):
        """Two byte-identical but independently-stored files (dedupe fodder)."""
        data = os.urandom(size)
        a = self.write(rel_a, data)
        b = self.write(rel_b, data)   # plain write never reflinks
        return a, b

    def make_sparse(self, relpath, head, hole, tail):
        """Write head bytes, a real hole of `hole` bytes, then tail bytes.

        head/tail are bytes objects; pass the same ones to two calls to build
        identical, independently-stored sparse twins.
        """
        p = self.path(relpath)
        with open(p, "wb") as f:
            f.write(head)
            f.seek(len(head) + hole)
            f.write(tail)
        return p

    def make_trailing_hole(self, relpath, data, size):
        """Write `data`, then extend the file to `size` so it ends in a hole."""
        p = self.path(relpath)
        with open(p, "wb") as f:
            f.write(data)
            f.truncate(size)
        return p

    def reflink(self, src_rel, dst_rel):
        """Make dst a reflink (shared-extent) copy of src; returns dst path."""
        src, dst = self.path(src_rel), self.path(dst_rel)
        subprocess.run(["cp", "--reflink=always", src, dst], check=True)
        return dst

    def hardlink(self, rel_src, rel_dst):
        dst = self.path(rel_dst)
        os.link(self.path(rel_src), dst)
        return dst

    # -- btrfs subvolumes --------------------------------------------------
    #
    # Subvolumes cannot be removed with rmtree (a read-only one refuses even
    # to have its files unlinked), so anything created here is tracked and
    # deleted with the ioctl in tearDown, innermost first.

    def subvol(self, name):
        """Create a btrfs subvolume under the scratch dir; skips if it can't."""
        p = os.path.join(self.work, name)
        if not btrfs_ok("subvolume", "create", p):
            self.skipTest("cannot create a btrfs subvolume here")
        self._subvols.append(p)
        return p

    def snapshot(self, src, name, readonly=True):
        """Snapshot `src` to `name`; read-only by default."""
        args = ["subvolume", "snapshot"] + (["-r"] if readonly else [])
        dst = os.path.join(self.work, name)
        self.assertTrue(btrfs_ok(*args, src, dst),
                        f"could not snapshot {src} into {name}")
        self._subvols.append(dst)
        return dst

    def fragment(self, relpath, content):
        """Write content, then rewrite alternate 4K blocks in place.

        btrfs is copy-on-write, so the rewrites land elsewhere and the file
        ends up split across many physical extents. Needs btrfs; on xfs the
        overwrite happens in place and the file stays contiguous.
        """
        p = relpath if os.path.isabs(relpath) else self.path(relpath)
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

    def scanned_files(self):
        """Set of every path recorded in the hashfile."""
        return {r[0] for r in self.hf_query("select filename from files")}
