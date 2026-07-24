/*
 * longpath.h
 *
 * Open and stat files whose absolute path exceeds PATH_MAX (4096). The kernel
 * rejects any single open()/statx()/stat() argument longer than PATH_MAX with
 * ENAMETOOLONG, so the only way to reach such a file is to open a reachable
 * ancestor and openat-walk the remaining components, keeping every individual
 * argument within PATH_MAX. These helpers encapsulate that walk; see issue
 * #117 (follow-up to #108/#115).
 *
 * Each helper takes the plain open()/stat()/opendir() fast path internally when
 * the path fits, so ordinary-length paths cost nothing extra and callers never
 * need to gate on length themselves.
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

#ifndef	__LONGPATH_H__
#define	__LONGPATH_H__

#include <dirent.h>
#include <sys/stat.h>

/*
 * Like open(abspath, flags), but tolerates strlen(abspath) > PATH_MAX by
 * opening a reachable ancestor and openat-walking the rest (each argument kept
 * <= PATH_MAX). abspath must be absolute; symlinks are followed exactly as
 * open() would (pass O_NOFOLLOW in flags to change that for the final
 * component). Returns an fd on success or -1 with errno set. For paths that fit
 * in PATH_MAX this is exactly open(abspath, flags).
 */
int longpath_open(const char *abspath, int flags);

/* Like opendir(abspath), tolerating strlen(abspath) > PATH_MAX. NULL/errno on
 * failure. For in-range paths this is exactly opendir(abspath). */
DIR *longpath_opendir(const char *abspath);

/*
 * Like stat(abspath, st), but tolerates strlen(abspath) > PATH_MAX (opens the
 * parent directory via the same ancestor walk, then fstatat(dirfd, basename)).
 * Follows symlinks like stat(). Returns 0 on success or -1 with errno set.
 */
int longpath_stat(const char *abspath, struct stat *st);

/* Like lstat(abspath, st) (does not follow a final symlink), tolerating
 * strlen(abspath) > PATH_MAX. Returns 0 on success or -1 with errno set. */
int longpath_lstat(const char *abspath, struct stat *st);

#endif	/* __LONGPATH_H__ */
