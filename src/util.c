/*
 * util.c
 *
 * Copyright (C) 2014 SUSE except where noted.  All rights reserved.
 *
 * Code taken from btrfs-progs/util.c is:
 * Copyright (C) 2007 Oracle.  All rights reserved.
 * Copyright (C) 2008 Morey Roof.  All rights reserved.

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
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <inttypes.h>
#include <time.h>
#ifdef __GLIBC__
#include <execinfo.h>
#endif
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/types.h>
#include <regex.h>
#include <dirent.h>
#include <unistd.h>
#include <limits.h>
#include <errno.h>

#include "debug.h"
#include "util.h"

const char *col_reset = "", *col_bold = "", *col_dim = "";
const char *col_red = "", *col_green = "", *col_yellow = "";
const char *col_blue = "", *col_cyan = "";
const char *col_magenta = "";

void color_init(bool disable)
{
	if (disable || !isatty(STDOUT_FILENO) || getenv("NO_COLOR"))
		return;	/* leave every color string empty */

	col_reset = "\033[0m"; col_bold = "\033[1m"; col_dim = "\033[2m";
	col_red = "\033[31m"; col_green = "\033[32m"; col_yellow = "\033[33m";
	col_blue = "\033[34m"; col_cyan = "\033[36m"; col_magenta = "\033[35m";
}

static struct timespec timer_start;

void start_timer(void)
{
	clock_gettime(CLOCK_MONOTONIC, &timer_start);
}

double elapsed_seconds(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (now.tv_sec - timer_start.tv_sec) +
	       (now.tv_nsec - timer_start.tv_nsec) / 1e9;
}

uint64_t mono_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

/* Compact human-readable duration: "45s", "2m14s", "1h03m". */
int human_duration_snprintf(double seconds, char *str, size_t str_bytes)
{
	unsigned long s = (unsigned long)(seconds + 0.5);

	if (str_bytes == 0)
		return 0;
	if (s < 60)
		return snprintf(str, str_bytes, "%lus", s);
	if (s < 3600)
		return snprintf(str, str_bytes, "%lum%02lus", s / 60, s % 60);
	return snprintf(str, str_bytes, "%luh%02lum", s / 3600, (s % 3600) / 60);
}

/* Human-readable size (KiB/MiB/...); pretty_size_snprintf prints raw bytes. */
int human_size_snprintf(uint64_t size, char *str, size_t str_bytes)
{
	static const char * const units[] = { "B", "KiB", "MiB", "GiB",
					      "TiB", "PiB", "EiB" };
	unsigned int u = 0;
	double v = (double)size;

	if (str_bytes == 0)
		return 0;
	while (v >= 1024.0 && u < ARRAY_SIZE(units) - 1) {
		v /= 1024.0;
		u++;
	}
	if (u == 0)
		return snprintf(str, str_bytes, "%"PRIu64" B", size);
	return snprintf(str, str_bytes, "%.1f %s", v, units[u]);
}

uint64_t parse_size(char *s)
{
	int i;
	char c;
	uint64_t mult = 1;

	for (i = 0; s && s[i] && isdigit(s[i]); i++) ;
	if (!i) {
		eprintf("ERROR: size value is empty\n");
		exit(50);
	}

	if (s[i]) {
		c = tolower(s[i]);
		switch (c) {
		case 'e':
			mult *= 1024;
			/* fallthrough */
		case 'p':
			mult *= 1024;
			/* fallthrough */
		case 't':
			mult *= 1024;
			/* fallthrough */
		case 'g':
		case 'G':
			mult *= 1024;
			/* fallthrough */
		case 'm':
		case 'M':
			mult *= 1024;
			/* fallthrough */
		case 'k':
		case 'K':
			mult *= 1024;
			/* fallthrough */
		case 'b':
			break;
		default:
			eprintf("ERROR: Unknown size descriptor "
				"'%c'\n", c);
			exit(1);
		}
	}
	if (s[i] && s[i+1]) {
		eprintf("ERROR: Illegal suffix contains "
			"character '%c' in wrong position\n",
			s[i+1]);
		exit(51);
	}
	return strtoull(s, NULL, 10) * mult;
}

int pretty_size_snprintf(uint64_t size, char *str, size_t str_bytes)
{
	return snprintf(str, str_bytes, "%"PRIu64, size);
}

void print_stack_trace(void)
{
	void *trace[16];
	char **messages = (char **)NULL;
	int i, trace_size = 0;

#ifdef __GLIBC__
	trace_size = backtrace(trace, 16);
	messages = backtrace_symbols(trace, trace_size);
	printf("[stack trace follows]\n");
	for (i=0; i < trace_size; i++)
		printf("%s\n", messages[i]);
	free(messages);
#endif
}

int num_digits(unsigned long long num)
{
	unsigned int digits = 0;

	while (num) {
		num /= 10;
		digits++;
	}
	return digits;
}

unsigned int get_num_cpus(void)
{
	long n = sysconf(_SC_NPROCESSORS_ONLN);

	if (n < 1)
		n = 1;
	dprintf("Detected %ld cpus.\n", n);
	return n;
}

int increase_limits(void) {
	struct rlimit cur_r;
	struct rlimit new_r;
	int ret;

	ret = getrlimit(RLIMIT_NOFILE, &cur_r);
	if (ret < 0)
		return -errno;

	new_r.rlim_cur = cur_r.rlim_max;
	new_r.rlim_max = cur_r.rlim_max;
	ret = setrlimit(RLIMIT_NOFILE, &new_r);

	if (ret < 0)
		return -errno;

	vprintf("Increased open file limit from %llu to %llu.\n",
		(unsigned long long)cur_r.rlim_cur,
		(unsigned long long)new_r.rlim_cur);
	return 0;
}

void debug_print_uuid(uuid_t uuid)
{
	char buf[37];
	uuid_unparse(uuid, buf);
	eprintf("%s", buf);
}

void sanitize_ctrl(const char *in, char *out, size_t out_sz)
{
	const unsigned char *p = (const unsigned char *)in;
	size_t o = 0;

	if (out_sz == 0)
		return;

	while (*p && o + 1 < out_sz) {
		if (*p < 0x20 || *p == 0x7f) {
			out[o++] = '?';
			p++;
		} else if (p[0] == 0xc2 && p[1] >= 0x80 && p[1] <= 0x9f) {
			out[o++] = '?';
			p += 2;
		} else {
			out[o++] = *p++;
		}
	}
	out[o] = '\0';
}
