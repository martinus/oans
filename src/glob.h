/*
 * glob.h
 *
 * gitignore-style path matching for --exclude.
 *
 * The syntax is the one git, ripgrep and fd use, because that is what users
 * reach for. The rules, in full:
 *
 *   - A pattern with no '/' matches the **basename** at any depth, so
 *     `@eaDir`, `node_modules` and `*.iso` do the obvious thing. (The old
 *     fnmatch-against-the-full-path behaviour made these match nothing.)
 *   - A pattern starting with '/' is an absolute path, anchored at the
 *     filesystem root, e.g. `/srv/media/cache*`.
 *   - A pattern with a '/' anywhere else matches at any depth, i.e. it is
 *     implicitly prefixed with `**` + '/': `Steam/temp` matches
 *     `/data/Steam/temp` and `/home/u/Steam/temp`. (gitignore anchors these to
 *     the ignore file's directory; there is no ignore file here, so any-depth
 *     is the closest useful analogue - write a leading '/' to anchor.)
 *   - A trailing '/' restricts the pattern to directories.
 *   - `*` matches any run of characters except '/', `?` matches one non-'/'
 *     character, `[abc]` / `[a-z]` / `[!a-z]` are character classes, and `**`
 *     crosses directory boundaries.
 *
 * Excluding a directory prunes the walk there, so a pattern naming a directory
 * also drops everything under it; you need not add a wildcard for its
 * contents.
 *
 * Negation (`!`) is deliberately not supported.
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

#ifndef	__GLOB_H__
#define	__GLOB_H__

#include <stdbool.h>

struct glob_set;

struct glob_set *glob_set_new(void);

/*
 * Add a gitignore-style pattern. Returns 0 on success, non-zero if the pattern
 * is malformed (an unterminated character class) or on allocation failure; on
 * failure *err is set to a message the caller owns and must free.
 */
int glob_set_add(struct glob_set *gs, const char *pattern, char **err);

/*
 * Add a path that must match exactly, with no metacharacter interpretation.
 * For paths oans excludes on the user's behalf (the hashfile and its WAL
 * sidecars), which may legitimately contain '*' or '['.
 */
int glob_set_add_literal(struct glob_set *gs, const char *path);

/*
 * Compile the added patterns into the matching automaton. Must be called after
 * the last add and before the first match. Returns 0 on success; on failure
 * *err is set to a message the caller owns and must free.
 */
int glob_set_compile(struct glob_set *gs, char **err);

/* True when no pattern has been added (match always returns false). */
bool glob_set_empty(const struct glob_set *gs);

/*
 * True if `path` (absolute) is matched. `is_dir` selects whether trailing-'/'
 * directory-only patterns apply. When it matches and `which` is non-NULL,
 * *which is set to the matching pattern, owned by the set, for diagnostics.
 */
bool glob_set_match(struct glob_set *gs, const char *path, bool is_dir,
		    const char **which);

/*
 * Number of paths a pattern has matched so far, indexed over the *user's*
 * patterns in add order, so callers can report a pattern that never matched
 * anything. Entries added via glob_set_add_literal() are oans's own and are
 * skipped. Returns false once `i` runs past the end.
 */
bool glob_set_stat(const struct glob_set *gs, unsigned int i,
		   const char **pattern, unsigned long *matches);

void glob_set_free(struct glob_set *gs);

#endif	/* __GLOB_H__ */
