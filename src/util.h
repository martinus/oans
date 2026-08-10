/*
 * util.h
 *
 * Copyright (C) 2016 SUSE.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * Authors: Mark Fasheh <mfasheh@suse.de>
 */

#ifndef	__UTIL_H__
#define	__UTIL_H__

#include <stdlib.h>
#include <stdbool.h>
#include <dirent.h>
#include <stdint.h>
#include <sys/time.h>
#include <unistd.h>
#include <uuid/uuid.h>

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

/*
 * Code for parsing and printing human readable numbers is taken from
 * btrfs-progs/util.c and modified locally to suit my purposes.
 */
uint64_t parse_size(char *s);
int pretty_size_snprintf(uint64_t size, char *str, size_t str_bytes);
#define pretty_size(size) 						\
	({								\
		static __thread char _str[32];				\
		(void)pretty_size_snprintf((size), _str, sizeof(_str));	\
		_str;							\
	})

/* Human-readable size (KiB/MiB/...); pretty_size prints raw bytes. */
int human_size_snprintf(uint64_t size, char *str, size_t str_bytes);
#define human_size(size)						\
	({								\
		static __thread char _hs[32];				\
		(void)human_size_snprintf((size), _hs, sizeof(_hs));	\
		_hs;							\
	})

/* Group digits in threes with a ',' separator: 2505166 -> "2,505,166". */
int group_u64_snprintf(uint64_t n, char *str, size_t str_bytes);
#define group_u64(n)							\
	({								\
		static __thread char _gu[28];				\
		(void)group_u64_snprintf((n), _gu, sizeof(_gu));	\
		_gu;							\
	})

/* Compact human-readable duration ("2m14s"). */
int human_duration_snprintf(double seconds, char *str, size_t str_bytes);
#define human_duration(secs)						\
	({								\
		static __thread char _hd[16];				\
		(void)human_duration_snprintf((secs), _hd, sizeof(_hd));\
		_hd;							\
	})

/*
 * ANSI colors for status output. color_init() decides once whether to emit
 * escapes (stdout is a tty, NO_COLOR unset, and colors not disabled); the
 * strings below are empty when color is off, so they are always safe to print.
 */
extern const char *col_reset, *col_bold, *col_dim;
extern const char *col_red, *col_green, *col_yellow, *col_blue, *col_cyan;
extern const char *col_magenta;
void color_init(bool disable);

/* Monotonic seconds since the process recorded its start (see start_timer). */
void start_timer(void);
double elapsed_seconds(void);

/* Monotonic wall-clock nanoseconds, for ad-hoc interval timing. */
uint64_t mono_ns(void);

int num_digits(unsigned long long num);

/* Online logical CPU count (at least 1). */
unsigned int get_num_cpus(void);

/* Bump up maximum open file limit. */
int increase_limits(void);

/* qsort/bsearch over a plain uint64_t array. */
static inline int cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;

	return (x > y) - (x < y);
}

#define _cleanup_(x) __attribute__((cleanup(x)))
static inline void freep(void *p)
{
	free(*(void**) p);
}

static inline void closedirectory(DIR **p)
{
	if (*p)
		closedir(*p);
}

static inline void closefd(int *fd)
{
	if (*fd >= 0)
		close(*fd);
}

void debug_print_uuid(uuid_t uuid);

/*
 * Which bytes must never reach a terminal - the single definition of that
 * policy, so it cannot drift between output formats. Returns how many input
 * bytes the dangerous sequence occupies (1 for a C0 control or DEL, 2 for a C1
 * control, which UTF-8 spells in two) and stores its code point in *cp; 0 for a
 * byte that is safe to pass through, leaving *cp untouched.
 *
 * Every escaper renders the result its own way - sanitize_ctrl() as `\xNN`,
 * --json as `\u00NN` - but they all ask this one question. Reading p[1] is safe
 * on a NUL-terminated string: the terminator is not a C1 continuation byte.
 */
size_t ctrl_seq_len(const unsigned char *p, unsigned char *cp);

/* True if `s` holds anything ctrl_seq_len() would flag - which no ordinary
 * name does, so this is the fast path out of every escaper below. */
bool has_ctrl(const char *s);

/*
 * Copy `in` to `out` (up to out_sz bytes, always NUL-terminated), escaping the
 * bytes a terminal would act on rather than print. Tab, newline and carriage
 * return become `\t`, `\n`, `\r`; everything else ctrl_seq_len() flags becomes
 * `\xNN`. Valid multi-byte UTF-8 passes through untouched.
 *
 * File names are untrusted input - anyone who can create a file inside a
 * scanned tree chooses bytes that reach the administrator's terminal - and a
 * crafted name can corrupt the display, overwrite the line above it with a
 * `\r`, or spoof output entirely (#353, #202). So a name is escaped wherever it
 * is printed, tty or not: machine consumers use --json.
 *
 * The escape is for reading, not for round-tripping: a backslash in the name
 * itself is passed through, so `\x07` in the output may be either the byte or
 * those four characters. Nothing here is meant to be parsed back into a path;
 * a name that needs no escaping is unchanged, which is every real one.
 *
 * An escape is never split across the end of the buffer: `out` is truncated at
 * the last complete one that fit.
 */
void sanitize_ctrl(const char *in, char *out, size_t out_sz);

/* Longest output sanitize_ctrl() can produce for one input byte ("\xNN"). */
#define SANITIZE_CTRL_MAX	4

/*
 * sanitize_ctrl() into a freshly allocated string sized to hold the whole
 * escaped path, for the print sites that have no buffer to spare and must not
 * truncate - a path may exceed PATH_MAX (#117), and a truncated one names a
 * different file. Returns NULL only if the allocation fails.
 */
char *path_for_display(const char *path);

/*
 * Declare `var` as a display-safe view of the path `p`, valid to the end of the
 * enclosing block:
 *
 *	declare_display_path(dpath, path);
 *	eprintf("cannot open %s\n", dpath);
 *
 * A *declaration* macro, in the shape of declare_alloc_tracking() - deliberately
 * not a statement expression, since `({ ... })` would end the cleanup scope and
 * free the string before the printf could read it. So it has to sit where a
 * declaration may, which every print site already is.
 *
 * `p` is evaluated twice; pass a plain lvalue. If the allocation fails `var` is
 * the unescaped path - a message the admin can act on beats no message, and the
 * memory to do better is already gone.
 */
#define declare_display_path(var, p)					\
	_cleanup_(freep) char *var##_escaped = path_for_display(p);	\
	const char *var = var##_escaped ? var##_escaped : (p)

#endif	/* __UTIL_H__ */
