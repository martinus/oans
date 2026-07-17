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
DUPEREMOVE = os.environ.get("DUPEREMOVE", os.path.join(REPO_DIR, "oans"))
TEST_ROOT = os.environ.get("DUPEREMOVE_TEST_DIR", os.path.join(REPO_DIR, ".itest-scratch"))

# oans exits 0 even after per-file failures, so we scan its output for
# these signatures rather than trusting the exit code.
_ERROR_RE = re.compile(
    r"Invalid argument|constraint failed|Database error|FAILURE|unable to|Error [0-9]",
    re.IGNORECASE,
)
_NET_CHANGE_RE = re.compile(r"net change in shared extents of:\s*(\d+)")


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


def fiemap_extents(path):
    """Return [(logical, physical, length, flags), ...] for path's data extents.

    Holes are not returned by FIEMAP, so every entry is real data. Uses
    FIEMAP_FLAG_SYNC so results are stable right after writes.
    """
    fd = os.open(path, os.O_RDONLY)
    try:
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
    finally:
        os.close(fd)


def phys_extents(path):
    """Set of physical start offsets of path's real (allocated) data extents."""
    return {phys for _log, phys, _len, flags in fiemap_extents(path)
            if not (flags & _NO_PHYS)}


def files_share(a, b):
    """True if a and b have at least one physical extent in common (reflinked)."""
    return bool(phys_extents(a) & phys_extents(b))


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
# Base test case
# --------------------------------------------------------------------------

class DuperemoveTest(unittest.TestCase):
    """Base class: a fresh scratch dir + hashfile per test, plus helpers."""

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

    def tearDown(self):
        shutil.rmtree(self.work, ignore_errors=True)

    # -- running oans ------------------------------------------------

    def dm(self, *args, hashfile=True, stdin=None, env=None):
        """Run oans; capture combined output in self.out and code in self.rc.

        Pass stdin=<str> to feed the process on standard input (e.g. a "-"
        file list), or env={...} to add environment variables for this run.
        """
        cmd = [DUPEREMOVE, "-q", "--io-threads=4"]
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
        """Fail if the last run printed any error signature."""
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

    def assertShared(self, a, b, msg=None):
        self.assertTrue(files_share(a, b),
                        msg or f"expected {a} and {b} to share storage")

    def assertNotShared(self, a, b, msg=None):
        self.assertFalse(files_share(a, b),
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
        p = self.write(relpath, data)
        os.truncate(p, size)
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
