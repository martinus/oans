#!/usr/bin/env python3
"""Fill demo files with pseudo-random content.

Reads a job list — one ``<bytes>\\t<path>`` line per file — from the file named
by ``argv[1]`` and writes each path with that many pseudo-random bytes.

Uses Python's ``random`` (Mersenne Twister): not cryptographic, but plenty for a
demo, where the bytes only need to be distinct per file and incompressible (so
the data really lands on disk and takes real I/O to hash). The RNG is seeded from
the job-list filename, so parallel workers — each handed a different chunk — draw
independent streams and can't accidentally emit two identical files.
"""
import os
import random
import sys

CHUNK = 1 << 20  # 1 MiB write buffer


def main() -> None:
    joblist = sys.argv[1]
    rng = random.Random(os.path.basename(joblist))
    with open(joblist) as jobs:
        for line in jobs:
            line = line.rstrip("\n")
            if not line:
                continue
            size_str, path = line.split("\t", 1)
            remaining = int(size_str)
            with open(path, "wb") as out:
                while remaining > 0:
                    n = min(remaining, CHUNK)
                    out.write(rng.randbytes(n))
                    remaining -= n


if __name__ == "__main__":
    main()
