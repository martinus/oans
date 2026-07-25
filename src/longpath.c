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

#include "debug.h"
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
 * a dirfd for openat()/fstatat()/fdopendir(). Symlinks in the prefix are
 * followed, matching open()/stat(). Returns -1 with errno set on failure.
 *
 * openat() accepts a multi-component relative argument, so the walk advances by
 * the longest run of whole components that still fits one syscall argument
 * rather than one component per call: a ~4 KiB path costs 2-3 opens instead of
 * ~20, and the cost is O(len / PATH_MAX) instead of O(components).
 */
static int open_ancestor(const char *begin, const char *end)
{
	const char *p = begin;
	int dfd;

	dfd = open("/", O_PATH | O_DIRECTORY | O_CLOEXEC);
	if (dfd < 0)
		return -1;

	while (p < end) {
		char chunk[LONGPATH_MAXLEN + 1];
		const char *start, *fit;
		size_t chunklen;
		int next;

		while (p < end && *p == '/')
			p++;
		if (p >= end)
			break;

		/*
		 * Take the longest prefix of the remainder that fits one syscall
		 * argument and ends on a component boundary: either all of it, or
		 * up to the last '/' within the limit. Searching LONGPATH_MAXLEN
		 * + 1 bytes lets a boundary sitting exactly on the limit count.
		 * No '/' in range means a single component longer than any
		 * openat() would accept.
		 */
		start = p;
		if ((size_t)(end - start) <= LONGPATH_MAXLEN)
			fit = end;
		else
			fit = memrchr(start, '/', LONGPATH_MAXLEN + 1);

		if (!fit) {
			close_keep_errno(dfd);
			errno = ENAMETOOLONG;
			return -1;
		}

		chunklen = fit - start;
		memcpy(chunk, start, chunklen);
		chunk[chunklen] = '\0';
		p = fit;

		next = openat(dfd, chunk, O_PATH | O_DIRECTORY | O_CLOEXEC);
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

	/* Callers gate on !fits_one_syscall(), which is false for any relative
	 * path, so abspath starts with '/' and strrchr always finds one. */
	abort_on(!slash);

	/* A trailing slash leaves no final component to reach relative to a
	 * parent dirfd. */
	if (slash[1] == '\0') {
		errno = ENOTDIR;
		return -1;
	}
	*base_out = slash + 1;
	return open_ancestor(abspath, slash);
}

/* True when abspath can be handled by a single plain syscall (fits, or is a
 * relative path we cannot anchor a walk on - let the syscall report the error). */
static bool fits_one_syscall(const char *abspath)
{
	/* strnlen, not strlen: this runs per file (and per hashfile row during
	 * the prune), and the answer only depends on the first PATH_MAX bytes. */
	return strnlen(abspath, PATH_MAX) <= LONGPATH_MAXLEN || abspath[0] != '/';
}

/*
 * Open the final component of an over-PATH_MAX absolute path relative to its
 * parent directory. Shared by longpath_open() and longpath_opendir(); only
 * called once the fast path has been ruled out.
 */
static int openat_leaf(const char *abspath, int flags)
{
	const char *base;
	int dfd, fd;

	dfd = open_parent_dir(abspath, &base);
	if (dfd < 0)
		return -1;
	fd = openat(dfd, base, flags);
	close_keep_errno(dfd);
	return fd;
}

int longpath_open(const char *abspath, int flags)
{
	if (fits_one_syscall(abspath))
		return open(abspath, flags);

	return openat_leaf(abspath, flags);
}

DIR *longpath_opendir(const char *abspath)
{
	DIR *dirp;
	int fd;

	if (fits_one_syscall(abspath))
		return opendir(abspath);

	fd = openat_leaf(abspath, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fd < 0)
		return NULL;
	dirp = fdopendir(fd);
	if (!dirp)
		close_keep_errno(fd);
	return dirp;
}

/*
 * Shared body of longpath_stat()/longpath_lstat(): they differ only in the
 * AT_SYMLINK_NOFOLLOW flag. fstatat(AT_FDCWD, p, st, 0) is stat(p, st) and
 * fstatat(AT_FDCWD, p, st, AT_SYMLINK_NOFOLLOW) is lstat(p, st), so the fast
 * path collapses too.
 */
static int longpath_statat(const char *abspath, struct stat *st, int atflags)
{
	const char *base;
	int dfd, ret;

	if (fits_one_syscall(abspath))
		return fstatat(AT_FDCWD, abspath, st, atflags);

	dfd = open_parent_dir(abspath, &base);
	if (dfd < 0)
		return -1;
	ret = fstatat(dfd, base, st, atflags);
	close_keep_errno(dfd);
	return ret;
}

int longpath_stat(const char *abspath, struct stat *st)
{
	return longpath_statat(abspath, st, 0);
}

int longpath_lstat(const char *abspath, struct stat *st)
{
	return longpath_statat(abspath, st, AT_SYMLINK_NOFOLLOW);
}
