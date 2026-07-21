/*
 * storage.c
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

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/sysmacros.h>
#include <linux/magic.h>
#include <linux/btrfs.h>

#include "storage.h"

/*
 * Read /sys .../queue/rotational for a block device. `dev` is a device node's
 * st_rdev (btrfs member) or a file's st_dev (single-device fs). Returns 0 and
 * sets *rot on success, or a negative errno. Whole disks answer directly;
 * partitions have no queue/ of their own, so we retry via the parent ("..",
 * which resolves against the symlink target's real directory).
 */
static int read_rotational(dev_t dev, bool *rot)
{
	static const char *const forms[] = {
		"/sys/dev/block/%u:%u/queue/rotational",
		"/sys/dev/block/%u:%u/../queue/rotational",
	};
	char path[64];
	unsigned maj = major(dev), min = minor(dev);

	for (size_t i = 0; i < sizeof(forms) / sizeof(forms[0]); i++) {
		char c;
		int fd, n;

		snprintf(path, sizeof(path), forms[i], maj, min);
		fd = open(path, O_RDONLY | O_CLOEXEC);
		if (fd < 0)
			continue;

		n = read(fd, &c, 1);
		close(fd);
		if (n == 1) {
			*rot = (c != '0');
			return 0;
		}
	}
	return -ENOENT;
}

/* Merge one device's rotational reading into the aggregate profile. */
static void fold_rotational(struct storage_profile *p, dev_t dev)
{
	bool rot;

	if (read_rotational(dev, &rot) == 0) {
		p->rotational_known = true;
		if (rot)
			p->rotational = true;	/* any spinner -> treat as HDD */
	}
}

/*
 * A btrfs pool's st_dev is an anonymous bdev with no /sys/block entry, so the
 * member devices are enumerated over the ioctl interface instead.
 */
static void detect_btrfs(int fd, struct storage_profile *p)
{
	struct btrfs_ioctl_fs_info_args fi = {0};
	uint64_t id;

	if (ioctl(fd, BTRFS_IOC_FS_INFO, &fi) != 0)
		return;

	if (fi.num_devices)
		p->num_devices = fi.num_devices;

	/* devids are 1..max_id and may be sparse (removed devices) -> skip gaps. */
	for (id = 1; id <= fi.max_id; id++) {
		struct btrfs_ioctl_dev_info_args di = {0};
		struct stat st;

		di.devid = id;
		if (ioctl(fd, BTRFS_IOC_DEV_INFO, &di) != 0)
			continue;
		if (di.path[0] == '\0')
			continue;
		if (stat((const char *)di.path, &st) != 0 || !S_ISBLK(st.st_mode))
			continue;

		fold_rotational(p, st.st_rdev);
	}
}

int storage_detect(const char *path, struct storage_profile *p)
{
	struct statfs sfs;
	struct stat st;
	int fd;

	memset(p, 0, sizeof(*p));
	p->num_devices = 1;

	if (stat(path, &st) != 0)
		return -errno;

	if (statfs(path, &sfs) == 0 && sfs.f_type == BTRFS_SUPER_MAGIC) {
		fd = open(path, O_RDONLY | O_CLOEXEC);
		if (fd >= 0) {
			detect_btrfs(fd, p);
			close(fd);
			return 0;
		}
		/* fall through: still report it as a single-device guess */
	}

	/* Single-device filesystem: the file's st_dev is the block device. */
	fold_rotational(p, st.st_dev);
	return 0;
}

unsigned int storage_recommend_io_threads(const struct storage_profile *p,
					  unsigned int ncpus)
{
	unsigned int base = ncpus < AUTO_THREADS_CAP ? ncpus : AUTO_THREADS_CAP;

	if (base < 1)
		base = 1;

	/* Unknown or non-rotational (SSD/NVMe): keep the established default. */
	if (!p->rotational_known || !p->rotational)
		return base;

	/*
	 * Spinning disks are seek-bound: many concurrent readers thrash the
	 * heads. A single HDD wants only a couple of readers; a multi-device
	 * pool stripes/mirrors across spindles, so scale ~2 readers per device,
	 * still bounded by the CPU-count cap above.
	 */
	if (p->num_devices <= 1)
		return base < 4 ? base : 4;

	unsigned int want = 2 * p->num_devices;

	return want < base ? want : base;
}

bool storage_benefits_from_concurrency(const struct storage_profile *p)
{
	/*
	 * Concurrent reads to one file raise throughput only where the media
	 * isn't seek-bound: SSD/NVMe, a multi-device pool (independent spindles),
	 * or unknown media (assume SSD-like, as the io-threads heuristic does). A
	 * single spinning disk just thrashes its heads.
	 */
	return !p->rotational_known || !p->rotational || p->num_devices > 1;
}

void storage_describe(const struct storage_profile *p, char *buf, size_t len)
{
	const char *disk = !p->rotational_known ? "unknown media" :
			   p->rotational ? "rotational (HDD)" :
					   "non-rotational (SSD)";

	if (p->num_devices > 1)
		snprintf(buf, len, "btrfs pool of %u devices, %s",
			 p->num_devices, disk);
	else
		snprintf(buf, len, "single device, %s", disk);
}
