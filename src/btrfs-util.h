/*
 * btrfs-util.h
 *
 * Copyright (C) 2014 SUSE.  All rights reserved.
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

#ifndef	__BTRFS_UTIL__
#define	__BTRFS_UTIL__

#include <stdint.h>
#include <stdbool.h>
#include <uuid/uuid.h>

int lookup_btrfs_subvol(int fd, uint64_t *rootid);
int btrfs_get_fsuuid(int fd, uuid_t *uuid);

/*
 * Set *rdonly when fd's containing subvolume is read-only (a snapshot taken
 * with -r). Returns 0 on success, -1 on failure with *rdonly untouched.
 *
 * Uses BTRFS_IOC_GET_SUBVOL_INFO, which resolves the subvolume containing the
 * inode, rather than BTRFS_IOC_SUBVOL_GETFLAGS, which the kernel rejects with
 * EINVAL unless fd is the subvolume *root*. The walk meets ordinary
 * directories, so the latter would only ever answer for a path that happened
 * to be a subvolume root.
 */
int btrfs_subvol_is_readonly(int fd, bool *rdonly);
#endif	/* __BTRFS_UTIL__ */
