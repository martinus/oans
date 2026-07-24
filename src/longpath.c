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
#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "longpath.h"

/*
 * Open the directory named by the range [begin, end) (an absolute path prefix,
 * possibly longer than PATH_MAX), returning an O_PATH directory fd suitable as
 * a dirfd for openat()/fstatat(). Walks from "/" one chunk at a time, where a
 * chunk is the longest run of '/'-separated components whose joined length is
 * <= PATH_MAX (a single component is bounded by NAME_MAX, so a chunk always
 * fits). Symlinks in the prefix are followed, matching open()/stat() semantics.
 * Returns -1 with errno set on failure.
 */
static int open_ancestor(const char *begin, const char *end)
{
	char chunk[PATH_MAX + 1];
	size_t clen = 0;
	const char *p = begin;
	int dfd;

	dfd = open("/", O_PATH | O_DIRECTORY | O_CLOEXEC);
	if (dfd < 0)
		return -1;

	while (p < end) {
		const char *start;
		size_t complen;

		while (p < end && *p == '/')
			p++;
		if (p >= end)
			break;
		start = p;
		while (p < end && *p != '/')
			p++;
		complen = p - start;

		/* A lone component over PATH_MAX can never be opened. */
		if (complen > PATH_MAX) {
			close(dfd);
			errno = ENAMETOOLONG;
			return -1;
		}

		/* Flush the accumulated chunk if this component won't fit. */
		if (clen != 0 && clen + 1 + complen > PATH_MAX) {
			int next = openat(dfd, chunk,
					  O_PATH | O_DIRECTORY | O_CLOEXEC);
			int err = errno;

			close(dfd);
			if (next < 0) {
				errno = err;
				return -1;
			}
			dfd = next;
			clen = 0;
		}

		if (clen != 0)
			chunk[clen++] = '/';
		memcpy(chunk + clen, start, complen);
		clen += complen;
		chunk[clen] = '\0';
	}

	if (clen != 0) {
		int next = openat(dfd, chunk, O_PATH | O_DIRECTORY | O_CLOEXEC);
		int err = errno;

		close(dfd);
		if (next < 0) {
			errno = err;
			return -1;
		}
		dfd = next;
	}

	return dfd;
}

/*
 * Split abspath into its directory prefix and final component, open the prefix
 * via open_ancestor(), and hand the resulting dirfd to `act` (openat/fstatat).
 * Shared by longpath_open() and longpath_stat(). Returns act()'s result, or -1
 * (errno set) if the prefix could not be opened. Only reached for paths that
 * are absolute and longer than PATH_MAX.
 */
static int with_parent_dirfd(const char *abspath,
			     int (*act)(int dirfd, const char *base, void *arg),
			     void *arg)
{
	const char *slash = strrchr(abspath, '/');
	const char *base = slash ? slash + 1 : NULL;
	int dfd, ret, err;

	/* No slash at all, or a trailing slash (no final component): there is
	 * no basename we can reach relative to a parent dirfd. */
	if (!base || *base == '\0') {
		errno = ENOTDIR;
		return -1;
	}

	dfd = open_ancestor(abspath, slash);
	if (dfd < 0)
		return -1;

	ret = act(dfd, base, arg);
	err = errno;
	close(dfd);
	if (ret < 0)
		errno = err;
	return ret;
}

static int act_open(int dirfd, const char *base, void *arg)
{
	int flags = *(int *)arg;

	return openat(dirfd, base, flags);
}

static int act_stat(int dirfd, const char *base, void *arg)
{
	return fstatat(dirfd, base, (struct stat *)arg, 0);
}

int longpath_open(const char *abspath, int flags)
{
	/* Fast path: fits in a single syscall argument. */
	if (strlen(abspath) <= PATH_MAX)
		return open(abspath, flags);

	/* Only an absolute path can be reached by walking from "/". A relative
	 * path this long has no anchor; let open() report ENAMETOOLONG. */
	if (abspath[0] != '/')
		return open(abspath, flags);

	return with_parent_dirfd(abspath, act_open, &flags);
}

int longpath_stat(const char *abspath, struct stat *st)
{
	if (strlen(abspath) <= PATH_MAX)
		return stat(abspath, st);

	if (abspath[0] != '/')
		return stat(abspath, st);

	return with_parent_dirfd(abspath, act_stat, st);
}
