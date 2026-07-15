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

/* controlled by user options, turns pretty print on if true. */
extern int human_readable;

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

/* Always human-readable (KiB/MiB/...), regardless of the --human option. */
int human_size_snprintf(uint64_t size, char *str, size_t str_bytes);
#define human_size(size)						\
	({								\
		static __thread char _hs[32];				\
		(void)human_size_snprintf((size), _hs, sizeof(_hs));	\
		_hs;							\
	})

/*
 * ANSI colors for status output. color_init() decides once whether to emit
 * escapes (stdout is a tty, NO_COLOR unset, and colors not disabled); the
 * strings below are empty when color is off, so they are always safe to print.
 */
extern const char *col_reset, *col_bold, *col_dim;
extern const char *col_red, *col_green, *col_yellow, *col_blue, *col_cyan;
void color_init(bool disable);

/* Monotonic seconds since the process recorded its start (see start_timer). */
void start_timer(void);
double elapsed_seconds(void);

/* Trivial wrapper around gettimeofday */
struct elapsed_time {
	struct timeval	start;
	struct timeval	end;
	const char	*name;
	double		elapsed;
};
void record_start(struct elapsed_time *e, const char *name);
void record_end_print(struct elapsed_time *e);

int num_digits(unsigned long long num);

void get_num_cpus(unsigned int *nr_phys, unsigned int *nr_log);

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

#endif	/* __UTIL_H__ */
