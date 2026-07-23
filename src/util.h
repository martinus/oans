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
 * Copy `in` to `out` (up to out_sz bytes, always NUL-terminated), replacing
 * terminal control characters with '?': C0 controls (< 0x20), DEL (0x7f), and
 * the two-byte UTF-8 encodings of the C1 controls (U+0080..U+009F). Some
 * terminals act on these when a filename is printed verbatim (#353).
 */
void sanitize_ctrl(const char *in, char *out, size_t out_sz);

#endif	/* __UTIL_H__ */
