/*
 * longpath.c
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
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "longpath.h"

/*
 * The longest pathname the kernel accepts in a single syscall argument. PATH_MAX
 * counts the terminating NUL, so a usable path is at most PATH_MAX - 1 bytes;
 * anything longer gets ENAMETOOLONG and must be reached via the openat chain.
 */
#define LONGPATH_MAXLEN	(PATH_MAX - 1)

/* Close fd without disturbing errno (so a preceding failure's errno survives). */
static void close_keep_errno(int fd)
{
	int err = errno;

	close(fd);
	errno = err;
}

/*
 * Open the directory named by the range [begin, end) (an absolute path prefix,
 * possibly longer than PATH_MAX), returning an O_PATH directory fd suitable as
 * a dirfd for openat()/fstatat()/fdopendir(). Walks from "/" one component at a
 * time (each is bounded by NAME_MAX, well within a single syscall argument).
 * Symlinks in the prefix are followed, matching open()/stat(). Returns -1 with
 * errno set on failure.
 */
static int open_ancestor(const char *begin, const char *end)
{
	const char *p = begin;
	int dfd;

	dfd = open("/", O_PATH | O_DIRECTORY | O_CLOEXEC);
	if (dfd < 0)
		return -1;

	while (p < end) {
		char comp[NAME_MAX + 1];
		const char *start;
		size_t complen;
		int next;

		while (p < end && *p == '/')
			p++;
		if (p >= end)
			break;
		start = p;
		while (p < end && *p != '/')
			p++;
		complen = p - start;

		/* A lone component this long can never be opened. */
		if (complen > NAME_MAX) {
			close_keep_errno(dfd);
			errno = ENAMETOOLONG;
			return -1;
		}
		memcpy(comp, start, complen);
		comp[complen] = '\0';

		next = openat(dfd, comp, O_PATH | O_DIRECTORY | O_CLOEXEC);
		close_keep_errno(dfd);
		if (next < 0)
			return -1;
		dfd = next;
	}

	return dfd;
}

/*
 * Open the parent directory of abspath (absolute, possibly longer than
 * PATH_MAX) via open_ancestor() and set *base_out to its final component, so a
 * caller can reach the leaf with a single openat()/fstatat()/fdopendir(). Only
 * called for over-PATH_MAX absolute paths. Returns the parent dir fd, or -1
 * with errno set.
 */
static int open_parent_dir(const char *abspath, const char **base_out)
{
	const char *slash = strrchr(abspath, '/');
	const char *base = slash ? slash + 1 : NULL;

	/* No slash at all, or a trailing slash (no final component): there is
	 * no basename we can reach relative to a parent dirfd. */
	if (!base || *base == '\0') {
		errno = ENOTDIR;
		return -1;
	}
	*base_out = base;
	return open_ancestor(abspath, slash);
}

/* True when abspath can be handled by a single plain syscall (fits, or is a
 * relative path we cannot anchor a walk on - let the syscall report the error). */
static bool fits_one_syscall(const char *abspath)
{
	return strlen(abspath) <= LONGPATH_MAXLEN || abspath[0] != '/';
}

int longpath_open(const char *abspath, int flags)
{
	const char *base;
	int dfd, fd;

	if (fits_one_syscall(abspath))
		return open(abspath, flags);

	dfd = open_parent_dir(abspath, &base);
	if (dfd < 0)
		return -1;
	fd = openat(dfd, base, flags);
	close_keep_errno(dfd);
	return fd;
}

DIR *longpath_opendir(const char *abspath)
{
	const char *base;
	int dfd, fd;
	DIR *dirp;

	if (fits_one_syscall(abspath))
		return opendir(abspath);

	dfd = open_parent_dir(abspath, &base);
	if (dfd < 0)
		return NULL;
	fd = openat(dfd, base, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	close_keep_errno(dfd);
	if (fd < 0)
		return NULL;
	dirp = fdopendir(fd);
	if (!dirp)
		close_keep_errno(fd);
	return dirp;
}

int longpath_stat(const char *abspath, struct stat *st)
{
	const char *base;
	int dfd, ret;

	if (fits_one_syscall(abspath))
		return stat(abspath, st);

	dfd = open_parent_dir(abspath, &base);
	if (dfd < 0)
		return -1;
	ret = fstatat(dfd, base, st, 0);
	close_keep_errno(dfd);
	return ret;
}

int longpath_lstat(const char *abspath, struct stat *st)
{
	const char *base;
	int dfd, ret;

	if (fits_one_syscall(abspath))
		return lstat(abspath, st);

	dfd = open_parent_dir(abspath, &base);
	if (dfd < 0)
		return -1;
	ret = fstatat(dfd, base, st, AT_SYMLINK_NOFOLLOW);
	close_keep_errno(dfd);
	return ret;
}
