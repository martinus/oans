/*
 * storage.h
 *
 * Best-effort detection of the backing storage of a scan target, so oans can
 * pick I/O concurrency that suits the device: spinning disks want few
 * concurrent readers (seeks dominate), SSDs want more, and a multi-device
 * btrfs pool wants roughly one reader per spindle.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */

#ifndef	__STORAGE_H__
#define	__STORAGE_H__

#include <stdbool.h>

/*
 * Upper bound for any auto-detected worker-thread count. On btrfs the walk
 * plateaus at ~8 threads on metadata b-tree lock contention regardless of the
 * disk, so more just burns cores. Both the io-threads recommendation and the
 * cpu-threads default are capped here; an explicit --io-threads/--cpu-threads
 * overrides it.
 */
#define AUTO_THREADS_CAP	8

struct storage_profile {
	/*
	 * True if any backing device is a spinning disk. Conservative: on a
	 * mixed pool one rotational member makes the whole profile rotational,
	 * because the slowest spindle sets the seek-bound ceiling.
	 */
	bool		rotational;
	/* False when we could not read rotational state for any device. */
	bool		rotational_known;
	/* Backing block devices: btrfs pool member count, else 1. */
	unsigned int	num_devices;
};

/*
 * Fill *p for the filesystem containing `path`. Always succeeds in the sense
 * that *p is fully initialised; check p->rotational_known before trusting
 * p->rotational. Returns 0, or a negative errno if `path` could not be
 * inspected at all (p is still zeroed/defaulted in that case).
 */
int storage_detect(const char *path, struct storage_profile *p);

/*
 * Recommend a default --io-threads for this profile and CPU count. Pure, so
 * it is unit-tested directly (see tests.c). The heuristic is deliberately
 * conservative; `--autotune` measures the real optimum on the target hardware.
 */
unsigned int storage_recommend_io_threads(const struct storage_profile *p,
					  unsigned int ncpus);

/* One-line human summary, e.g. "btrfs pool of 4 devices, rotational (HDD)". */
void storage_describe(const struct storage_profile *p, char *buf, size_t len);

#endif	/* __STORAGE_H__ */
